# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(firmware)

target_sources(
  app PRIVATE src/main.cpp src/pwm_control.cpp src/charger_status.cpp src/accelerometer.cpp src/runtime_config.cpp
              src/ble.cpp
)

target_include_directories(app PRIVATE src/include)
