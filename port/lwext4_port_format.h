/*
 * SPDX-FileCopyrightText: 2026 esp_lwext4 contributors
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef LWEXT4_PORT_FORMAT_H_
#define LWEXT4_PORT_FORMAT_H_

#include "esp_err.h"
#include "ext4_blockdev.h"
#include "lwext4_port_bdl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Format an lwext4 filesystem on an arbitrary block device.
 *
 * Shared by the BDL and SDMMC adapters. The device must not be in use.
 * Formatting is synchronous and can take a long time on large devices;
 * config->progress is called periodically so the caller can yield.
 */
esp_err_t lwext4_port_format_blockdev(struct ext4_blockdev *bdev, const lwext4_port_bdl_format_config_t *config);

#ifdef __cplusplus
}
#endif

#endif /* LWEXT4_PORT_FORMAT_H_ */
