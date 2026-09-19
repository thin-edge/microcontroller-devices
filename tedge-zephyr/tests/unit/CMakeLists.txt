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
)
target_include_directories(app PRIVATE ../../include ../../src)
