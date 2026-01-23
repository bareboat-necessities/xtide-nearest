# FindTCD.cmake - minimal finder for libtcd (XTide)
#
# Exports:
#   TCD_FOUND
#   TCD_LIBRARIES
#   TCD_INCLUDE_DIRS

find_path(TCD_INCLUDE_DIR
  NAMES tide_db.h
  PATH_SUFFIXES libtcd include
)

find_library(TCD_LIBRARY
  NAMES tcd libtcd
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(TCD
  REQUIRED_VARS TCD_INCLUDE_DIR TCD_LIBRARY
)

if(TCD_FOUND)
  set(TCD_INCLUDE_DIRS "${TCD_INCLUDE_DIR}")
  set(TCD_LIBRARIES "${TCD_LIBRARY}")
endif()
