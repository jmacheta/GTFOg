# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(firmware)

add_compile_options(-fmodules-ts)

test_sysbuild()

include(requirements.cmake)

target_sources(
  app
  PRIVATE src/main.cpp src/pwm_control.cpp src/charger_status.cpp src/accelerometer.cpp
          # src/runtime_config.cpp src/ble.cpp
          src/system.cpp src/buttons.cpp
)
target_include_directories(app PRIVATE src/include)

FetchContent_MakeAvailable(sml si fmt)

target_link_libraries(app PRIVATE sml::sml SI::SI fmt::fmt-header-only)

# target_link_libraries(sml PRIVATE zephyr_interface) target_link_libraries(si PRIVATE app) target_link_libraries(fmt
# PRIVATE zephyr_interface)
