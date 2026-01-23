# third_party/CPM.cmake
# Minimal bootstrap to fetch CPM.cmake if it isn't already present.

set(CPM_DOWNLOAD_VERSION 0.40.2)

set(CPM_DOWNLOAD_LOCATION "${CMAKE_CURRENT_LIST_DIR}/CPM.cmake")
if(NOT EXISTS "${CPM_DOWNLOAD_LOCATION}")
  file(DOWNLOAD
    "https://github.com/cpm-cmake/CPM.cmake/releases/download/v${CPM_DOWNLOAD_VERSION}/CPM.cmake"
    "${CPM_DOWNLOAD_LOCATION}"
    TLS_VERIFY ON
  )
endif()

include("${CPM_DOWNLOAD_LOCATION}")
