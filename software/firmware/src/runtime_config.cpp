#include "runtime_config.hpp"

#include "compile_time_config.hpp"

#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/fs/nvs.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/storage/flash_map.h>


static struct nvs_fs fs;

#define NVS_PARTITION        storage_partition
#define NVS_PARTITION_DEVICE FIXED_PARTITION_DEVICE(NVS_PARTITION)
#define NVS_PARTITION_OFFSET FIXED_PARTITION_OFFSET(NVS_PARTITION)

int nvs_init() {
    struct flash_pages_info info;
    int                     rc;
    printk("nvs init");


    /* define the nvs file system by settings with:
     *	sector_size equal to the pagesize,
     *	3 sectors
     *	starting at NVS_PARTITION_OFFSET
     */
    fs.flash_device = NVS_PARTITION_DEVICE;
    if (!device_is_ready(fs.flash_device)) {
        printk("Flash device %s is not ready\n", fs.flash_device->name);
        return -1;
    }
    fs.offset = NVS_PARTITION_OFFSET;
    rc        = flash_get_page_info_by_offs(fs.flash_device, fs.offset, &info);
    if (rc) {
        printk("Unable to get page info, rc=%d\n", rc);
        return rc;
    }
    fs.sector_size  = info.size;
    fs.sector_count = 3U;

    rc = nvs_mount(&fs);
    if (rc) {
        printk("Flash Init failed, rc=%d\n", rc);
        return rc;
    }
    return 0;
}


SYS_INIT_NAMED(nvs_init, nvs_init, APPLICATION, NVS_INITIALIZATION_PRIORITY);


auto Configuration::do_get(uint16_t id, std::span<std::byte> buffer) noexcept -> std::expected<void, error_code> {
    auto to_read = static_cast<ssize_t>(buffer.size_bytes());
    auto read    = nvs_read(&fs, id, buffer.data(), to_read);

    std::expected<void, error_code> result;

    if (read != to_read) {
        result = std::unexpected(error_code{read});
    }
    return result;
}

auto Configuration::do_set(uint16_t id, std::span<std::byte const> data) noexcept -> std::expected<void, error_code> {
    auto to_write = static_cast<ssize_t>(data.size_bytes());

    auto written = nvs_write(&fs, id, data.data(), to_write);

    std::expected<void, error_code> result;

    if ((written != 0) && (written != to_write)) {
        result = std::unexpected(error_code{written});
    }
    return result;
}
