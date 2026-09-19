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

if(CONFIG_SPIKE_TLS_MQTT)
  target_sources(app PRIVATE src/spike_time.c src/spike_mqtt.c)
  target_sources_ifdef(CONFIG_SPIKE_OTA app PRIVATE src/spike_ota.c)

  # Credentials are embedded as NUL-terminated PEM. The server trust anchor
  # is public and committed; device credentials live in the git-ignored
  # credentials/ directory.
  set(gen_dir ${ZEPHYR_BINARY_DIR}/include/generated/spike)
  generate_inc_file_for_target(app certs/godaddy-root-g2.pem
    ${gen_dir}/server_ca.pem.inc)

  if(CONFIG_SPIKE_OTA_EXTRA_CAS)
    generate_inc_file_for_target(app certs/usertrust-ecc.pem
      ${gen_dir}/usertrust_ecc.pem.inc)
    generate_inc_file_for_target(app certs/isrg-root-x1.pem
      ${gen_dir}/isrg_root_x1.pem.inc)
  endif()

  if(CONFIG_SPIKE_AUTH_CERT)
    set(id ${CONFIG_SPIKE_DEVICE_ID})
    foreach(ext crt key)
      set(f ${CMAKE_CURRENT_SOURCE_DIR}/credentials/${id}.${ext})
      if(NOT EXISTS ${f})
        message(FATAL_ERROR "Missing ${f}: issue a certificate for ${id} "
          "first (see credentials/README.md)")
      endif()
      generate_inc_file_for_target(app ${f} ${gen_dir}/device.${ext}.inc)
    endforeach()
  endif()
endif()
