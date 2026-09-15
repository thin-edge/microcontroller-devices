# SPDX-License-Identifier: Apache-2.0
#
# Modbus TCP protocol frontend: a TCP listener on the configured port feeding
# Zephyr's in-tree modbus subsystem (raw-ADU server), with object callbacks that
# map the shared device data model (lib/common) onto Modbus registers/coils.

if(CONFIG_APP_MODBUS_SERVER)
  zephyr_library()
  zephyr_library_sources(modbus_server.c)
  zephyr_include_directories(.)
endif()
