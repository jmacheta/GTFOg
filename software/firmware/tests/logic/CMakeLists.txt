cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(gtfog_logic_tests LANGUAGES C CXX)

target_sources(app PRIVATE src/test_battery.cpp src/test_buttons.cpp src/test_controller.cpp)
add_subdirectory(../../src/logic logic)
