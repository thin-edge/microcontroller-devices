# SPDX-License-Identifier: Apache-2.0
#
# Sysbuild for boards with BLE Wi-Fi provisioning, included by each app's
# sysbuild.cmake. One `west build --sysbuild` builds three images that share a
# flash layout (lib/common/dts/layout-*.dtsi):
#
#   mcuboot           the bootloader, with the hooks in lib/mcuboot-hooks that
#                     launch the provisioner when the boot request asks for it
#   <app>             the application, signed, for slot0
#   wifi-provisioner  apps/wifi-provisioner, signed, for prov_partition
#
# Images are signed with MCUboot's public development key (sysbuild.conf); not
# for production. scripts/flash.sh writes all three at their offsets.

set(_repo ${CMAKE_CURRENT_LIST_DIR}/..)

# Board target -> layout. Every board here needs the app, the provisioner and
# MCUboot to agree on it; the app's own board overlay includes the same file.
set(_board "${SB_CONFIG_BOARD}/${SB_CONFIG_BOARD_QUALIFIERS}")
if(_board STREQUAL "esp32c6_devkitc/esp32c6/hpcore")
  set(_layout ${_repo}/lib/common/dts/layout-esp32c6-4M.dtsi)
elseif(_board STREQUAL "esp32s3_devkitc/esp32s3/procpu")
  set(_layout ${_repo}/lib/common/dts/layout-esp32s3-16M.dtsi)
elseif(_board STREQUAL "adafruit_qt_py_esp32s3/esp32s3/procpu")
  set(_layout ${_repo}/lib/common/dts/layout-esp32s3-4M.dtsi)
elseif(_board STREQUAL "esp32_devkitc/esp32/procpu")
  set(_layout ${_repo}/lib/common/dts/layout-esp32-4M.dtsi)
else()
  message(FATAL_ERROR
    "${_board} has no MCUboot + Wi-Fi provisioner layout. Build it without "
    "--sysbuild (simple boot, compile-time credentials), or add a layout in "
    "lib/common/dts/ and list the board in sysbuild/provisioning.cmake.")
endif()

if(NOT SB_CONFIG_BOOTLOADER_MCUBOOT)
  message(FATAL_ERROR "sysbuild.conf must enable SB_CONFIG_BOOTLOADER_MCUBOOT")
endif()

# --- MCUboot: layout + boot-request hooks ---
set(mcuboot_EXTRA_DTC_OVERLAY_FILE ${_layout} CACHE INTERNAL "" FORCE)
set(mcuboot_EXTRA_ZEPHYR_MODULES ${_repo}/lib/mcuboot-hooks CACHE INTERNAL "" FORCE)
set(mcuboot_EXTRA_CONF_FILE ${_repo}/lib/mcuboot-hooks/mcuboot.conf CACHE INTERNAL "" FORCE)

# --- The Wi-Fi provisioner, signed like the application ---
ExternalZephyrProject_Add(
  APPLICATION wifi-provisioner
  SOURCE_DIR ${_repo}/apps/wifi-provisioner
  BOARD ${SB_CONFIG_BOARD}/${SB_CONFIG_BOARD_QUALIFIERS}
)
set_config_bool(wifi-provisioner CONFIG_BOOTLOADER_MCUBOOT y)
set_config_string(wifi-provisioner CONFIG_MCUBOOT_SIGNATURE_KEY_FILE
                  "${SB_CONFIG_BOOT_SIGNATURE_KEY_FILE}")
set_config_bool(wifi-provisioner CONFIG_MCUBOOT_GENERATE_UNSIGNED_IMAGE n)
set_config_bool(wifi-provisioner CONFIG_MCUBOOT_BOOTLOADER_MODE_SWAP_SCRATCH y)
add_dependencies(wifi-provisioner mcuboot)
