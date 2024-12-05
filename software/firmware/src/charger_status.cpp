// #include "charger_status.hpp"

#include "compile_time_config.hpp"
#include "system.hpp"

#include <SI/electric_potential.h>
#include <expected>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>

#include <tuple>
#include <utility>


/**
 * Charger status
 * The values are encoded STAT1 and STAT2 pin states
 */
enum class charger_status {
    off               = 0b00,
    recoverable_fault = 0b01,
    charging          = 0b10,
    charge_completed  = 0b11,
};


using namespace SI::literals;

#define ZEPHYR_USER_NODE DT_PATH(zephyr_user)

static gpio_dt_spec const stat1 = GPIO_DT_SPEC_GET(ZEPHYR_USER_NODE, charger_stat1_gpios);
static gpio_dt_spec const stat2 = GPIO_DT_SPEC_GET(ZEPHYR_USER_NODE, charger_stat2_gpios);


// #if !DT_NODE_EXISTS(DT_PATH(zephyr_user)) || !DT_NODE_HAS_PROP(DT_PATH(zephyr_user), io_channels)
// #    error "No suitable devicetree overlay specified"
// #endif

#define DT_SPEC_AND_COMMA(node_id, prop, idx) ADC_DT_SPEC_GET_BY_IDX(node_id, idx),

/* Data of ADC io-channels specified in devicetree. */
static const adc_dt_spec adc_channels[] = {DT_FOREACH_PROP_ELEM(DT_NODELABEL(adc), io_channels, DT_SPEC_AND_COMMA)};

// static_assert(std::size(adc_channel) == 1, "There should be exactly one channel to measure battery voltage");


static charger_status get_charger_status() {
    auto s1 = gpio_pin_get(stat1.port, stat1.pin);
    auto s2 = gpio_pin_get(stat2.port, stat2.pin);

    // [[unlikely]] if (s1 < 0) { return std::unexpected(error_code{s1}); }
    // [[unlikely]] if (s2 < 0) { return std::unexpected(error_code{s2}); }
// printk("CS1: %d, CS2: %d\n", s1, s2);
    return charger_status{(s1 << 1) | s2};
}


static int charger_status_thread(void) {
    [[unlikely]] if (!gpio_is_ready_dt(&stat1) || !gpio_is_ready_dt(&stat2)) { return -ENOENT; }
    // [[unlikely]] if (!adc_is_ready_dt(adc_channel)) { return -ENOENT; }


    int stat1_result = gpio_pin_configure_dt(&stat1, GPIO_INPUT);
    int stat2_result = gpio_pin_configure_dt(&stat2, GPIO_INPUT);

    [[unlikely]] if ((stat1_result < 0) || (stat2_result < 0)) { return -EINVAL; }


    // int err;
    while(1){

    // /* Configure channels individually prior to sampling. */
    // for (size_t i = 0U; i < ARRAY_SIZE(adc_channels); i++) {
    //     if (!adc_is_ready_dt(&adc_channels[i])) {
    //         printk("ADC controller device %s not ready\n", adc_channels[i].dev->name);
    //         return 0;
    //     }

    //     err = adc_channel_setup_dt(&adc_channels[i]);
    //     if (err < 0) {
    //         printk("Could not setup channel #%d (%d)\n", i, err);
    //         return 0;
    //     }
    // }
    // int16_t            buf;
    // struct adc_sequence sequence = {
    //     .buffer = &buf,
    //     /* buffer size in bytes, not number of samples */
    //     .buffer_size = sizeof(buf),
    //     .calibrate = true
    // };

    charger_status current_status = charger_status::off;
    // while (1) {
    //     for (size_t i = 0U; i < ARRAY_SIZE(adc_channels); i++) {
    //          buf = 0;

    //         printk("- %s, channel %d: ", adc_channels[i].dev->name, adc_channels[i].channel_id);

    //         (void)adc_sequence_init_dt(&adc_channels[i], &sequence);

    //         err = adc_read_dt(&adc_channels[i], &sequence);
    //         if (err < 0) {
    //             printk("Could not read (%d)\n", err);
    //             continue;
    //         }
    //        int32_t val_mv = static_cast<int32_t>(buf);
    //         printk("%" PRId32, val_mv);
    //         std::ignore = adc_raw_to_millivolts_dt(&adc_channels[i], &val_mv);
    //         printk(" = %" PRId32 " mV\n", val_mv);
    //          k_sleep(K_MSEC(1000));
    //     }


       


        // printk("- %s, channel %d: ", adc_channel->dev->name, adc_channel->channel_id);

        // std::ignore = adc_sequence_init_dt(adc_channel, &sequence);

        // int err = adc_read_dt(adc_channel, &sequence);
        // [[unlikely]] if (err < 0) { continue; }

        // int32_t val_mv = static_cast<int32_t>(sample_buffer);

        // std::ignore = adc_raw_to_millivolts_dt(adc_channel, &val_mv);
        // /* conversion to mV may not be supported, skip if not */
        // auto v_batt = 5_mV * val_mv;

        auto status = get_charger_status();
        if (current_status != status) {
            current_status = status;
            // system_process_event(charger_status_changed{status});
        }

        // printk(", charger status: %d\n",static_cast<int>(status));

        k_sleep(K_MSEC(50));
    }
    std::unreachable();
}

K_THREAD_DEFINE(charger_status, config::charger_status_thread_stack_size, charger_status_thread, nullptr, nullptr, nullptr, config::charger_status_thread_priority, 0, 0);
