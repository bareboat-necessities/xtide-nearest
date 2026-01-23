# cmake/FindTCD.cmake
# Finds libtcd (XTide Tide Constituent Database library)
#
# Provides:
#   TCD_FOUND
#   TCD_INCLUDE_DIR
#   TCD_LIBRARY
#   TCD::TCD (imported target)

find_path(TCD_INCLUDE_DIR
  NAMES tcd.h
  HINTS
    ${TCD_ROOT} $ENV{TCD_ROOT}
    ${TCD_DIR}  $ENV{TCD_DIR}
  PATHS
    /usr
    /usr/local
  PATH_SUFFIXES
    include
)

find_library(TCD_LIBRARY
  NAMES tcd libtcd
  HINTS
    ${TCD_ROOT} $ENV{TCD_ROOT}
    ${TCD_DIR}  $ENV{TCD_DIR}
  PATHS
    /usr
    /usr/local
  PATH_SUFFIXES
    lib lib64
    lib/x86_64-linux-gnu
    lib/aarch64-linux-gnu
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(TCD
  REQUIRED_VARS TCD_INCLUDE_DIR TCD_LIBRARY
)

if (TCD_FOUND AND NOT TARGET TCD::TCD)
  add_library(TCD::TCD UNKNOWN IMPORTED)
  set_target_properties(TCD::TCD PROPERTIES
    IMPORTED_LOCATION "${TCD_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${TCD_INCLUDE_DIR}"
  )
endif()
