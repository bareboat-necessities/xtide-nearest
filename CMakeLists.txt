cmake_minimum_required(VERSION 3.20)
project(xtide-nearest VERSION 0.2.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(XTN_WARNINGS_AS_ERRORS "Treat warnings as errors" OFF)

include(FetchContent)

# ---- CLI11 ----
FetchContent_Declare(
  CLI11
  GIT_REPOSITORY https://github.com/CLIUtils/CLI11.git
  GIT_TAG v2.4.2
)
FetchContent_MakeAvailable(CLI11)

# ---- libtcd ----
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")
find_package(TCD REQUIRED)

add_executable(xtide-nearest
  src/main.cpp
)

target_link_libraries(xtide-nearest PRIVATE CLI11::CLI11 TCD::tcd)

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
