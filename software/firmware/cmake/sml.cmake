# Boost.SML, header only, fetched at configure time; the app and the tests link the `sml` interface target.
include(FetchContent)

FetchContent_Declare(
  sml
  URL https://github.com/boost-ext/sml/archive/refs/tags/v1.1.11.zip
  URL_HASH SHA256=ba583c96dbfe4e2d743099e5e73f808a5ce6d334f55f36140f058d4cdb6c3023
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  SOURCE_SUBDIR none # Only the headers are used: SML's own CMakeLists is not added.
)
FetchContent_MakeAvailable(sml)

if(NOT TARGET sml)
  add_library(sml INTERFACE)
  # SYSTEM: the library's own warnings are not ours. It detects -fno-exceptions and -fno-rtti by itself.
  target_include_directories(sml SYSTEM INTERFACE ${sml_SOURCE_DIR}/include)
endif()
