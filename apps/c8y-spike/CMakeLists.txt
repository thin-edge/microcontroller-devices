# SPDX-License-Identifier: Apache-2.0
#
# Throwaway spike firmware for the c8y-direct-spikes change: measures TLS/MQTT
# to Cumulocity, OTA into slot1, Cumulocity CA enrollment and remote access.
# It stands in for a user application: lib/common brings Wi-Fi, identity and
# the data model, and tedge-zephyr is included the way any app would.
# Build with:
#   west build -b <board> apps/c8y-spike --sysbuild
cmake_minimum_required(VERSION 3.20.0)

list(APPEND ZEPHYR_EXTRA_MODULES
  ${CMAKE_CURRENT_SOURCE_DIR}/../../lib/common
  ${CMAKE_CURRENT_SOURCE_DIR}/../../tedge-zephyr
)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(c8y_spike_firmware C)

target_sources(app PRIVATE src/main.c)
