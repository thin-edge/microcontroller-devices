# SPDX-License-Identifier: Apache-2.0
#
# Unit tests for the module's pure helpers, run on native_sim:
#   west build -b native_sim/native/64 tedge-zephyr/tests/unit -t run
#
# The helpers have no state, no I/O and no Kconfig of their own, so the test
# compiles them directly instead of building the whole client.
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(tedge_unit_tests C)

target_sources(app PRIVATE
  src/main.c
  ../../src/tedge_smartrest.c
  ../../src/tedge_backoff.c
  ../../src/tedge_pkcs7.c
  ../../src/tedge_ra_allow_list.c
  ../../src/tedge_url.c
  ../../src/tedge_cert_decision.c
  ../../src/tedge_telemetry.c
  ../../src/tedge_json.c
  ../../src/tedge_shell_allow_list.c
  ../../src/tedge_log_ring.c
  ../../src/tedge_op_json.c
  ../../src/tedge_parameters.c
  ../../src/tedge_otp.c
  ../../src/tedge_fw_decision.c
)

# The telemetry API is compiled in for the tests, with a small buffer so the
# "it filled up" cases are quick to reach.
target_compile_definitions(app PRIVATE
  CONFIG_TEDGE_TELEMETRY=1
  CONFIG_TEDGE_TELEMETRY_BUFFER_BYTES=512
  # The module's log level comes from its Kconfig, which is not part of a
  # test build.
  CONFIG_TEDGE_LOG_LEVEL=3
  # A small ring, so "it wrapped" is one test away.
  CONFIG_TEDGE_LOG_RING_BYTES=256
  CONFIG_TEDGE_C8Y_OPERATION_JSON=1
  # Parameters: room for the mixed set the tests declare, and the
  # schema printer, which is what the tenant registration needs.
  CONFIG_TEDGE_PARAMETERS=1
  CONFIG_TEDGE_PARAMETERS_MAX=8
  CONFIG_TEDGE_PARAMETERS_SCHEMA=1
)
target_include_directories(app PRIVATE ../../include ../../src)
