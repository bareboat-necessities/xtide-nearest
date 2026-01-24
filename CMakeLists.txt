cmake_minimum_required(VERSION 3.20)

project(xtide_nearest VERSION 0.2.0 LANGUAGES C CXX)

option(XTN_WARNINGS_AS_ERRORS "Treat warnings as errors" OFF)

include(FetchContent)

# ---- CLI11 (CLI only) ----
FetchContent_Declare(
  CLI11
  GIT_REPOSITORY https://github.com/CLIUtils/CLI11.git
  GIT_TAG v2.4.2
)
FetchContent_MakeAvailable(CLI11)

# ---- libtcd ----
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")
find_package(TCD REQUIRED)

# ---- Warnings helper ----
add_library(xtn_warnings INTERFACE)
if (MSVC)
  target_compile_options(xtn_warnings INTERFACE /W4)
  if (XTN_WARNINGS_AS_ERRORS)
    target_compile_options(xtn_warnings INTERFACE /WX)
  endif()
else()
  target_compile_options(xtn_warnings INTERFACE -Wall -Wextra -Wpedantic)
  if (XTN_WARNINGS_AS_ERRORS)
    target_compile_options(xtn_warnings INTERFACE -Werror)
  endif()
endif()

# ---- Library (server/API) ----
add_library(xtide_nearest
  src/xtide_nearest_core.cpp
  src/xtide_nearest_stations.cpp
  src/xtide_nearest_tide_runner.cpp
)

target_compile_features(xtide_nearest PUBLIC cxx_std_20)
set_target_properties(xtide_nearest PROPERTIES
  POSITION_INDEPENDENT_CODE ON
)

target_include_directories(xtide_nearest
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(xtide_nearest
  PRIVATE
    xtn_warnings
  PUBLIC
    TCD::tcd
)

# ---- CLI executable ----
add_executable(xtide-nearest
  src/xtide_nearest_cli.cpp
)

target_compile_features(xtide-nearest PRIVATE cxx_std_20)

target_link_libraries(xtide-nearest
  PRIVATE
    xtn_warnings
    xtide_nearest
    CLI11::CLI11
)

# Optional: if you ever rename the target but want the binary name:
# set_target_properties(xtide-nearest PROPERTIES OUTPUT_NAME "xtide-nearest")
