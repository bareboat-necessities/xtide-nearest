# FindTCD.cmake
#
# Provides:
#   TCD_FOUND
#   TCD_INCLUDE_DIR
#   TCD_LIBRARY
#   TCD::tcd (imported target)

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(PC_TCD QUIET libtcd)
endif()

find_path(TCD_INCLUDE_DIR
  NAMES tcd.h
  HINTS
    ${PC_TCD_INCLUDEDIR}
    ${PC_TCD_INCLUDE_DIRS}
  PATH_SUFFIXES include
)

find_library(TCD_LIBRARY
  NAMES tcd libtcd
  HINTS
    ${PC_TCD_LIBDIR}
    ${PC_TCD_LIBRARY_DIRS}
  PATH_SUFFIXES lib
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(TCD
  REQUIRED_VARS TCD_INCLUDE_DIR TCD_LIBRARY
)

if(TCD_FOUND AND NOT TARGET TCD::tcd)
  add_library(TCD::tcd UNKNOWN IMPORTED)
  set_target_properties(TCD::tcd PROPERTIES
    IMPORTED_LOCATION "${TCD_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${TCD_INCLUDE_DIR}"
  )

  if(PC_TCD_CFLAGS_OTHER)
    set_property(TARGET TCD::tcd APPEND PROPERTY
      INTERFACE_COMPILE_OPTIONS "${PC_TCD_CFLAGS_OTHER}"
    )
  endif()

  if(PC_TCD_LDFLAGS_OTHER)
    set_property(TARGET TCD::tcd APPEND PROPERTY
      INTERFACE_LINK_OPTIONS "${PC_TCD_LDFLAGS_OTHER}"
    )
  endif()
endif()
