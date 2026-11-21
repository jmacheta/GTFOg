# Hardware-independent logic, also built by the tests in tests/logic.
target_sources(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/battery.cpp ${CMAKE_CURRENT_LIST_DIR}/buttons.cpp ${CMAKE_CURRENT_LIST_DIR}/controller.cpp)
target_include_directories(app PRIVATE ${CMAKE_CURRENT_LIST_DIR})
