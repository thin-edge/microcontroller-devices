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
# Exactly one provisioning protocol (the APP_PROV_PROTOCOL choice).
if(CONFIG_APP_WIFI_PROV_SOFTAP)
  target_sources(app PRIVATE src/softap.c)
elseif(CONFIG_APP_PROV_ZTP)
  target_sources(app PRIVATE
    src/ztp.c
    src/ztp_envelope.c
    src/ztp_manifest.c
    src/ztp_apply.c
  )
else()
  target_sources(app PRIVATE src/improv.c)
endif()
target_sources_ifdef(CONFIG_APP_PROV_ZTP_CRYPTO app PRIVATE src/ztp_crypto.c)
target_sources_ifdef(CONFIG_APP_PROV_ZTP_CRYPTO_SELFTEST app PRIVATE src/ztp_selftest.c)
