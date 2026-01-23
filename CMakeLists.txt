cmake_minimum_required(VERSION 3.20)
project(xtide_nearest LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(XTIDE_NEAREST_WARNINGS_AS_ERRORS "Treat warnings as errors" OFF)

if (MSVC)
  add_compile_options(/W4)
  if (XTIDE_NEAREST_WARNINGS_AS_ERRORS)
    add_compile_options(/WX)
  endif()
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
  if (XTIDE_NEAREST_WARNINGS_AS_ERRORS)
    add_compile_options(-Werror)
  endif()
endif()

# ---- deps: CLI11 via CPM ----
set(CPM_DOWNLOAD_LOCATION "${CMAKE_CURRENT_LIST_DIR}/third_party/CPM.cmake")
if(NOT EXISTS "${CPM_DOWNLOAD_LOCATION}")
  file(DOWNLOAD
    "https://github.com/cpm-cmake/CPM.cmake/releases/latest/download/CPM.cmake"
    "${CPM_DOWNLOAD_LOCATION}"
  )
endif()
include(${CPM_DOWNLOAD_LOCATION})

CPMAddPackage(
  NAME CLI11
  GITHUB_REPOSITORY CLIUtils/CLI11
  VERSION 2.4.2
)

# ---- libtcd (XTide) ----
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")
find_package(TCD REQUIRED)

add_executable(xtide-nearest
  src/main.cpp
)

target_link_libraries(xtide-nearest
  PRIVATE
    CLI11::CLI11
    ${TCD_LIBRARIES}
)

target_include_directories(xtide-nearest
  PRIVATE
    ${TCD_INCLUDE_DIRS}
)

# On some distros libtcd needs math library explicitly
if (UNIX AND NOT APPLE)
  target_link_libraries(xtide-nearest PRIVATE m)
endif()
