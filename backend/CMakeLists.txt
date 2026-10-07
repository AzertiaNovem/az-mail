cmake_minimum_required(VERSION 3.24)

project(azmail VERSION 0.1.0 LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(AZMAIL_BUILD_TESTS "Build the Catch2 unit tests" ON)
option(AZMAIL_SANITIZE "Build with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)
option(AZMAIL_WERROR "Treat compiler warnings as errors" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(Warnings)

if(AZMAIL_SANITIZE)
  # Applied globally so fetched dependencies (Catch2) are instrumented consistently.
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer
                      -fno-sanitize-recover=undefined)
  add_link_options(-fsanitize=address,undefined)
endif()

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------
# CMake >= 4 has no FindBoost module: use Boost's own CMake config.
# Asio / Beast / System are header-only; json, url and program_options are compiled.
find_package(Boost 1.83 CONFIG REQUIRED COMPONENTS json url program_options)
find_package(OpenSSL 3 REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(Threads REQUIRED)
# RFC 2047 charset conversion in mail/eml.cpp (GBK/GB18030/Big5/ISO-8859-x). glibc provides it
# built in; macOS needs libiconv.
find_package(Iconv REQUIRED)

if(NOT TARGET SQLite3::SQLite3 AND TARGET SQLite::SQLite3)  # CMake < 4.3
  add_library(SQLite3::SQLite3 ALIAS SQLite::SQLite3)
endif()

# ---------------------------------------------------------------------------
# Library: everything under src/ except main.cpp (globbed; work packages never edit CMake)
# ---------------------------------------------------------------------------
file(GLOB_RECURSE AZMAIL_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
list(FILTER AZMAIL_SOURCES EXCLUDE REGEX ".*/src/main\\.cpp$")

add_library(azmail_lib STATIC ${AZMAIL_SOURCES})
target_include_directories(azmail_lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_definitions(azmail_lib PUBLIC
  BOOST_ASIO_NO_DEPRECATED
  BOOST_ASIO_SEPARATE_COMPILATION
  BOOST_BEAST_SEPARATE_COMPILATION
  "AZMAIL_VERSION=\"${PROJECT_VERSION}\"")
target_link_libraries(azmail_lib PUBLIC
  Boost::headers Boost::json Boost::url Boost::program_options
  OpenSSL::SSL OpenSSL::Crypto
  SQLite3::SQLite3
  Iconv::Iconv
  Threads::Threads)
azmail_set_warnings(azmail_lib)

target_precompile_headers(azmail_lib PRIVATE cmake/pch.hpp)
# The separate-compilation TU must see Asio/Beast in "source" mode from its first include.
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/src/core/asio_impl.cpp"
  PROPERTIES SKIP_PRECOMPILE_HEADERS ON)

# ---------------------------------------------------------------------------
# Executable
# ---------------------------------------------------------------------------
add_executable(azmail src/main.cpp)
target_link_libraries(azmail PRIVATE azmail_lib)
azmail_set_warnings(azmail)
if(AZMAIL_SANITIZE)
  target_sources(azmail PRIVATE cmake/sanitizer_defaults.cpp)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(AZMAIL_BUILD_TESTS)
  enable_testing()

  find_package(Catch2 3 QUIET)
  if(NOT Catch2_FOUND)
    include(FetchContent)
    FetchContent_Declare(Catch2
      GIT_REPOSITORY https://github.com/catchorg/Catch2.git
      GIT_TAG v3.7.1
      GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(Catch2)
    list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
  endif()
  include(Catch)

  file(GLOB AZMAIL_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/*.cpp")
  add_executable(azmail_tests ${AZMAIL_TEST_SOURCES})
  target_link_libraries(azmail_tests PRIVATE azmail_lib Catch2::Catch2WithMain)
  target_compile_definitions(azmail_tests PRIVATE
    "AZMAIL_TEST_FIXTURES=\"${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures\"")
  target_precompile_headers(azmail_tests PRIVATE cmake/pch.hpp <catch2/catch_test_macros.hpp>)
  azmail_set_warnings(azmail_tests)
  if(AZMAIL_SANITIZE)
    target_sources(azmail_tests PRIVATE cmake/sanitizer_defaults.cpp)
    set_source_files_properties(cmake/sanitizer_defaults.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
  endif()

  # Sanitizer runtime defaults are compiled in (cmake/sanitizer_defaults.cpp).
  catch_discover_tests(azmail_tests
    DISCOVERY_MODE PRE_TEST
    PROPERTIES TIMEOUT 300)
endif()
