# SPDX-License-Identifier: Apache-2.0
#
# Device-management agent firmware: the shared core (lib/common) and the
# thin-edge.io client (tedge-zephyr), with no protocol frontend. For a device
# whose job is to be managed from Cumulocity and to give it remote access to
# hosts on its LAN. Build with a tedge-zephyr profile and the board's
# settings (lib/common/tedge-boards/):
#   west build --sysbuild -b <board> apps/tedge-agent -- \
#     "-DEXTRA_CONF_FILE=<profile>.conf;<board settings>.conf"
cmake_minimum_required(VERSION 3.20.0)

list(APPEND ZEPHYR_EXTRA_MODULES
  ${CMAKE_CURRENT_SOURCE_DIR}/../../lib/common
  ${CMAKE_CURRENT_SOURCE_DIR}/../../tedge-zephyr
)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(tedge_agent_firmware C)

# The glue to the device-management client lives in the application, never in
# the module.
target_sources(app PRIVATE src/main.c src/tedge_glue.c)
