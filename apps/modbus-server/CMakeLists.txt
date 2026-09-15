# SPDX-License-Identifier: Apache-2.0
#
# Modbus TCP server firmware: composes the shared core (lib/common, pump
# simulation) with the Modbus TCP frontend (lib/modbus). Build with:
#   west build -b <board> apps/modbus-server
cmake_minimum_required(VERSION 3.20.0)

list(APPEND ZEPHYR_EXTRA_MODULES
  ${CMAKE_CURRENT_SOURCE_DIR}/../../lib/common
  ${CMAKE_CURRENT_SOURCE_DIR}/../../lib/modbus
)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(modbus_server_firmware C)

target_sources(app PRIVATE src/main.c)
