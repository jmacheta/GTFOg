/* MCUboot status feedback: everything off at startup; the red strobe blinks while a new image is being installed. */
#include <bootutil/mcuboot_status.h>
#include <zephyr/drivers/pwm.h>

/* The PWM blinks on its own, so the copy loop needs no timer or thread. */
#define BLINK_PERIOD PWM_MSEC(200)

static const struct pwm_dt_spec strobe = PWM_DT_SPEC_GET(DT_NODELABEL(strobe));

/* The PWM driver applies each channel's polarity, so a zero pulse is "off" for the inverted RGB LEDs too. */
static const struct pwm_dt_spec off_at_startup[] = {
    PWM_DT_SPEC_GET(DT_NODELABEL(fan)),   PWM_DT_SPEC_GET(DT_NODELABEL(led_r)),  PWM_DT_SPEC_GET(DT_NODELABEL(led_g)),
    PWM_DT_SPEC_GET(DT_NODELABEL(led_b)), PWM_DT_SPEC_GET(DT_NODELABEL(strobe)),
};

void mcuboot_status_change(mcuboot_status_type_t status) {
  if(status == MCUBOOT_STATUS_STARTUP) {
    for(size_t i = 0; i < ARRAY_SIZE(off_at_startup); i++) {
      if(pwm_is_ready_dt(&off_at_startup[i])) {
        (void)pwm_set_pulse_dt(&off_at_startup[i], 0);
      }
    }
    return;
  }
  if(!pwm_is_ready_dt(&strobe)) {
    return;
  }
  (void)pwm_set_dt(&strobe, BLINK_PERIOD, status == MCUBOOT_STATUS_UPGRADING ? BLINK_PERIOD / 2 : 0);
}
