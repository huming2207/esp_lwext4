/*
 * SPDX-FileCopyrightText: 2026 esp_lwext4 contributors
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef LWEXT4_PORT_SDMMC_H_
#define LWEXT4_PORT_SDMMC_H_

#include <stdint.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "ext4_blockdev.h"
#include "lwext4_port_bdl.h"
#include "sdmmc_cmd.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct lwext4_port_sdmmc lwext4_port_sdmmc_t;

/**
 * @brief Configuration for an lwext4 adapter that drives SDMMC directly.
 *
 * This adapter bypasses ESP-IDF's byte-addressed SDMMC esp_blockdev, which
 * narrows byte addresses to size_t on 32-bit targets and therefore corrupts
 * media above 4 GiB (esp-idf#18875). It calls sdmmc_read_sectors() and
 * sdmmc_write_sectors() with sector numbers, so full card capacity is safe.
 */
typedef struct {
    /**
     * Physical block size exposed to lwext4.
     *
     * Must equal card->csd.sector_size so that one lwext4 physical block is
     * one SD sector and block IDs translate directly to sector numbers.
     */
    uint32_t physical_block_size;

    /**
     * ESP-IDF heap capabilities for the physical-block transfer buffer.
     *
     * MALLOC_CAP_8BIT is required, plus capabilities required by the SDMMC
     * DMA engine, such as MALLOC_CAP_DMA and MALLOC_CAP_INTERNAL.
     */
    uint32_t buffer_caps;

    /**
     * Required byte alignment of the physical-block transfer buffer.
     *
     * Must be a nonzero power of two that divides physical_block_size.
     */
    uint32_t buffer_alignment;

    /**
     * Number of physical blocks held by the aligned transfer buffer.
     *
     * Contiguous requests are split into chunks of at most this many sectors.
     * Zero selects one block for backward compatible behavior.
     */
    uint32_t transfer_buffer_blocks;
} lwext4_port_sdmmc_config_t;

/**
 * @brief Create an lwext4 block-device adapter over an initialized SDMMC card.
 *
 * The adapter borrows @p card. It never deinitializes the card, which must
 * remain valid until after lwext4_port_sdmmc_destroy() succeeds. The card
 * must be exclusively owned by this adapter: no other code may issue direct
 * sdmmc_* operations on the same card while the adapter exists.
 */
esp_err_t lwext4_port_sdmmc_create(sdmmc_card_t *card, const lwext4_port_sdmmc_config_t *config,
                                   lwext4_port_sdmmc_t **out_adapter);

/**
 * @brief Get the lwext4 block-device descriptor owned by an adapter.
 *
 * The returned pointer remains owned by the adapter.
 */
struct ext4_blockdev *lwext4_port_sdmmc_get(lwext4_port_sdmmc_t *adapter);

/**
 * @brief No-op sync.
 *
 * SDMMC writes complete synchronously, so there is nothing to flush.
 */
esp_err_t lwext4_port_sdmmc_sync(lwext4_port_sdmmc_t *adapter);

/**
 * @brief Format the card behind an adapter.
 *
 * The adapter must not be in use: formatting fails with
 * ESP_ERR_INVALID_STATE while ext4_mount holds the device. Formatting is
 * synchronous and can take a long time on large cards; supply progress to
 * keep the scheduler alive. With CONFIG_LWEXT4_FORMAT_ROUND_TO_FULL_GROUPS
 * enabled, cards smaller than one block group (128 MiB for 4096-byte blocks)
 * cannot be formatted.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG for an invalid configuration
 *      - ESP_ERR_INVALID_STATE if the device is mounted
 *      - ESP_ERR_INVALID_SIZE for a geometry or block-size mismatch
 *      - ESP_ERR_NO_MEM if the temporary filesystem state cannot be allocated
 *      - ESP_FAIL if ext4_mkfs fails
 */
esp_err_t lwext4_port_sdmmc_format(lwext4_port_sdmmc_t *adapter, const lwext4_port_bdl_format_config_t *config);

/**
 * @brief Destroy an adapter without deinitializing the SDMMC card.
 *
 * Returns ESP_ERR_INVALID_STATE while lwext4 still references the block
 * device. The caller must unmount and unregister the lwext4 device before
 * destroying the adapter. Complete quiescence is required: no other task may
 * be executing an adapter operation or waiting on its lock when destroy is
 * called, because the mutex and adapter are freed immediately after the final
 * unlock.
 */
esp_err_t lwext4_port_sdmmc_destroy(lwext4_port_sdmmc_t *adapter);

#ifdef __cplusplus
}
#endif

#endif /* LWEXT4_PORT_SDMMC_H_ */
