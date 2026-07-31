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

if(LWEXT4_XATTR_ENABLE)
    message(FATAL_ERROR
        "CONFIG_LWEXT4_XATTR_ENABLE is unsupported: the GPL xattr source is intentionally excluded")
endif()

if(LWEXT4_EXTENTS_ENABLE)
    message(FATAL_ERROR
        "CONFIG_LWEXT4_EXTENTS_ENABLE is unsupported: the GPL extent source is intentionally excluded")
endif()

if(LWEXT4_EXT_FEATURE_SET_LVL GREATER 3)
    message(FATAL_ERROR
        "The ext4 feature set is unsafe without the excluded extent implementation")
endif()

set(LWEXT4_GENERATED_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/include")
file(MAKE_DIRECTORY "${LWEXT4_GENERATED_INCLUDE_DIR}/generated")

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/ext4_config.h.in"
    "${LWEXT4_GENERATED_INCLUDE_DIR}/generated/ext4_config.h"
    @ONLY
)

# This list is intentionally explicit. Do not replace it with a glob or
# aux_source_directory(): ext4_extent.c and ext4_xattr.c are GPL-licensed and
# form a deliberate source/binary boundary for this component.
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
    "${LWEXT4_PORT_DIR}/lwext4_xattr_stub.c"
)

add_library(lwext4 STATIC ${LWEXT4_SOURCES})
target_include_directories(lwext4 PUBLIC
    "${LWEXT4_SOURCE_DIR}/include"
    "${LWEXT4_GENERATED_INCLUDE_DIR}"
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
