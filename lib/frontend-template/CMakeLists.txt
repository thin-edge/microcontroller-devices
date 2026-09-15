# SPDX-License-Identifier: Apache-2.0
# Protocol-frontend TEMPLATE. Compiles only if an app both composes this module
# and enables CONFIG_APP_FRONTEND_TEMPLATE (default n) — so it is inert by
# default and exists purely as a copy-me skeleton.

if(CONFIG_APP_FRONTEND_TEMPLATE)
  zephyr_library()
  zephyr_library_sources(template_frontend.c)
  zephyr_include_directories(.)
endif()
