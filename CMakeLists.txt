# SPDX-FileCopyrightText: 2026 esp_lwext4 contributors
# SPDX-License-Identifier: BSD-3-Clause

cmake_minimum_required(VERSION 3.16)

# ESP-IDF-facing adapters are compiled as the component library. lwext4 itself
# is compiled by the ExternalProject below and must not be listed here.
idf_component_register(
    SRCS
        "port/esp_lwext4.c"
        "port/lwext4_port_bdl.c"
        "port/lwext4_port_format.c"
        "port/lwext4_port_sdmmc.c"
    INCLUDE_DIRS
        "lwext4/include"
        "port"
    REQUIRES
        esp_blockdev vfs esp_blockdev_util heap sdmmc
)

include(ExternalProject)

idf_build_get_property(LWEXT4_IDF_PATH IDF_PATH)
idf_build_get_property(LWEXT4_IDF_CONFIG_DIR CONFIG_DIR)

set(LWEXT4_SOURCE_DIR "${COMPONENT_DIR}/lwext4")
set(LWEXT4_EXTERNAL_SOURCE_DIR "${COMPONENT_DIR}/cmake/lwext4")
set(LWEXT4_EXTERNAL_BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}/lwext4-build")
set(LWEXT4_LIBRARY "${LWEXT4_EXTERNAL_BINARY_DIR}/liblwext4.a")

# Imported targets reject non-existent include directories at generate time.
# The external project fills this directory with generated/ext4_config.h.
file(MAKE_DIRECTORY "${LWEXT4_EXTERNAL_BINARY_DIR}/include")

function(lwext4_bool_to_int output config_symbol)
    if(DEFINED ${config_symbol} AND ${config_symbol})
        set(${output} 1 PARENT_SCOPE)
    else()
        set(${output} 0 PARENT_SCOPE)
    endif()
endfunction()

lwext4_bool_to_int(LWEXT4_JOURNALING_ENABLE CONFIG_LWEXT4_JOURNALING_ENABLE)
lwext4_bool_to_int(LWEXT4_XATTR_ENABLE CONFIG_LWEXT4_XATTR_ENABLE)
lwext4_bool_to_int(LWEXT4_EXTENTS_ENABLE CONFIG_LWEXT4_EXTENTS_ENABLE)
lwext4_bool_to_int(LWEXT4_HAVE_OWN_ERRNO CONFIG_LWEXT4_HAVE_OWN_ERRNO)
lwext4_bool_to_int(LWEXT4_DEBUG_PRINTF CONFIG_LWEXT4_DEBUG_PRINTF)
lwext4_bool_to_int(LWEXT4_DEBUG_ASSERT CONFIG_LWEXT4_DEBUG_ASSERT)
lwext4_bool_to_int(LWEXT4_HAVE_OWN_ASSERT CONFIG_LWEXT4_HAVE_OWN_ASSERT)
lwext4_bool_to_int(LWEXT4_BLOCK_DEV_ENABLE_STATS CONFIG_LWEXT4_BLOCK_DEV_ENABLE_STATS)
lwext4_bool_to_int(LWEXT4_HAVE_OWN_OFLAGS CONFIG_LWEXT4_HAVE_OWN_OFLAGS)
lwext4_bool_to_int(LWEXT4_UNALIGNED_ACCESS CONFIG_LWEXT4_UNALIGNED_ACCESS)
lwext4_bool_to_int(LWEXT4_USE_INTERNAL_SRAM CONFIG_LWEXT4_USE_INTERNAL_SRAM)
lwext4_bool_to_int(LWEXT4_USE_PSRAM CONFIG_LWEXT4_USE_PSRAM)
lwext4_bool_to_int(LWEXT4_USE_PREFER_PSRAM CONFIG_LWEXT4_USE_PREFER_PSRAM)
lwext4_bool_to_int(LWEXT4_BIG_ENDIAN CONFIG_LWEXT4_BIG_ENDIAN)

# ext4_config.h derives CONFIG_SUPPORTED_*, CONFIG_DIR_INDEX_ENABLE,
# CONFIG_EXTENT_ENABLE, and CONFIG_META_CSUM_ENABLE from this value.
if(CONFIG_LWEXT4_FEATURE_SET_EXT2)
    set(LWEXT4_EXT_FEATURE_SET_LVL 2)
elseif(CONFIG_LWEXT4_FEATURE_SET_EXT3)
    set(LWEXT4_EXT_FEATURE_SET_LVL 3)
else()
    message(FATAL_ERROR "Select an lwext4 on-disk feature set in menuconfig")
endif()

