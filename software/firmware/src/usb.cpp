// USB device: a DFU run-time interface, which switches to DFU mode to write a new image to slot 1 for MCUboot.
#include "app.hpp"

#include <zephyr/dfu/mcuboot.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/usb/usbd.h>

#include <tuple>

LOG_MODULE_REGISTER(usb, LOG_LEVEL_INF);

namespace {

constexpr std::uint16_t vid = 0x1209; // pid.codes
constexpr std::uint16_t pid = 0xEACC;
constexpr std::uint16_t dfu_pid = 0xEACD;

USBD_DEVICE_DEFINE(app_usbd, DEVICE_DT_GET(DT_NODELABEL(usbd)), vid, pid);
USBD_DEVICE_DEFINE(dfu_usbd, DEVICE_DT_GET(DT_NODELABEL(usbd)), vid, dfu_pid);

USBD_DESC_LANG_DEFINE(lang);
USBD_DESC_MANUFACTURER_DEFINE(manufacturer, "elmaczete_customs");
USBD_DESC_PRODUCT_DEFINE(product, "GTFOg");
USBD_DESC_SERIAL_NUMBER_DEFINE(serial_number);
USBD_DESC_CONFIG_DEFINE(app_config_desc, "GTFOg");
USBD_DESC_CONFIG_DEFINE(dfu_config_desc, "GTFOg DFU");

constexpr int max_power_2ma = 50; // 100 mA
USBD_CONFIGURATION_DEFINE(app_config, 0, max_power_2ma, &app_config_desc);
USBD_CONFIGURATION_DEFINE(dfu_config, 0, max_power_2ma, &dfu_config_desc);

void msg_cb(usbd_context *const ctx, usbd_msg const *const msg);

void reboot(k_work *) { sys_reboot(SYS_REBOOT_COLD); }
K_WORK_DELAYABLE_DEFINE(reboot_work, reboot);

auto setup(usbd_context *ctx, usbd_config_node *config, char const *class_name) -> int {
  int err = 0;
  for(auto *desc : {&lang, &manufacturer, &product, &serial_number}) {
    if(err == 0) {
      err = usbd_add_descriptor(ctx, desc);
    }
  }
  if(err == 0) {
    err = usbd_add_configuration(ctx, USBD_SPEED_FS, config);
  }
  if(err == 0) {
    err = usbd_register_class(ctx, class_name, USBD_SPEED_FS, 1);
  }
  if(err == 0) {
    usbd_device_set_code_triple(ctx, USBD_SPEED_FS, 0, 0, 0);
    err = usbd_msg_register_cb(ctx, msg_cb);
  }
  if(err == 0) {
    err = usbd_init(ctx);
  }
  return err;
}

void switch_to_dfu_mode(usbd_context *ctx) {
  usbd_disable(ctx);
  usbd_shutdown(ctx);
  if(int err = setup(&dfu_usbd, &dfu_config, "dfu_dfu"); err != 0) {
    LOG_ERR("DFU mode: %d", err);
    return;
  }
  if(int err = usbd_enable(&dfu_usbd); err != 0) {
    LOG_ERR("DFU mode enable: %d", err);
  }
}

void msg_cb(usbd_context *const ctx, usbd_msg const *const msg) {
  switch(msg->type) {
  case USBD_MSG_VBUS_READY:
    if(int err = usbd_enable(ctx); err != 0) {
      LOG_ERR("enable: %d", err);
    }
    break;
  case USBD_MSG_VBUS_REMOVED:
    std::ignore = usbd_disable(ctx);
    break;
  case USBD_MSG_DFU_APP_DETACH:
    switch_to_dfu_mode(ctx);
    break;
  case USBD_MSG_DFU_DOWNLOAD_COMPLETED:
    // With MCUboot in upgrade-only mode the request is permanent; reboot after the host has seen the status.
    if(int err = boot_request_upgrade(BOOT_UPGRADE_PERMANENT); err != 0) {
      LOG_ERR("upgrade request: %d", err);
      break;
    }
    LOG_INF("new image in slot 1, rebooting");
    k_work_schedule(&reboot_work, K_SECONDS(1));
    break;
  default:
    break;
  }
}

} // namespace

auto usb_init() -> int {
  if(int err = setup(&app_usbd, &app_config, "dfu_runtime"); err != 0) {
    return err;
  }
  // VBUS detection enables the device on connect; without it, enable it now.
  return usbd_can_detect_vbus(&app_usbd) ? 0 : usbd_enable(&app_usbd);
}
