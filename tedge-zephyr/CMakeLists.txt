# SPDX-License-Identifier: Apache-2.0
#
# tedge-zephyr: thin-edge.io device-management client for any Zephyr
# application. Nothing is built unless the application enables CONFIG_TEDGE,
# so an image without it carries no code, RAM or threads from this module.
#
# This directory is self-contained: it must never reference files outside
# itself (scripts/check-independence.py enforces this).

if(CONFIG_TEDGE)
  file(STRINGS ${CMAKE_CURRENT_LIST_DIR}/VERSION TEDGE_VERSION LIMIT_COUNT 1)

  zephyr_library_named(tedge)
  zephyr_library_sources(src/tedge_version.c)
  zephyr_library_compile_definitions(TEDGE_VERSION_STRING="${TEDGE_VERSION}")

  zephyr_include_directories(include)
endif()
