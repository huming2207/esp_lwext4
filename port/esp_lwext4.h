/*
 * SPDX-FileCopyrightText: 2026 esp_lwext4 contributors
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Configuration for registering an already-mounted lwext4 filesystem
 *        with ESP-IDF VFS.
 *
 * This adapter does not register a block device, mount lwext4, start its
 * journal, or take ownership of the lwext4 mount. The caller must keep the
 * lwext4 mount alive until esp_vfs_lwext4_unregister() succeeds.
 */
typedef struct {
    /**
     * ESP-IDF VFS prefix, for example "/data".
     *
     * The usual ESP-IDF VFS restrictions apply: it must start with '/', must
     * not end with '/', and must fit in ESP_VFS_PATH_MAX characters.
     */
    const char *base_path;

    /**
     * Existing lwext4 mount point, for example "/" or "/ext".
     *
     * A trailing slash is optional. The adapter normalizes it internally.
     */
    const char *mount_point;

    /** Maximum number of files which may be open through this VFS instance. */
    size_t max_files;

    /**
     * Register the VFS as read-only.
     *
     * This must agree with the mode used when calling ext4_mount().
     */
    bool read_only;
} esp_vfs_lwext4_conf_t;

/**
 * @brief Register an existing lwext4 mount with ESP-IDF VFS.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG for an invalid configuration
 *      - ESP_ERR_INVALID_STATE if the VFS prefix or lwext4 mount is already
 *        registered here, or the lwext4 mount point is not mounted
 *      - ESP_ERR_NO_MEM if adapter state cannot be allocated
 *      - another ESP-IDF VFS registration error
 */
esp_err_t esp_vfs_lwext4_register(const esp_vfs_lwext4_conf_t *conf);

/**
 * @brief Unregister an lwext4 VFS instance.
 *
 * This does not unmount lwext4. Unregistration fails while files or
 * directories opened through this VFS instance remain open.
 *
 * @param base_path VFS prefix previously passed to
 *                  esp_vfs_lwext4_register().
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if base_path is invalid
 *      - ESP_ERR_INVALID_STATE if the prefix is unknown or is still in use
 *      - another ESP-IDF VFS unregistration error
 */
esp_err_t esp_vfs_lwext4_unregister(const char *base_path);

/**
 * @brief Recursively remove a directory tree on a registered lwext4 VFS.
 *
 * Unlike POSIX rmdir(), this function removes all files and subdirectories
 * below the requested directory by using lwext4's recursive ext4_dir_rm()
 * operation.
 *
 * The path must be an absolute ESP-IDF VFS path below a mount registered by
 * esp_vfs_lwext4_register(), for example "/data/cache". The function rejects
 * paths belonging to another filesystem, the registered mount root, paths
 * containing "." or ".." components, read-only mounts, and mounts with files
 * or directories currently open through this VFS adapter.
 *
 * This function does not recognize lwext4 handles opened directly outside the
 * VFS adapter. The application must synchronize and close such handles before
 * recursively removing a tree.
 *
 * @param path Absolute path below a registered lwext4 VFS prefix.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if path is invalid, identifies the mount root,
 *        contains a dot component, or does not identify a directory
 *      - ESP_ERR_NOT_SUPPORTED if path is not handled by a registered lwext4
 *        VFS instance
 *      - ESP_ERR_NOT_FOUND if the directory does not exist
 *      - ESP_ERR_INVALID_STATE if the mount is read-only or has open VFS
 *        handles
 *      - ESP_ERR_NO_MEM if temporary state cannot be allocated
 *      - ESP_FAIL for another lwext4 failure
 */
esp_err_t esp_vfs_lwext4_rmdir_recurse(const char *path);

#ifdef __cplusplus
}
#endif
