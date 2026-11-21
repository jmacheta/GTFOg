// Bluetooth LE peripheral: the GTFOg control service, Battery Service and Device Information Service.
#include "accel.hpp"
#include "app.hpp"

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/services/bas.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

#include <array>
#include <atomic>
#include <cstring>
#include <tuple>
#include <utility>

LOG_MODULE_REGISTER(ble, LOG_LEVEL_INF);

namespace {

#define GTFOG_UUID(n) BT_UUID_INIT_128(BT_UUID_128_ENCODE(0xa7f00000 + (n), 0x5c1e, 0x4b8e, 0x9d2a, 0x6f0e1c3b4d50))

bt_uuid_128 const service_uuid = GTFOG_UUID(1);
bt_uuid_128 const fan_uuid = GTFOG_UUID(2);
bt_uuid_128 const strobe_uuid = GTFOG_UUID(3);
bt_uuid_128 const accel_uuid = GTFOG_UUID(4);
bt_uuid_128 const charger_uuid = GTFOG_UUID(5);
bt_uuid_128 const settings_uuid = GTFOG_UUID(6);
constexpr char settings_key[] = "gtfog/cfg";

std::atomic<std::uint8_t> charger{0};
bt_conn *current_conn; // ponytail: one connection at a time (CONFIG_BT_MAX_CONN=1).

auto read_fan(bt_conn *conn, bt_gatt_attr const *attr, void *buf, std::uint16_t len, std::uint16_t offset) -> ssize_t {
  auto value = with_controller([](gtfog::controller &c) { return static_cast<std::uint8_t>(c.fan_speed()); });
  return bt_gatt_attr_read(conn, attr, buf, len, offset, &value, sizeof(value));
}

auto write_fan(bt_conn *, bt_gatt_attr const *, void const *buf, std::uint16_t len, std::uint16_t offset, std::uint8_t) -> ssize_t {
  if(offset != 0 || len != 1) {
    return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
  }
  auto value = *static_cast<std::uint8_t const *>(buf);
  if(value > 100) {
    return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
  }
  with_controller([&](gtfog::controller &c) { c.set_fan_speed(value); });
  return len;
}

auto read_strobe(bt_conn *conn, bt_gatt_attr const *attr, void *buf, std::uint16_t len, std::uint16_t offset) -> ssize_t {
  std::uint8_t value = with_controller([](gtfog::controller &c) { return c.strobe(); });
  return bt_gatt_attr_read(conn, attr, buf, len, offset, &value, sizeof(value));
}

auto write_strobe(bt_conn *, bt_gatt_attr const *, void const *buf, std::uint16_t len, std::uint16_t offset, std::uint8_t) -> ssize_t {
  if(offset != 0 || len != 1) {
    return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
  }
  auto value = *static_cast<std::uint8_t const *>(buf);
  if(value > 1) {
    return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
  }
  with_controller([&](gtfog::controller &c) { c.set_strobe(value != 0); });
  return len;
}

auto read_charger(bt_conn *conn, bt_gatt_attr const *attr, void *buf, std::uint16_t len, std::uint16_t offset) -> ssize_t {
  std::uint8_t value = charger;
  return bt_gatt_attr_read(conn, attr, buf, len, offset, &value, sizeof(value));
}

auto read_settings(bt_conn *conn, bt_gatt_attr const *attr, void *buf, std::uint16_t len, std::uint16_t offset) -> ssize_t {
  auto value = gtfog::encode(with_controller([](gtfog::controller &c) { return c.settings(); }));
  return bt_gatt_attr_read(conn, attr, buf, len, offset, value.data(), value.size());
}

auto write_settings(bt_conn *, bt_gatt_attr const *, void const *buf, std::uint16_t len, std::uint16_t offset, std::uint8_t) -> ssize_t {
  auto settings = gtfog::decode_settings({static_cast<std::uint8_t const *>(buf), len});
  if(offset != 0 || !settings) {
    return BT_GATT_ERR(len != gtfog::settings_size ? BT_ATT_ERR_INVALID_ATTRIBUTE_LEN : BT_ATT_ERR_VALUE_NOT_ALLOWED);
  }
  with_controller([&](gtfog::controller &c) { c.configure(*settings); });
  if(int err = settings_save_one(settings_key, buf, len); err != 0) {
    LOG_ERR("save settings: %d", err);
    return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
  }
  return len;
}

auto load_settings(char const *key, std::size_t len, settings_read_cb read, void *arg) -> int {
  std::array<std::uint8_t, gtfog::settings_size> bytes{};
  if(std::strcmp(key, "cfg") != 0 || len != bytes.size() || read(arg, bytes.data(), bytes.size()) != static_cast<ssize_t>(bytes.size())) {
    return 0; // Unknown or damaged entries keep the defaults.
  }
  if(auto settings = gtfog::decode_settings(bytes)) {
    with_controller([&](gtfog::controller &c) { c.configure(*settings); });
  }
  return 0;
}

void accel_subscribed(bt_gatt_attr const *, std::uint16_t value) { accelerometer_stream(value == BT_GATT_CCC_NOTIFY); }

constexpr auto rw_notify = BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE | BT_GATT_CHRC_NOTIFY;
constexpr auto encrypted = BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT;

} // namespace

