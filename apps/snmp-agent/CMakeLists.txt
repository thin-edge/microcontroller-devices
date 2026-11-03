# SPDX-License-Identifier: Apache-2.0
#
# SNMP agent firmware: composes the shared core (lib/common, switch/router
# simulation) with the SNMPv2c agent frontend (lib/snmp). Build with:
#   west build -b <board> apps/snmp-agent
cmake_minimum_required(VERSION 3.20.0)

list(APPEND ZEPHYR_EXTRA_MODULES
  ${CMAKE_CURRENT_SOURCE_DIR}/../../lib/common
  ${CMAKE_CURRENT_SOURCE_DIR}/../../lib/snmp
)
# thin-edge.io device management (tiny-edge, off unless CONFIG_TEDGE=y) is a
# west project, so west adds it as a module; see west.yml.

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(snmp_agent_firmware C)

target_sources(app PRIVATE src/main.c)
# The glue to the device-management client lives in the application, never in
# the module.
target_sources_ifdef(CONFIG_TEDGE app PRIVATE src/tedge_glue.c)
