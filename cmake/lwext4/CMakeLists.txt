# SPDX-FileCopyrightText: 2026 esp_lwext4 contributors
# SPDX-License-Identifier: BSD-3-Clause

cmake_minimum_required(VERSION 3.16)
project(lwext4_external C)

if(NOT IS_DIRECTORY "${LWEXT4_SOURCE_DIR}")
    message(FATAL_ERROR "LWEXT4_SOURCE_DIR does not identify the lwext4 source tree")
endif()

if(NOT IS_DIRECTORY "${LWEXT4_PORT_DIR}")
    message(FATAL_ERROR "LWEXT4_PORT_DIR does not identify the ESP-IDF port sources")
endif()

if(NOT IS_DIRECTORY "${LWEXT4_IDF_PATH}/components/heap/include")
    message(FATAL_ERROR "LWEXT4_IDF_PATH does not identify an ESP-IDF tree with heap headers")
endif()

if(NOT IS_DIRECTORY "${LWEXT4_IDF_CONFIG_DIR}")
    message(FATAL_ERROR "LWEXT4_IDF_CONFIG_DIR does not identify the generated ESP-IDF configuration directory")
endif()

if(LWEXT4_XATTR_ENABLE)
    message(FATAL_ERROR
        "CONFIG_LWEXT4_XATTR_ENABLE is unsupported: the GPL xattr source is intentionally excluded")
endif()

if(LWEXT4_EXT_FEATURE_SET_LVL GREATER 3 AND NOT LWEXT4_EXTENTS_ENABLE)
    message(FATAL_ERROR
        "The ext4 feature set requires an extents implementation "
        "(CONFIG_LWEXT4_EXTENTS_MIT or CONFIG_LWEXT4_EXTENTS_UPSTREAM)")
endif()

math(EXPR LWEXT4_USE_HEAP_MODE_COUNT
    "${LWEXT4_USE_INTERNAL_SRAM}+${LWEXT4_USE_PSRAM}+${LWEXT4_USE_PREFER_PSRAM}")
if(NOT LWEXT4_USE_HEAP_MODE_COUNT EQUAL 1)
    message(FATAL_ERROR "Select exactly one ESP lwext4 heap allocation mode")
endif()

set(LWEXT4_GENERATED_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/include")
file(MAKE_DIRECTORY "${LWEXT4_GENERATED_INCLUDE_DIR}/generated")

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/ext4_config.h.in"
    "${LWEXT4_GENERATED_INCLUDE_DIR}/generated/ext4_config.h"
    @ONLY
)

# This list is intentionally explicit. Do not replace it with a glob or
# aux_source_directory(): ext4_xattr.c is GPL-licensed and forms a deliberate
# source/binary boundary for this component. The extent implementation is
# selected by the Kconfig extents choice.
set(LWEXT4_SOURCES
    "${LWEXT4_SOURCE_DIR}/src/ext4.c"
    "${LWEXT4_SOURCE_DIR}/src/ext4_balloc.c"
    "${LWEXT4_SOURCE_DIR}/src/ext4_bcache.c"
    "${LWEXT4_SOURCE_DIR}/src/ext4_bitmap.c"
    "${LWEXT4_SOURCE_DIR}/src/ext4_block_group.c"
    "${LWEXT4_SOURCE_DIR}/src/ext4_blockdev.c"
    "${LWEXT4_SOURCE_DIR}/src/ext4_crc32.c"
    "${LWEXT4_SOURCE_DIR}/src/ext4_debug.c"
    "${LWEXT4_SOURCE_DIR}/src/ext4_dir.c"
    "${LWEXT4_SOURCE_DIR}/src/ext4_dir_idx.c"
    "${LWEXT4_SOURCE_DIR}/src/ext4_fs.c"
    "${LWEXT4_SOURCE_DIR}/src/ext4_hash.c"
    "${LWEXT4_SOURCE_DIR}/src/ext4_ialloc.c"
    "${LWEXT4_SOURCE_DIR}/src/ext4_inode.c"
    "${LWEXT4_SOURCE_DIR}/src/ext4_journal.c"
    "${LWEXT4_SOURCE_DIR}/src/ext4_mbr.c"
    "${LWEXT4_SOURCE_DIR}/src/ext4_mkfs.c"
    "${LWEXT4_SOURCE_DIR}/src/ext4_super.c"
    "${LWEXT4_SOURCE_DIR}/src/ext4_trans.c"
    "${LWEXT4_PORT_DIR}/lwext4_port_esp_heap.c"
    "${LWEXT4_PORT_DIR}/lwext4_xattr_stub.c"
)
if(LWEXT4_EXTENT_SOURCE)
    list(APPEND LWEXT4_SOURCES "${LWEXT4_EXTENT_SOURCE}")
endif()

add_library(lwext4 STATIC ${LWEXT4_SOURCES})
target_include_directories(lwext4 PUBLIC
    "${LWEXT4_SOURCE_DIR}/include"
    "${LWEXT4_GENERATED_INCLUDE_DIR}"
)
target_include_directories(lwext4 PRIVATE
    "${LWEXT4_PORT_DIR}"
    "${LWEXT4_IDF_PATH}/components/heap/include"
    "${LWEXT4_IDF_PATH}/components/esp_common/include"
    "${LWEXT4_IDF_CONFIG_DIR}"
)
target_compile_definitions(lwext4 PRIVATE CONFIG_USE_DEFAULT_CFG=0)
target_compile_options(lwext4 PRIVATE
    -Wall
    -Wextra
    -Wno-unused-parameter
)
set_target_properties(lwext4 PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED YES
    C_EXTENSIONS YES
)
