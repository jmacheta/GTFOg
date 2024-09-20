#include "runtime_config.hpp"

#include "compile_time_config.hpp"

#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/fs/nvs.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/storage/flash_map.h>


struct direct_immediate_value {
    size_t size;
    void*  buffer;
    bool   fetched;
};

static int direct_loader_immediate_value(const char* name, size_t length, settings_read_cb read_cb, void* cb_arg, void* param) {
    if constexpr (!IS_ENABLED(CONFIG_SETTINGS)) {
        return -ENOTSUP;
    }
    auto        value = static_cast<direct_immediate_value*>(param);
    const char* next;
    size_t      name_len = settings_name_next(name, &next);

    if (name_len == 0) {
        if (length == value->size) {
            int rc = read_cb(cb_arg, value->buffer, length);
            if (rc >= 0) {
                value->fetched = true;
                printk("immediate load: OK.\n");
                return 0;
            }

            printk("immediate load: failed (status %d)", rc);
            return rc;
        }
        return -EINVAL;
    }

    /* other keys aren't served by the callback
     * Return success in order to skip them
     * and keep storage processing.
     */
    return 0;
}


static int settings_init() {
    if constexpr (!IS_ENABLED(CONFIG_SETTINGS)) {
        printk("settings subsys initialization: fail - not enabled");

        return static_cast<int>(error_code::feature_disabled);
    }

    int rc = settings_subsys_init();
    if (rc != 0) {
        printk("settings subsys initialization: fail (err %d)\n", rc);
        return rc;
    }

    printk("settings subsys initialization: OK.\n");
    return 0;
}

SYS_INIT_NAMED(settings_init, settings_init, APPLICATION, NVS_INITIALIZATION_PRIORITY);


auto Configuration::do_get(std::string_view key, std::span<std::byte> buffer) noexcept -> std::expected<void, error_code> {
    if constexpr (!IS_ENABLED(CONFIG_SETTINGS)) {
        return std::unexpected(error_code::feature_disabled);
    }

    direct_immediate_value descriptor{
        .size    = buffer.size_bytes(),
        .buffer  = buffer.data(),
        .fetched = false,
    };

    int load_status = settings_load_subtree_direct(key.data(), direct_loader_immediate_value, &descriptor);

    if ((0 == load_status) && !descriptor.fetched) {
        load_status = -ENOENT;
    }


    std::expected<void, error_code> result;

    if (load_status != 0) {
        result = std::unexpected(error_code{load_status});
    }
    return result;
}

auto Configuration::do_set(std::string_view key, std::span<std::byte const> data) noexcept -> std::expected<void, error_code> {
    if constexpr (!IS_ENABLED(CONFIG_SETTINGS)) {
        return std::unexpected(error_code::feature_disabled);
    }

    int write_status = settings_save_one(key.data(), data.data(), data.size_bytes());

    std::expected<void, error_code> result;

    if (write_status != 0) {
        result = std::unexpected(error_code{write_status});
    }
    return result;
#
}
