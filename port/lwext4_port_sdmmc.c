/*
 * SPDX-FileCopyrightText: 2026 esp_lwext4 contributors
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "lwext4_port_sdmmc.h"

#include <errno.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "ext4_config.h"
#include "ext4_errno.h"
#include "lwext4_port_format.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

struct lwext4_port_sdmmc {
    struct ext4_blockdev ext4;
    struct ext4_blockdev_iface iface;
    sdmmc_card_t *card;
    SemaphoreHandle_t lock;
    uint8_t *transfer_buffer;
    size_t transfer_buffer_size;
    uint32_t transfer_buffer_blocks;
};

static const char *TAG = "lwext4_sdmmc";

static lwext4_port_sdmmc_t *adapter_from_bdev(struct ext4_blockdev *bdev)
{
    if (bdev == NULL || bdev->bdif == NULL) {
        return NULL;
    }
    return bdev->bdif->p_user;
}

static int esp_error_to_lwext4(esp_err_t error)
{
    switch (error) {
    case ESP_OK:
        return EOK;
    case ESP_ERR_INVALID_ARG:
    case ESP_ERR_INVALID_SIZE:
    case ESP_ERR_INVALID_STATE:
        return EINVAL;
    case ESP_ERR_NO_MEM:
        return ENOMEM;
    case ESP_ERR_NOT_SUPPORTED:
        return ENOTSUP;
    case ESP_ERR_NOT_FOUND:
        return ENOENT;
    default:
        return EIO;
    }
}

static bool valid_card(const lwext4_port_sdmmc_t *adapter)
{
    return adapter != NULL && adapter->card != NULL && adapter->card->csd.sector_size > 0 && adapter->card->csd.capacity > 0;
}

static uint64_t device_size(const lwext4_port_sdmmc_t *adapter)
{
    return (uint64_t)adapter->card->csd.capacity * adapter->card->csd.sector_size;
}

static int sdmmc_open(struct ext4_blockdev *bdev)
{
    return valid_card(adapter_from_bdev(bdev)) ? EOK : EIO;
}

static int sdmmc_close(struct ext4_blockdev *bdev)
{
    /* SDMMC writes complete synchronously; nothing to flush. */
    return valid_card(adapter_from_bdev(bdev)) ? EOK : EIO;
}

static int validate_io_range(const lwext4_port_sdmmc_t *adapter, uint64_t block_id, uint32_t block_count, uint64_t *start_sector)
{
    uint64_t total_sectors = (uint64_t)adapter->card->csd.capacity;

    if (block_count == 0) {
        *start_sector = 0;
        return EOK;
    }
    if (block_id >= total_sectors || (uint64_t)block_count > total_sectors - block_id) {
        return EINVAL;
    }
    if ((uint64_t)block_count > SIZE_MAX / adapter->iface.ph_bsize) {
        return EOVERFLOW;
    }
    if (block_id > SIZE_MAX || (uint64_t)block_count > SIZE_MAX - (size_t)block_id) {
        return EOVERFLOW;
    }
    *start_sector = block_id;
    return EOK;
}

static int sdmmc_read(struct ext4_blockdev *bdev, void *buffer, uint64_t block_id, uint32_t block_count)
{
    lwext4_port_sdmmc_t *adapter = adapter_from_bdev(bdev);
    uint8_t *destination = buffer;
    uint64_t sector;
    size_t block_size;
    uint32_t blocks_remaining;
    int result;

    if (!valid_card(adapter) || buffer == NULL) {
        return EINVAL;
    }
    result = validate_io_range(adapter, block_id, block_count, &sector);
    if (result != EOK || block_count == 0) {
        return result;
    }
    if (buffer == adapter->transfer_buffer && block_count > adapter->transfer_buffer_blocks) {
        return EINVAL;
    }

    block_size = adapter->iface.ph_bsize;
    blocks_remaining = block_count;
    while (blocks_remaining != 0) {
        uint32_t chunk_blocks = blocks_remaining;
        if (chunk_blocks > adapter->transfer_buffer_blocks) {
            chunk_blocks = adapter->transfer_buffer_blocks;
        }
        size_t chunk_size = (size_t)chunk_blocks * block_size;
        esp_err_t error = sdmmc_read_sectors(adapter->card, adapter->transfer_buffer, (size_t)sector, chunk_blocks);
        if (error != ESP_OK) {
            ESP_LOGE(TAG, "sdmmc_read_sectors failed at sector %" PRIu64 ", count=%" PRIu32 ": %s (0x%x)", sector, chunk_blocks,
                     esp_err_to_name(error), (unsigned)error);
            return esp_error_to_lwext4(error);
        }
        if (destination != adapter->transfer_buffer) {
            memcpy(destination, adapter->transfer_buffer, chunk_size);
        }
        destination += chunk_size;
        sector += chunk_blocks;
        blocks_remaining -= chunk_blocks;
    }
    return EOK;
}

