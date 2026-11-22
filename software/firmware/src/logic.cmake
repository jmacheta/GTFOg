# The pure logic without Zephyr, built into the firmware and into the host tests: `gtfog_add_logic(app)`.
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/sml.cmake)

function(gtfog_add_logic target)
  target_sources(
    ${target}
    PRIVATE ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/battery_model.cpp
            ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/controller.cpp
            ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/gestures.cpp
            ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/motion.cpp)
  target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/include)
  target_link_libraries(${target} PRIVATE sml)
endfunction()
