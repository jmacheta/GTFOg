target_sources(
  app
  PRIVATE main.cpp
          pwm.cpp
          charger.cpp
          accelerometer.cpp
          system.cpp
          buttons.cpp
          runtime_config.cpp
       #   src/ble.cpp
)
target_include_directories(app PRIVATE include)

FetchContent_MakeAvailable(sml si fmt)

target_link_libraries(app PRIVATE sml::sml SI::SI fmt::fmt-header-only)
