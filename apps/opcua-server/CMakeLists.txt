# SPDX-License-Identifier: Apache-2.0
#
# OPC-UA server firmware: composes the shared core (lib/common) with the OPC-UA
# protocol frontend (lib/opcua). Build with:
#   west build -b <board> apps/opcua-server
cmake_minimum_required(VERSION 3.20.0)

# Pull in the in-repo Zephyr modules this firmware is composed from. Must be set
# before find_package(Zephyr). Each module contributes its sources (via
# zephyr_library) and Kconfig. An app that needs a different protocol simply
# lists a different lib/<protocol> module here.
list(APPEND ZEPHYR_EXTRA_MODULES
  ${CMAKE_CURRENT_SOURCE_DIR}/../../lib/common
  ${CMAKE_CURRENT_SOURCE_DIR}/../../lib/opcua
  # thin-edge.io device management; off unless CONFIG_TEDGE=y.
  ${CMAKE_CURRENT_SOURCE_DIR}/../../tedge-zephyr
)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(opcua_server_firmware C)

target_sources(app PRIVATE src/main.c)
# The glue to the device-management client lives in the application, never in
# the module.
target_sources_ifdef(CONFIG_TEDGE app PRIVATE src/tedge_glue.c)
