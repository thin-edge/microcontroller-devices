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
  zephyr_library_sources(
    src/tedge_version.c
    src/tedge_core.c
    src/tedge_platform.c
    src/tedge_time.c
    src/tedge_backoff.c
  )
  zephyr_library_sources_ifdef(CONFIG_TEDGE_TRANSPORT_C8Y
    src/tedge_c8y.c
    src/tedge_smartrest.c
  )
  zephyr_library_sources_ifdef(CONFIG_TEDGE_REMOTE_ACCESS
    src/tedge_remote_access.c
  )
  zephyr_library_sources_ifdef(CONFIG_TEDGE_AUTH_C8Y_CA
    src/tedge_enroll.c
    src/tedge_pkcs7.c
  )
  zephyr_library_sources_ifdef(CONFIG_TEDGE_AUTH_BOOTSTRAP src/tedge_bootstrap.c)
  zephyr_library_compile_definitions(TEDGE_VERSION_STRING="${TEDGE_VERSION}")

  zephyr_include_directories(include)

  # Trust anchors: every PEM in CONFIG_TEDGE_C8Y_CA_FILES is concatenated
  # into one bundle and embedded as tedge_ca_bundle.inc.
  if(CONFIG_TEDGE_TRANSPORT_C8Y)
    separate_arguments(_ca_files UNIX_COMMAND "${CONFIG_TEDGE_C8Y_CA_FILES}")
    set(_bundle "")
    foreach(_pem ${_ca_files})
      if(NOT IS_ABSOLUTE ${_pem})
        set(_pem ${CMAKE_CURRENT_LIST_DIR}/${_pem})
      endif()
      if(NOT EXISTS ${_pem})
        message(FATAL_ERROR "CONFIG_TEDGE_C8Y_CA_FILES: no such file: ${_pem}")
      endif()
      file(READ ${_pem} _content)
      string(APPEND _bundle "${_content}")
    endforeach()
    set(_bundle_pem ${ZEPHYR_BINARY_DIR}/misc/generated/tedge_ca_bundle.pem)
    file(WRITE ${_bundle_pem} "${_bundle}")
    generate_inc_file_for_target(tedge ${_bundle_pem}
      ${ZEPHYR_BINARY_DIR}/include/generated/tedge_ca_bundle.inc)
  endif()
endif()
