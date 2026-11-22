// Bluetooth LE peripheral: the GTFOg control service, Battery Service and Device Information Service.
#include "app.hpp"
#include "motion.hpp"

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/services/bas.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <optional>
#include <tuple>
#include <utility>

LOG_MODULE_REGISTER(ble, LOG_LEVEL_INF);

using gtfog::accelerometer_stream, gtfog::with_controller;

namespace {

K_MUTEX_DEFINE(save_lock); // Keeps the delayed save and the save before System OFF apart.

#define GTFOG_UUID(n) BT_UUID_INIT_128(BT_UUID_128_ENCODE(0xa7f00000 + (n), 0x5c1e, 0x4b8e, 0x9d2a, 0x6f0e1c3b4d50))

bt_uuid_128 const service_uuid = GTFOG_UUID(1);
bt_uuid_128 const fan_uuid = GTFOG_UUID(2);
bt_uuid_128 const strobe_uuid = GTFOG_UUID(3);
bt_uuid_128 const accelerometer_uuid = GTFOG_UUID(4);
bt_uuid_128 const charger_uuid = GTFOG_UUID(5);
bt_uuid_128 const settings_uuid = GTFOG_UUID(6);
bt_uuid_128 const effective_fan_uuid = GTFOG_UUID(7);
constexpr char settings_key[] = "gtfog/cfg";

std::atomic<std::uint8_t> charger{0};
// ponytail: one connection at a time (CONFIG_BT_MAX_CONN=1); the accelerometer thread reads only the cached ATT MTU.
std::atomic<bt_conn *> current_conn{nullptr};
std::atomic<std::uint16_t> att_mtu{0};

auto read_fan(bt_conn *conn, bt_gatt_attr const *attr, void *buf, std::uint16_t len, std::uint16_t offset) -> ssize_t {
  auto value = with_controller([](gtfog::controller &control) { return static_cast<std::uint8_t>(control.fan_speed()); });
  return bt_gatt_attr_read(conn, attr, buf, len, offset, &value, sizeof(value));
}

auto write_fan(bt_conn *, bt_gatt_attr const *, void const *buf, std::uint16_t len, std::uint16_t offset, std::uint8_t) -> ssize_t {
  if(offset != 0 || len != 1) {
    return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
  }
  auto value = *static_cast<std::uint8_t const *>(buf);
  if(!gtfog::is_fan_percent(value)) {
    return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
  }
  with_controller([&](gtfog::controller &control) { control.set_fan_speed(value); });
  return len;
}

auto read_strobe(bt_conn *conn, bt_gatt_attr const *attr, void *buf, std::uint16_t len, std::uint16_t offset) -> ssize_t {
  std::uint8_t value = with_controller([](gtfog::controller &control) { return control.strobe(); });
  return bt_gatt_attr_read(conn, attr, buf, len, offset, &value, sizeof(value));
}

auto write_strobe(bt_conn *, bt_gatt_attr const *, void const *buf, std::uint16_t len, std::uint16_t offset, std::uint8_t) -> ssize_t {
  if(offset != 0 || len != 1) {
    return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
  }
  auto value = *static_cast<std::uint8_t const *>(buf);
  if(!gtfog::is_strobe_value(value)) {
    return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
  }
  with_controller([&](gtfog::controller &control) { control.set_strobe(value != 0); });
  return len;
}

auto read_charger(bt_conn *conn, bt_gatt_attr const *attr, void *buf, std::uint16_t len, std::uint16_t offset) -> ssize_t {
  std::uint8_t value = charger;
  return bt_gatt_attr_read(conn, attr, buf, len, offset, &value, sizeof(value));
}

auto read_settings(bt_conn *conn, bt_gatt_attr const *attr, void *buf, std::uint16_t len, std::uint16_t offset) -> ssize_t {
  auto value = gtfog::encode(with_controller([](gtfog::controller &control) { return control.settings(); }));
  return bt_gatt_attr_read(conn, attr, buf, len, offset, value.data(), value.size());
}

auto write_settings(bt_conn *, bt_gatt_attr const *, void const *buf, std::uint16_t len, std::uint16_t offset, std::uint8_t) -> ssize_t {
  // Shorter forms change only the fields they carry, so they are decoded on top of the current settings.
  bool accepted = offset == 0 && with_controller([&](gtfog::controller &control) {
                    auto settings = gtfog::decode_settings({static_cast<std::uint8_t const *>(buf), len}, control.settings());
                    if(settings) {
                      control.update_settings(*settings); // Calls ble_settings_changed().
                    }
                    return settings.has_value();
                  });
  if(!accepted) {
    return BT_GATT_ERR(gtfog::is_settings_size(len) ? BT_ATT_ERR_VALUE_NOT_ALLOWED : BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
  }
  return len;
}

std::optional<std::array<std::uint8_t, gtfog::settings_size>> last_saved; // Flash content; used by load_settings and save_pending, under save_lock.
auto load_settings(char const *key, std::size_t len, settings_read_cb read, void *arg) -> int {
  std::array<std::uint8_t, gtfog::settings_size> bytes{};
  if(std::strcmp(key, "cfg") != 0 || !gtfog::is_settings_size(len) || read(arg, bytes.data(), len) != static_cast<ssize_t>(len)) {
    return 0; // Unknown or damaged entries keep the defaults.
  }
  if(auto settings = gtfog::decode_settings({bytes.data(), len})) {
    with_controller([&](gtfog::controller &control) { control.configure(*settings); });
    k_mutex_lock(&save_lock, K_FOREVER);
    last_saved = gtfog::encode(*settings);
    k_mutex_unlock(&save_lock);
  }
  return 0;
}

auto read_effective_fan(bt_conn *conn, bt_gatt_attr const *attr, void *buf, std::uint16_t len, std::uint16_t offset) -> ssize_t {
  auto value = with_controller([](gtfog::controller &control) {
    return std::array<std::uint8_t, 2>{static_cast<std::uint8_t>(control.fan_speed()), control.activity_code()};
  });
  return bt_gatt_attr_read(conn, attr, buf, len, offset, &value, sizeof(value));
}

void accelerometer_subscribed(bt_gatt_attr const *, std::uint16_t value) { accelerometer_stream(value == BT_GATT_CCC_NOTIFY); }

constexpr auto rw_notify = BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE | BT_GATT_CHRC_NOTIFY;
constexpr auto encrypted = BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT;

} // namespace

BT_GATT_SERVICE_DEFINE(gtfog_service, BT_GATT_PRIMARY_SERVICE(&service_uuid),
                       BT_GATT_CHARACTERISTIC(&fan_uuid.uuid, rw_notify, encrypted, read_fan, write_fan, nullptr),
                       BT_GATT_CCC(nullptr, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE_ENCRYPT),
                       BT_GATT_CHARACTERISTIC(&strobe_uuid.uuid, rw_notify, encrypted, read_strobe, write_strobe, nullptr),
                       BT_GATT_CCC(nullptr, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE_ENCRYPT),
                       BT_GATT_CHARACTERISTIC(&accelerometer_uuid.uuid, BT_GATT_CHRC_NOTIFY, BT_GATT_PERM_NONE, nullptr, nullptr, nullptr),
                       BT_GATT_CCC(accelerometer_subscribed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE_ENCRYPT),
                       BT_GATT_CHARACTERISTIC(&charger_uuid.uuid, BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY, BT_GATT_PERM_READ_ENCRYPT, read_charger,
                                              nullptr, nullptr),
                       BT_GATT_CCC(nullptr, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE_ENCRYPT),
                       BT_GATT_CHARACTERISTIC(&settings_uuid.uuid, rw_notify, encrypted, read_settings, write_settings, nullptr),
                       BT_GATT_CCC(nullptr, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE_ENCRYPT),
                       BT_GATT_CHARACTERISTIC(&effective_fan_uuid.uuid, BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY, BT_GATT_PERM_READ_ENCRYPT,
                                              read_effective_fan, nullptr, nullptr),
                       BT_GATT_CCC(nullptr, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE_ENCRYPT), );

SETTINGS_STATIC_HANDLER_DEFINE(gtfog, "gtfog", nullptr, load_settings, nullptr, nullptr);

namespace {

// Attribute indices in gtfog_service: 0 service, 1 + 3k characteristic declaration, 2 + 3k value, 3 + 3k CCC.
constexpr auto fan_value = 2;
constexpr auto strobe_value = 5;
constexpr auto accelerometer_value = 8;
constexpr auto charger_value = 11;
constexpr auto settings_value = 14;
constexpr auto effective_fan_value = 17;
static_assert(ARRAY_SIZE(attr_gtfog_service) == 19, "update the attribute indices above");

std::uint8_t const flags[] = {BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR};
std::uint8_t const service_uuid_bytes[] = {BT_UUID_128_ENCODE(0xa7f00001, 0x5c1e, 0x4b8e, 0x9d2a, 0x6f0e1c3b4d50)};

bt_data const advertising[] = {
    {BT_DATA_FLAGS, sizeof(flags), flags},
    {BT_DATA_UUID128_ALL, sizeof(service_uuid_bytes), service_uuid_bytes},
};
bt_data const scan_response[] = {
    {BT_DATA_NAME_COMPLETE, sizeof(CONFIG_BT_DEVICE_NAME) - 1, reinterpret_cast<std::uint8_t const *>(CONFIG_BT_DEVICE_NAME)},
};

void start_advertising(k_work *work);
K_WORK_DELAYABLE_DEFINE(advertising_work, start_advertising);

// A failed start is tried again every 5 s, so the device does not stay unreachable.
void start_advertising(k_work *) {
  bt_le_adv_param param = BT_LE_ADV_PARAM_INIT(BT_LE_ADV_OPT_CONN, BT_GAP_ADV_FAST_INT_MIN_2, BT_GAP_ADV_FAST_INT_MAX_2, nullptr);
  if(int err = bt_le_adv_start(&param, advertising, ARRAY_SIZE(advertising), scan_response, ARRAY_SIZE(scan_response));
     err != 0 && err != -EALREADY) {
    LOG_ERR("advertising: %d", err);
    k_work_reschedule(&advertising_work, K_SECONDS(5));
  }
}

void notify_state(k_work *) {
  auto [fan, activity, strobe] = with_controller([](gtfog::controller &control) {
    return std::tuple<std::uint8_t, std::uint8_t, std::uint8_t>(control.fan_speed(), control.activity_code(), control.strobe());
  });
  std::array<std::uint8_t, 2> const actual{fan, activity};
  // -ENOTCONN and friends only mean nobody listens.
  std::ignore = bt_gatt_notify(nullptr, &gtfog_service.attrs[fan_value], &fan, sizeof(fan));
  std::ignore = bt_gatt_notify(nullptr, &gtfog_service.attrs[effective_fan_value], actual.data(), actual.size());
  std::ignore = bt_gatt_notify(nullptr, &gtfog_service.attrs[strobe_value], &strobe, sizeof(strobe));
}
K_WORK_DEFINE(notify_work, notify_state);

std::optional<std::array<std::uint8_t, gtfog::settings_size>> pending_settings;
k_spinlock pending_settings_lock;

// Writes the newest settings to flash unless they are already there. Used by the delayed work and by ble_settings_flush.
void save_pending() {
  k_mutex_lock(&save_lock, K_FOREVER);
  auto key = k_spin_lock(&pending_settings_lock);
  auto bytes = pending_settings;
  k_spin_unlock(&pending_settings_lock, key);
  if(bytes && bytes != last_saved) {
    if(int err = settings_save_one(settings_key, bytes->data(), bytes->size()); err != 0) {
      LOG_ERR("save settings: %d", err);
    } else {
      last_saved = bytes;
    }
  }
  k_mutex_unlock(&save_lock);
}

void save_settings(k_work *) { save_pending(); }
K_WORK_DELAYABLE_DEFINE(save_settings_work, save_settings);

void notify_settings(k_work *) {
  auto key = k_spin_lock(&pending_settings_lock);
  auto bytes = pending_settings;
  k_spin_unlock(&pending_settings_lock, key);
  if(bytes) {
    // Fails only when nobody listens.
    std::ignore = bt_gatt_notify(nullptr, &gtfog_service.attrs[settings_value], bytes->data(), static_cast<std::uint16_t>(bytes->size()));
  }
}
K_WORK_DEFINE(notify_settings_work, notify_settings);

// Remembers that a refused pairing needs a status LED signal; the work takes the controller mutex, the Bluetooth callback must not.
void report_pairing_refused(k_work *) {
  with_controller([](gtfog::controller &control) { control.report(gtfog::signal::pairing_refused); });
}
K_WORK_DEFINE(pairing_refused_work, report_pairing_refused);

// New pairings are only accepted in the first minutes after boot; bonded phones are not asked.
enum bt_security_err pairing_accept(bt_conn *, bt_conn_pairing_feat const *) {
  constexpr std::int64_t pairing_window_ms = 3 * 60 * 1000;
  if(k_uptime_get() < pairing_window_ms) {
    return BT_SECURITY_ERR_SUCCESS;
  }
  k_work_submit(&pairing_refused_work);
  return BT_SECURITY_ERR_PAIR_NOT_ALLOWED;
}

bt_conn_auth_cb const auth_callbacks = {.pairing_accept = pairing_accept};

void connected(bt_conn *conn, std::uint8_t err) {
  if(err != 0) {
    LOG_WRN("connection failed: 0x%02x", err);
    return;
  }
  att_mtu = bt_gatt_get_mtu(conn);
  current_conn = bt_conn_ref(conn);
}

void mtu_updated(bt_conn *, std::uint16_t tx, std::uint16_t rx) { att_mtu = std::min(tx, rx); }

bt_gatt_cb gatt_callbacks = {.att_mtu_updated = mtu_updated};

void disconnected(bt_conn *conn, std::uint8_t reason) {
  LOG_INF("disconnected: 0x%02x", reason);
  accelerometer_stream(false);
  if(conn == current_conn) {
    att_mtu = 0;
    bt_conn_unref(current_conn.exchange(nullptr));
  }
}

void recycled() { k_work_reschedule(&advertising_work, K_NO_WAIT); }

} // namespace

BT_CONN_CB_DEFINE(connection_callbacks) = {
    .connected = connected,
    .disconnected = disconnected,
    .recycled = recycled,
};

namespace gtfog {

void app_settings_load() {
  if(int err = settings_subsys_init(); err != 0) {
    LOG_WRN("settings init: %d", err);
    return;
  }
  if(int err = settings_load_subtree("gtfog"); err != 0) {
    LOG_WRN("settings: %d", err);
  }
}

auto ble_init() -> int {
  bt_gatt_cb_register(&gatt_callbacks);
  if(int err = bt_conn_auth_cb_register(&auth_callbacks); err != 0) {
    return err;
  }
  if(int err = bt_enable(nullptr); err != 0) {
    return err;
  }
  if(int err = settings_load_subtree("bt"); err != 0) { // Bonds; the "gtfog" subtree was loaded by app_settings_load().
    LOG_WRN("bt settings: %d", err);
  }
  k_work_reschedule(&advertising_work, K_NO_WAIT);
  return 0;
}

auto ble_connected() -> bool { return current_conn.load() != nullptr; }

void ble_state_changed() {
  if(bt_is_ready()) {
    k_work_submit(&notify_work);
  }
}

void ble_settings_changed(device_settings const &settings) {
  auto key = k_spin_lock(&pending_settings_lock);
  pending_settings = encode(settings);
  k_spin_unlock(&pending_settings_lock, key);
  k_work_submit(&notify_settings_work);
  // Bursts (button auto-repeat in motion mode) become one flash write.
  k_work_reschedule(&save_settings_work, K_SECONDS(2));
}

void ble_settings_flush() {
  k_work_cancel_delayable(&save_settings_work); // Does not wait: the system workqueue may be blocked on the controller mutex.
  save_pending();
}

void ble_battery_changed(std::uint8_t percent) {
  if(bt_is_ready()) {
    std::ignore = bt_bas_set_battery_level(percent); // Only fails for a value above 100; the next sample retries.
  }
}

void ble_charger_changed(charger_state state) {
  auto value = static_cast<std::uint8_t>(state);
  if(charger.exchange(value) != value && bt_is_ready()) {
    std::ignore = bt_gatt_notify(nullptr, &gtfog_service.attrs[charger_value], &value, sizeof(value)); // Fails only when nobody listens.
  }
}

auto ble_accelerometer_samples_per_packet() -> std::size_t {
  return accelerometer_samples_per_packet(att_mtu); // 0 without a connection.
}

void ble_accelerometer_send(std::span<std::uint8_t const> packet) {
  // A full buffer drops the packet; the sequence number shows the gap to the phone.
  std::ignore = bt_gatt_notify(nullptr, &gtfog_service.attrs[accelerometer_value], packet.data(), static_cast<std::uint16_t>(packet.size()));
}

} // namespace gtfog