static int sdmmc_write(struct ext4_blockdev *bdev, const void *buffer, uint64_t block_id, uint32_t block_count)
{
    lwext4_port_sdmmc_t *adapter = adapter_from_bdev(bdev);
    const uint8_t *source = buffer;
    uint64_t sector;
    size_t block_size;
    uint32_t blocks_remaining;
    int result;

    if (!valid_card(adapter) || buffer == NULL) {
        return EINVAL;
    }
    result = validate_io_range(adapter, block_id, block_count, &sector);
    if (result != EOK || block_count == 0) {
        return result;
    }
    if (buffer == adapter->transfer_buffer && block_count > adapter->transfer_buffer_blocks) {
        return EINVAL;
    }

    block_size = adapter->iface.ph_bsize;
    blocks_remaining = block_count;
    while (blocks_remaining != 0) {
        uint32_t chunk_blocks = blocks_remaining;
        if (chunk_blocks > adapter->transfer_buffer_blocks) {
            chunk_blocks = adapter->transfer_buffer_blocks;
        }
        size_t chunk_size = (size_t)chunk_blocks * block_size;
        if (source != adapter->transfer_buffer) {
            memcpy(adapter->transfer_buffer, source, chunk_size);
        }
        esp_err_t error = sdmmc_write_sectors(adapter->card, adapter->transfer_buffer, (size_t)sector, chunk_blocks);
        if (error != ESP_OK) {
            ESP_LOGE(TAG, "sdmmc_write_sectors failed at sector %" PRIu64 ", count=%" PRIu32 ": %s (0x%x)", sector, chunk_blocks,
                     esp_err_to_name(error), (unsigned)error);
            return esp_error_to_lwext4(error);
        }
        source += chunk_size;
        sector += chunk_blocks;
        blocks_remaining -= chunk_blocks;
    }
    return EOK;
}

static int sdmmc_lock(struct ext4_blockdev *bdev)
{
    lwext4_port_sdmmc_t *adapter = adapter_from_bdev(bdev);

    if (adapter == NULL || adapter->lock == NULL || xSemaphoreTakeRecursive(adapter->lock, portMAX_DELAY) != pdTRUE) {
        return EIO;
    }
    return EOK;
}

static int sdmmc_unlock(struct ext4_blockdev *bdev)
{
    lwext4_port_sdmmc_t *adapter = adapter_from_bdev(bdev);

    if (adapter == NULL || adapter->lock == NULL || xSemaphoreGiveRecursive(adapter->lock) != pdTRUE) {
        return EIO;
    }
    return EOK;
}

static bool is_power_of_two(uint32_t value)
{
    return value != 0 && (value & (value - 1)) == 0;
}

