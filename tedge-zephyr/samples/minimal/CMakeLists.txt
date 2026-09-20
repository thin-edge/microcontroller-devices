# SPDX-License-Identifier: Apache-2.0
#
# Smallest application that uses tedge-zephyr. It builds with Zephyr and this
# module only, which proves the module does not depend on the repository that
# hosts it.
cmake_minimum_required(VERSION 3.20.0)

# The module is this sample's grandparent directory. Once tedge-zephyr is a
# west project, west finds it and this line can go.
list(APPEND ZEPHYR_EXTRA_MODULES ${CMAKE_CURRENT_SOURCE_DIR}/../..)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(tedge_minimal C)

target_sources(app PRIVATE src/main.c)
