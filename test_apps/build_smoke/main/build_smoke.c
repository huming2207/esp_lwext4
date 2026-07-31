/*
 * SPDX-FileCopyrightText: 2026 esp_lwext4 contributors
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <ext4.h>
#include <ext4_debug.h>
#include "esp_lwext4.h"
#include "lwext4_port_bdl.h"

static esp_err_t (*volatile s_vfs_register_link_check)(
    const esp_vfs_lwext4_conf_t *) = esp_vfs_lwext4_register;
static esp_err_t (*volatile s_vfs_unregister_link_check)(
    const char *) = esp_vfs_lwext4_unregister;
static esp_err_t (*volatile s_vfs_rmdir_recurse_link_check)(
    const char *) = esp_vfs_lwext4_rmdir_recurse;
static esp_err_t (*volatile s_bdl_create_link_check)(
    esp_blockdev_handle_t, const lwext4_port_bdl_config_t *,
    lwext4_port_bdl_t **) = lwext4_port_bdl_create;
static struct ext4_blockdev *(*volatile s_bdl_get_link_check)(
    lwext4_port_bdl_t *) = lwext4_port_bdl_get;
static esp_err_t (*volatile s_bdl_sync_link_check)(
    lwext4_port_bdl_t *) = lwext4_port_bdl_sync;
static esp_err_t (*volatile s_bdl_last_error_link_check)(
    const lwext4_port_bdl_t *) = lwext4_port_bdl_last_error;
static esp_err_t (*volatile s_bdl_destroy_link_check)(
    lwext4_port_bdl_t *) = lwext4_port_bdl_destroy;

void app_main(void)
{
    /*
     * Referencing code in ext4.c makes the final firmware link validate the
     * BSD xattr stubs as well as the external archive itself.
     */
    ext4_dmask_set(0);
    (void)ext4_device_unregister("unused");
    (void)s_vfs_register_link_check;
    (void)s_vfs_unregister_link_check;
    (void)s_vfs_rmdir_recurse_link_check;
    (void)s_bdl_create_link_check;
    (void)s_bdl_get_link_check;
    (void)s_bdl_sync_link_check;
    (void)s_bdl_last_error_link_check;
    (void)s_bdl_destroy_link_check;
}
