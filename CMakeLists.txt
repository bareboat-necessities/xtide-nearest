cmake_minimum_required(VERSION 3.20)

project(xtide-nearest VERSION 0.2.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(XTN_WARNINGS_AS_ERRORS "Treat warnings as errors" OFF)

# --- deps (CLI11 via CPM) ---
include(cmake/CPM.cmake)

CPMAddPackage(
  NAME CLI11
  GITHUB_REPOSITORY CLIUtils/CLI11
  VERSION 2.4.2
  OPTIONS
    "CLI11_BUILD_TESTS OFF"
    "CLI11_BUILD_EXAMPLES OFF"
    "CLI11_BUILD_DOCS OFF"
)

# --- libtcd (FindTCD.cmake) ---
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
find_package(TCD REQUIRED)

add_executable(xtide-nearest
  src/main.cpp
)

target_link_libraries(xtide-nearest PRIVATE CLI11::CLI11 TCD::tcd)

target_compile_features(xtide-nearest PRIVATE cxx_std_20)

if (MSVC)
  target_compile_options(xtide-nearest PRIVATE /W4)
  if (XTN_WARNINGS_AS_ERRORS)
    target_compile_options(xtide-nearest PRIVATE /WX)
  endif()
else()
  target_compile_options(xtide-nearest PRIVATE -Wall -Wextra -Wpedantic)
  if (XTN_WARNINGS_AS_ERRORS)
    target_compile_options(xtide-nearest PRIVATE -Werror)
  endif()
endif()

install(TARGETS xtide-nearest RUNTIME DESTINATION bin)