static esp_err_t validate_create_args(sdmmc_card_t *card, const lwext4_port_sdmmc_config_t *config,
                                      lwext4_port_sdmmc_t **out_adapter)
{
    if (out_adapter == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_adapter = NULL;

    if (card == NULL || config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (card->csd.sector_size == 0 || card->csd.capacity <= 0) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (config->physical_block_size != (uint32_t)card->csd.sector_size) {
        /* lwext4 physical blocks map one-to-one to SD sectors. */
        return ESP_ERR_INVALID_SIZE;
    }
    if (!is_power_of_two(config->physical_block_size) || config->buffer_caps == 0 ||
        (config->buffer_caps & MALLOC_CAP_8BIT) == 0 || !is_power_of_two(config->buffer_alignment) ||
        config->physical_block_size % config->buffer_alignment != 0) {
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t transfer_buffer_blocks = config->transfer_buffer_blocks;
    if (transfer_buffer_blocks == 0) {
        transfer_buffer_blocks = 1;
    }
    if ((size_t)transfer_buffer_blocks > SIZE_MAX / (size_t)config->physical_block_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

esp_err_t lwext4_port_sdmmc_create(sdmmc_card_t *card, const lwext4_port_sdmmc_config_t *config,
                                   lwext4_port_sdmmc_t **out_adapter)
{
    lwext4_port_sdmmc_t *adapter;
    esp_err_t error = validate_create_args(card, config, out_adapter);

    if (error != ESP_OK) {
        return error;
    }

    adapter = calloc(1, sizeof(*adapter));
    if (adapter == NULL) {
        return ESP_ERR_NO_MEM;
    }
    adapter->transfer_buffer_blocks = config->transfer_buffer_blocks;
    if (adapter->transfer_buffer_blocks == 0) {
        adapter->transfer_buffer_blocks = 1;
    }
    adapter->transfer_buffer_size = (size_t)adapter->transfer_buffer_blocks * (size_t)config->physical_block_size;
    adapter->transfer_buffer =
        heap_caps_aligned_alloc(config->buffer_alignment, adapter->transfer_buffer_size, config->buffer_caps);
    adapter->lock = xSemaphoreCreateRecursiveMutex();
    if (adapter->transfer_buffer == NULL || adapter->lock == NULL) {
        if (adapter->lock != NULL) {
            vSemaphoreDelete(adapter->lock);
        }
        heap_caps_free(adapter->transfer_buffer);
        free(adapter);
        return ESP_ERR_NO_MEM;
    }

    adapter->card = card;
    adapter->iface.open = sdmmc_open;
    adapter->iface.bread = sdmmc_read;
    adapter->iface.bwrite = sdmmc_write;
    adapter->iface.close = sdmmc_close;
    adapter->iface.lock = sdmmc_lock;
    adapter->iface.unlock = sdmmc_unlock;
    adapter->iface.ph_bsize = config->physical_block_size;
    adapter->iface.ph_bcnt = (uint64_t)card->csd.capacity;
    adapter->iface.ph_bbuf = adapter->transfer_buffer;
    adapter->iface.p_user = adapter;
    adapter->ext4.bdif = &adapter->iface;
    adapter->ext4.part_offset = 0;
    adapter->ext4.part_size = device_size(adapter);

    *out_adapter = adapter;
    return ESP_OK;
}

struct ext4_blockdev *lwext4_port_sdmmc_get(lwext4_port_sdmmc_t *adapter)
{
    return adapter != NULL ? &adapter->ext4 : NULL;
}

esp_err_t lwext4_port_sdmmc_sync(lwext4_port_sdmmc_t *adapter)
{
    if (adapter == NULL || adapter->lock == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTakeRecursive(adapter->lock, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }
    /* SDMMC writes complete synchronously; nothing to flush. */
    if (xSemaphoreGiveRecursive(adapter->lock) != pdTRUE) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t validate_format_args(lwext4_port_sdmmc_t *adapter, const lwext4_port_bdl_format_config_t *config)
{
    uint32_t block_size;

    if (adapter == NULL || config == NULL || !valid_card(adapter)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (adapter->iface.ph_refctr != 0) {
        return ESP_ERR_INVALID_STATE;
    }
    if (config->feature_set != 0 && config->feature_set != F_SET_EXT2 && config->feature_set != F_SET_EXT3) {
        return ESP_ERR_INVALID_ARG;
    }

    block_size = config->block_size != 0 ? config->block_size : 4096;
    if (!is_power_of_two(block_size) || block_size % (uint32_t)adapter->card->csd.sector_size != 0 ||
        adapter->ext4.part_size % block_size != 0) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

esp_err_t lwext4_port_sdmmc_format(lwext4_port_sdmmc_t *adapter, const lwext4_port_bdl_format_config_t *config)
{
    esp_err_t error = validate_format_args(adapter, config);

    if (error != ESP_OK) {
        return error;
    }
    return lwext4_port_format_blockdev(&adapter->ext4, config);
}

esp_err_t lwext4_port_sdmmc_destroy(lwext4_port_sdmmc_t *adapter)
{
    if (adapter == NULL || adapter->lock == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTakeRecursive(adapter->lock, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }
    if (adapter->iface.ph_refctr != 0) {
        xSemaphoreGiveRecursive(adapter->lock);
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreGiveRecursive(adapter->lock);

    vSemaphoreDelete(adapter->lock);
    heap_caps_free(adapter->transfer_buffer);
    free(adapter);
    return ESP_OK;
}
