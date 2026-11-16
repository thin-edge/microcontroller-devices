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

# tiny-edge comes from the component manager: main/idf_component.yml.

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(tedge-agent-idf)
