# SPDX-License-Identifier: Apache-2.0
#
# ZTP provisioner unit tests on native_sim: the canonical EnrollRequest against
# the Go canonicaliser, the text response/manifest parsers against output of
# the Go renderer, and the p256 crypto against the Go vectors.
#
#   west build -b native_sim/native/64 tests/ztp_provisioner -t run
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ztp_provisioner_test C)

set(prov ${CMAKE_CURRENT_SOURCE_DIR}/../../apps/wifi-provisioner/src)
target_include_directories(app PRIVATE ${prov})
target_sources(app PRIVATE
  src/test_ztp.c
  ${prov}/ztp_manifest.c
  ${prov}/ztp_envelope.c
  ${prov}/ztp_crypto.c
)
