# SPDX-License-Identifier: Apache-2.0
#
# MCUboot hooks that launch the Wi-Fi provisioner from its own partition when
# a boot request asks for it. Added to the MCUboot image only (sysbuild passes
# this module to it); every other image ignores it.
if(CONFIG_MCUBOOT AND CONFIG_BOOT_GO_HOOKS)
  zephyr_library()
  zephyr_library_sources(prov_hooks.c)
  zephyr_library_link_libraries(MCUBOOT_BOOTUTIL)
  # The hook builds a loader state for image validation (boot_read_sectors
  # and friends are declared in bootutil's private header).
  zephyr_library_include_directories(${ZEPHYR_MCUBOOT_MODULE_DIR}/boot/bootutil/src)
  # boot_request.h: the record layout shared with the apps.
  zephyr_library_include_directories(${CMAKE_CURRENT_LIST_DIR}/../common)
endif()
