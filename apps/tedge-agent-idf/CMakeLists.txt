# SPDX-License-Identifier: Apache-2.0
#
# The device agent on ESP-IDF: Wi-Fi, the thin-edge.io client (tiny-edge),
# and the device's own telemetry, with no protocol server. The device is set
# up over USB with tedge-onboard.
#
#   idf.py set-target esp32c6
#   idf.py build flash monitor

cmake_minimum_required(VERSION 3.22)

# The version comes from VERSION, the file release-please bumps in every
# application (Zephyr's format): MAJOR.MINOR.PATCH, and -EXTRAVERSION when
# scripts/release/check-version.sh sets one.
foreach(_key VERSION_MAJOR VERSION_MINOR PATCHLEVEL EXTRAVERSION)
  file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/VERSION" _line REGEX "^${_key} *=")
  string(REGEX REPLACE "^${_key} *= *([^ #]*).*$" "\\1" _${_key} "${_line}")
endforeach()
set(PROJECT_VER "${_VERSION_MAJOR}.${_VERSION_MINOR}.${_PATCHLEVEL}")
if(_EXTRAVERSION)
  string(APPEND PROJECT_VER "-${_EXTRAVERSION}")
endif()

# tiny-edge, pinned to a release. The whole repository is fetched, not only
# its ESP-IDF component: the component builds the portable core and the
# shared ports from beside it, which the component manager's git+path
# dependency does not bring along. TINY_EDGE_DIR (environment or -D) uses a
# local checkout instead, to try a client change.
set(TINY_EDGE_VERSION v0.5.0)
if(NOT TINY_EDGE_DIR AND DEFINED ENV{TINY_EDGE_DIR})
  set(TINY_EDGE_DIR $ENV{TINY_EDGE_DIR})
endif()
if(NOT TINY_EDGE_DIR)
  include(FetchContent)
  FetchContent_Declare(tiny_edge
    GIT_REPOSITORY https://github.com/thin-edge/tiny-edge.git
    GIT_TAG ${TINY_EDGE_VERSION}
    GIT_SHALLOW TRUE
    # Only the sources: tiny-edge has no top-level project to add.
    SOURCE_SUBDIR do-not-add)
  FetchContent_MakeAvailable(tiny_edge)
  set(TINY_EDGE_DIR ${tiny_edge_SOURCE_DIR})
endif()
message(STATUS "tiny-edge: ${TINY_EDGE_DIR}")
set(EXTRA_COMPONENT_DIRS ${TINY_EDGE_DIR}/ports/freertos/esp-idf/tiny_edge)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(tedge-agent-idf)
