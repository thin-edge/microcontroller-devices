# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(opcua_server_firmware C)

target_sources(app PRIVATE src/main.c)

# Networking / connectivity management (Wi-Fi on hardware, host backend on
# native_sim) and mDNS/DNS-SD advertisement.
target_sources(app PRIVATE src/net.c)

# NSOS (native_sim offloaded sockets) link shim — provides zsock_getnameinfo(),
# which Zephyr omits under CONFIG_NET_SOCKETS_OFFLOAD. Not built on hardware.
target_sources_ifdef(CONFIG_NET_SOCKETS_OFFLOAD app PRIVATE src/nsos_compat.c)

# OPC-UA server, address space and data source. Compiled only when the
# OPC-UA server is enabled so the base skeleton builds standalone.
target_sources_ifdef(CONFIG_APP_OPCUA_SERVER app PRIVATE
  src/opcua_server.c
  src/address_space.c
  src/data_source.c
)

# open62541 OPC-UA stack, vendored as a pre-generated amalgamation
# (third_party/open62541/open62541.{c,h}); see scripts/regen-open62541.sh for
# how it was produced (minimal, read-only, POSIX-architecture profile).
if(CONFIG_APP_OPCUA_SERVER)
  target_include_directories(app PRIVATE third_party/open62541)
  target_sources(app PRIVATE third_party/open62541/open62541.c)

  # The amalgamation is generated code; don't subject it to our warning flags.
  set_source_files_properties(third_party/open62541/open62541.c
    PROPERTIES COMPILE_OPTIONS "-w")
endif()
