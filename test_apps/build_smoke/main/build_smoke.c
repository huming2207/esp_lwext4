/*
 * SPDX-FileCopyrightText: 2026 esp_lwext4 contributors
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <ext4.h>
#include <ext4_debug.h>
#include "esp_lwext4.h"

static esp_err_t (*volatile s_vfs_register_link_check)(
    const esp_vfs_lwext4_conf_t *) = esp_vfs_lwext4_register;
static esp_err_t (*volatile s_vfs_unregister_link_check)(
    const char *) = esp_vfs_lwext4_unregister;

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
}