ExternalProject_Add(lwext4_external
    PREFIX "${CMAKE_CURRENT_BINARY_DIR}/lwext4-prefix"
    SOURCE_DIR "${LWEXT4_EXTERNAL_SOURCE_DIR}"
    BINARY_DIR "${LWEXT4_EXTERNAL_BINARY_DIR}"
    CMAKE_ARGS
        "-DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}"
        "-DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}"
        "-DIDF_TARGET=${IDF_TARGET}"
        "-DLWEXT4_IDF_PATH=${LWEXT4_IDF_PATH}"
        "-DLWEXT4_IDF_CONFIG_DIR=${LWEXT4_IDF_CONFIG_DIR}"
        "-DLWEXT4_SOURCE_DIR=${LWEXT4_SOURCE_DIR}"
        "-DLWEXT4_PORT_DIR=${COMPONENT_DIR}/port"
        "-DLWEXT4_EXT_FEATURE_SET_LVL=${LWEXT4_EXT_FEATURE_SET_LVL}"
        "-DLWEXT4_JOURNALING_ENABLE=${LWEXT4_JOURNALING_ENABLE}"
        "-DLWEXT4_XATTR_ENABLE=${LWEXT4_XATTR_ENABLE}"
        "-DLWEXT4_EXTENTS_ENABLE=${LWEXT4_EXTENTS_ENABLE}"
        "-DLWEXT4_HAVE_OWN_ERRNO=${LWEXT4_HAVE_OWN_ERRNO}"
        "-DLWEXT4_DEBUG_PRINTF=${LWEXT4_DEBUG_PRINTF}"
        "-DLWEXT4_DEBUG_ASSERT=${LWEXT4_DEBUG_ASSERT}"
        "-DLWEXT4_HAVE_OWN_ASSERT=${LWEXT4_HAVE_OWN_ASSERT}"
        "-DLWEXT4_BLOCK_DEV_ENABLE_STATS=${LWEXT4_BLOCK_DEV_ENABLE_STATS}"
        "-DLWEXT4_BLOCK_DEV_CACHE_SIZE=${CONFIG_LWEXT4_BLOCK_DEV_CACHE_SIZE}"
        "-DLWEXT4_EXT4_MAX_BLOCKDEV_NAME=${CONFIG_LWEXT4_EXT4_MAX_BLOCKDEV_NAME}"
        "-DLWEXT4_EXT4_BLOCKDEVS_COUNT=${CONFIG_LWEXT4_EXT4_BLOCKDEVS_COUNT}"
        "-DLWEXT4_EXT4_MAX_MP_NAME=${CONFIG_LWEXT4_EXT4_MAX_MP_NAME}"
        "-DLWEXT4_EXT4_MOUNTPOINTS_COUNT=${CONFIG_LWEXT4_EXT4_MOUNTPOINTS_COUNT}"
        "-DLWEXT4_HAVE_OWN_OFLAGS=${LWEXT4_HAVE_OWN_OFLAGS}"
        "-DLWEXT4_MAX_TRUNCATE_SIZE=${CONFIG_LWEXT4_MAX_TRUNCATE_SIZE}"
        "-DLWEXT4_UNALIGNED_ACCESS=${LWEXT4_UNALIGNED_ACCESS}"
        "-DLWEXT4_USE_INTERNAL_SRAM=${LWEXT4_USE_INTERNAL_SRAM}"
        "-DLWEXT4_USE_PSRAM=${LWEXT4_USE_PSRAM}"
        "-DLWEXT4_USE_PREFER_PSRAM=${LWEXT4_USE_PREFER_PSRAM}"
        "-DLWEXT4_BIG_ENDIAN=0"
    INSTALL_COMMAND ""
    BUILD_ALWAYS TRUE
    BUILD_BYPRODUCTS "${LWEXT4_LIBRARY}"
    USES_TERMINAL_CONFIGURE TRUE
    USES_TERMINAL_BUILD TRUE
)

add_prebuilt_library(lwext4_archive "${LWEXT4_LIBRARY}")
add_dependencies(lwext4_archive lwext4_external)
target_include_directories(lwext4_archive INTERFACE
    "${LWEXT4_SOURCE_DIR}/include"
    "${LWEXT4_EXTERNAL_BINARY_DIR}/include"
)

add_dependencies(${COMPONENT_LIB} lwext4_external)
target_link_libraries(${COMPONENT_LIB} PUBLIC lwext4_archive)

set_property(DIRECTORY APPEND PROPERTY ADDITIONAL_CLEAN_FILES
    "${LWEXT4_EXTERNAL_BINARY_DIR}"
    "${CMAKE_CURRENT_BINARY_DIR}/lwext4-prefix"
)
