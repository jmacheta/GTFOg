# Linked into the MCUboot image only (see ../sysbuild.cmake).
if(CONFIG_MCUBOOT_ACTION_HOOKS)
  zephyr_library()
  zephyr_library_sources(status.c)
  zephyr_library_include_directories(${ZEPHYR_MCUBOOT_MODULE_DIR}/boot/bootutil/include)
endif()
