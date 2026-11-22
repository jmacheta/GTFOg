cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(gtfog_logic_tests LANGUAGES C CXX)

include(${CMAKE_CURRENT_LIST_DIR}/../../src/logic.cmake)

target_sources(app PRIVATE src/test_battery_model.cpp src/test_controller.cpp src/test_controller_machine.cpp src/test_gestures.cpp src/test_motion.cpp src/test_rf_test.cpp)
gtfog_add_logic(app)
