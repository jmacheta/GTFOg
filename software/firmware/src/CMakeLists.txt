include(${CMAKE_CURRENT_LIST_DIR}/logic.cmake)

target_sources(app PRIVATE accelerometer.cpp battery.cpp ble.cpp board.cpp buttons.cpp main.cpp rf_test.cpp)
gtfog_add_logic(app)
