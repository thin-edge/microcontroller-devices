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
  )
  zephyr_library_sources_ifdef(CONFIG_TEDGE_TRANSPORT_C8Y src/tedge_c8y.c)
  zephyr_library_compile_definitions(TEDGE_VERSION_STRING="${TEDGE_VERSION}")

  zephyr_include_directories(include)

  # Trust anchors: each PEM in CONFIG_TEDGE_C8Y_CA_FILES is embedded as an
  # .inc that src/tedge_c8y.c includes (CA_INC_FILES).
  if(CONFIG_TEDGE_TRANSPORT_C8Y)
    set(_ca_incs "")
    separate_arguments(_ca_files UNIX_COMMAND "${CONFIG_TEDGE_C8Y_CA_FILES}")
    foreach(_pem ${_ca_files})
      if(NOT IS_ABSOLUTE ${_pem})
        set(_pem ${CMAKE_CURRENT_LIST_DIR}/${_pem})
      endif()
      if(NOT EXISTS ${_pem})
        message(FATAL_ERROR "CONFIG_TEDGE_C8Y_CA_FILES: no such file: ${_pem}")
      endif()
      get_filename_component(_name ${_pem} NAME_WE)
      generate_inc_file_for_target(tedge ${_pem}
        ${ZEPHYR_BINARY_DIR}/include/generated/tedge_ca_${_name}.inc)
      list(APPEND _ca_incs "\"tedge_ca_${_name}.inc\"")
    endforeach()
    string(REPLACE ";" "," _ca_incs_list "${_ca_incs}")
    zephyr_library_compile_definitions(TEDGE_CA_INC_FILES=${_ca_incs_list})
  endif()
endif()
