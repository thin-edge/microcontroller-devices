# SPDX-License-Identifier: Apache-2.0
#
# OPC-UA protocol frontend: the open62541-based server and the address-space
# adapter that maps the shared device data model (lib/common) onto OPC-UA nodes.
# Compiled only when CONFIG_APP_OPCUA_SERVER is enabled.

if(CONFIG_APP_OPCUA_SERVER)
  zephyr_library()

  # OPC-UA server lifecycle + address-space adapter over lib/common's data model.
  zephyr_library_sources(opcua_server.c address_space.c)

  # open62541 OPC-UA stack, vendored as a pre-generated amalgamation
  # (third_party/open62541.{c,h}); see scripts/regen-open62541.sh.
  zephyr_library_sources(third_party/open62541.c)

  # The amalgamation is generated code; don't subject it to our warning flags.
  set_source_files_properties(third_party/open62541.c
    PROPERTIES COMPILE_OPTIONS "-w")

  # Export opcua_server.h/address_space.h and open62541.h to consumers.
  zephyr_include_directories(. third_party)
endif()