BT_GATT_SERVICE_DEFINE(
    gtfog_service, BT_GATT_PRIMARY_SERVICE(&service_uuid), BT_GATT_CHARACTERISTIC(&fan_uuid.uuid, rw_notify, encrypted, read_fan, write_fan, nullptr),
    BT_GATT_CCC(nullptr, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE_ENCRYPT),
    BT_GATT_CHARACTERISTIC(&strobe_uuid.uuid, rw_notify, encrypted, read_strobe, write_strobe, nullptr),
    BT_GATT_CCC(nullptr, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE_ENCRYPT),
    BT_GATT_CHARACTERISTIC(&accel_uuid.uuid, BT_GATT_CHRC_NOTIFY, BT_GATT_PERM_NONE, nullptr, nullptr, nullptr),
    BT_GATT_CCC(accel_subscribed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE_ENCRYPT),
    BT_GATT_CHARACTERISTIC(&charger_uuid.uuid, BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY, BT_GATT_PERM_READ_ENCRYPT, read_charger, nullptr, nullptr),
    BT_GATT_CCC(nullptr, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE_ENCRYPT),
    BT_GATT_CHARACTERISTIC(&settings_uuid.uuid, BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE, encrypted, read_settings, write_settings, nullptr), );

SETTINGS_STATIC_HANDLER_DEFINE(gtfog, "gtfog", nullptr, load_settings, nullptr, nullptr);

namespace {

// Attribute indices in gtfog_service: 0 service, 1 + 3k characteristic declaration, 2 + 3k value, 3 + 3k CCC.
constexpr auto fan_value = 2;
constexpr auto strobe_value = 5;
constexpr auto accel_value = 8;
constexpr auto charger_value = 11;

std::uint8_t const flags[] = {BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR};
std::uint8_t const service_uuid_bytes[] = {BT_UUID_128_ENCODE(0xa7f00001, 0x5c1e, 0x4b8e, 0x9d2a, 0x6f0e1c3b4d50)};

bt_data const advertising[] = {
    {BT_DATA_FLAGS, sizeof(flags), flags},
    {BT_DATA_UUID128_ALL, sizeof(service_uuid_bytes), service_uuid_bytes},
};
bt_data const scan_response[] = {
    {BT_DATA_NAME_COMPLETE, sizeof(CONFIG_BT_DEVICE_NAME) - 1, reinterpret_cast<std::uint8_t const *>(CONFIG_BT_DEVICE_NAME)},
};

void start_advertising(k_work *) {
  bt_le_adv_param param = BT_LE_ADV_PARAM_INIT(BT_LE_ADV_OPT_CONN, BT_GAP_ADV_FAST_INT_MIN_2, BT_GAP_ADV_FAST_INT_MAX_2, nullptr);
  if(int err = bt_le_adv_start(&param, advertising, ARRAY_SIZE(advertising), scan_response, ARRAY_SIZE(scan_response));
     err != 0 && err != -EALREADY) {
    LOG_ERR("advertising: %d", err);
  }
}
K_WORK_DEFINE(advertising_work, start_advertising);

void notify_state(k_work *) {
  auto [fan, strobe] = with_controller([](gtfog::controller &c) { return std::pair<std::uint8_t, std::uint8_t>(c.fan_speed(), c.strobe()); });
  // -ENOTCONN and friends only mean nobody listens.
  std::ignore = bt_gatt_notify(nullptr, &gtfog_service.attrs[fan_value], &fan, sizeof(fan));
  std::ignore = bt_gatt_notify(nullptr, &gtfog_service.attrs[strobe_value], &strobe, sizeof(strobe));
}
K_WORK_DEFINE(notify_work, notify_state);

void connected(bt_conn *conn, std::uint8_t err) {
  if(err != 0) {
    LOG_WRN("connection failed: 0x%02x", err);
    return;
  }
  current_conn = bt_conn_ref(conn);
}

void disconnected(bt_conn *conn, std::uint8_t reason) {
  LOG_INF("disconnected: 0x%02x", reason);
  accelerometer_stream(false);
  if(conn == current_conn) {
    bt_conn_unref(current_conn);
    current_conn = nullptr;
  }
}

void recycled() { k_work_submit(&advertising_work); }

} // namespace

BT_CONN_CB_DEFINE(connection_callbacks) = {
    .connected = connected,
    .disconnected = disconnected,
    .recycled = recycled,
};

auto ble_init() -> int {
  if(int err = bt_enable(nullptr); err != 0) {
    return err;
  }
  if(int err = settings_load(); err != 0) {
    LOG_WRN("settings: %d", err);
  }
  k_work_submit(&advertising_work);
  return 0;
}

auto ble_connected() -> bool { return current_conn != nullptr; }

void ble_state_changed() {
  if(bt_is_ready()) {
    k_work_submit(&notify_work);
  }
}

void ble_battery_changed(std::uint8_t percent) {
  if(bt_is_ready()) {
    std::ignore = bt_bas_set_battery_level(percent);
  }
}

void ble_charger_changed(gtfog::charger_state state) {
  auto value = static_cast<std::uint8_t>(state);
  if(charger.exchange(value) != value && bt_is_ready()) {
    std::ignore = bt_gatt_notify(nullptr, &gtfog_service.attrs[charger_value], &value, sizeof(value));
  }
}

auto ble_accel_samples_per_packet() -> std::size_t {
  bt_conn *conn = current_conn;
  return conn ? gtfog::accel_samples_per_packet(bt_gatt_get_mtu(conn)) : 0;
}

void ble_accel_send(std::span<std::uint8_t const> packet) {
  // A full buffer drops the packet; the sequence number shows the gap to the phone.
  std::ignore = bt_gatt_notify(nullptr, &gtfog_service.attrs[accel_value], packet.data(), static_cast<std::uint16_t>(packet.size()));
}
