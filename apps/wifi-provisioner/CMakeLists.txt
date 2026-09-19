# SPDX-License-Identifier: Apache-2.0
#
# Wi-Fi provisioner: Improv Wi-Fi over BLE in its own image, launched by
# MCUboot from prov_partition. Built as an extra sysbuild image by each app's
# sysbuild.cmake (see sysbuild/provisioning.cmake); not built on its own.
cmake_minimum_required(VERSION 3.20.0)

list(APPEND ZEPHYR_EXTRA_MODULES
  ${CMAKE_CURRENT_SOURCE_DIR}/../../lib/common
)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(wifi_provisioner C)

target_sources(app PRIVATE src/main.c)
if(CONFIG_APP_WIFI_PROV_SOFTAP)
  target_sources(app PRIVATE src/softap.c)
else()
  target_sources(app PRIVATE src/improv.c)
endif()
