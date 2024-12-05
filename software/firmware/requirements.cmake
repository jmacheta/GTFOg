include(FetchContent)

FetchContent_Declare(sml URL https://github.com/boost-ext/sml/archive/refs/tags/v1.1.11.zip EXCLUDE_FROM_ALL)

FetchContent_Declare(si URL https://github.com/bernedom/SI/archive/refs/tags/2.5.1.zip EXCLUDE_FROM_ALL)


set(FMT_INSTALL OFF CACHE BOOL "")
set(FMT_OS OFF CACHE BOOL "")
set(FMT_UNICODE OFF CACHE BOOL "")
FetchContent_Declare(fmt URL https://github.com/fmtlib/fmt/releases/download/11.0.2/fmt-11.0.2.zip EXCLUDE_FROM_ALL)
