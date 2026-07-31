/*
 * SPDX-FileCopyrightText: 2026 esp_lwext4 contributors
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "lwext4_port_bdl.h"

#include <errno.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "ext4_errno.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

struct lwext4_port_bdl {
    struct ext4_blockdev ext4;
    struct ext4_blockdev_iface iface;
    esp_blockdev_handle_t lower;
    SemaphoreHandle_t lock;
    uint8_t *transfer_buffer;
    size_t transfer_buffer_size;
    uint32_t transfer_buffer_blocks;
    esp_err_t last_lower_error;
    bool read_only;
    bool sync_after_write;
};

static const char *TAG = "lwext4_bdl";

static lwext4_port_bdl_t *adapter_from_bdev(struct ext4_blockdev *bdev)
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

static int save_lower_result(lwext4_port_bdl_t *adapter, esp_err_t error)
{
    adapter->last_lower_error = error;
    return esp_error_to_lwext4(error);
}

static bool valid_lower_handle(const lwext4_port_bdl_t *adapter)
{
    return adapter != NULL && adapter->lower != NULL && adapter->lower->ops != NULL;
}

static esp_err_t sync_lower(lwext4_port_bdl_t *adapter)
{
    esp_err_t error = ESP_OK;

    if (!valid_lower_handle(adapter)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (adapter->lower->ops->sync != NULL) {
        error = adapter->lower->ops->sync(adapter->lower);
    }
    adapter->last_lower_error = error;
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "BDL sync failed: %s (0x%x)", esp_err_to_name(error), (unsigned)error);
    }
    return error;
}

static int bdl_open(struct ext4_blockdev *bdev)
{
    lwext4_port_bdl_t *adapter = adapter_from_bdev(bdev);

    if (!valid_lower_handle(adapter) || adapter->lower->ops->read == NULL ||
        (!adapter->read_only && adapter->lower->ops->write == NULL)) {
        return EIO;
    }
    return EOK;
}

static int validate_io_range(const lwext4_port_bdl_t *adapter, uint64_t block_id, uint32_t block_count, uint64_t *byte_address)
{
    uint64_t block_total = adapter->iface.ph_bcnt;

    if (block_count == 0) {
        *byte_address = 0;
        return EOK;
    }
    if (block_id >= block_total || (uint64_t)block_count > block_total - block_id) {
        return EINVAL;
    }
    if ((size_t)block_count > SIZE_MAX / (size_t)adapter->iface.ph_bsize) {
        return EOVERFLOW;
    }

    *byte_address = block_id * adapter->iface.ph_bsize;
    return EOK;
}

static int bdl_read(struct ext4_blockdev *bdev, void *buffer, uint64_t block_id, uint32_t block_count)
{
    lwext4_port_bdl_t *adapter = adapter_from_bdev(bdev);
    uint8_t *destination = buffer;
    uint64_t address;
    size_t block_size;
    uint32_t blocks_remaining;
    int result;

    if (!valid_lower_handle(adapter) || buffer == NULL || adapter->lower->ops->read == NULL) {
        return EINVAL;
    }
    result = validate_io_range(adapter, block_id, block_count, &address);
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
        esp_err_t error = adapter->lower->ops->read(adapter->lower, adapter->transfer_buffer, adapter->transfer_buffer_size,
                                                    address, chunk_size);
        result = save_lower_result(adapter, error);
        if (result != EOK) {
            ESP_LOGE(TAG, "BDL read failed at 0x%016" PRIx64 ", size=%zu: %s (0x%x)", address, chunk_size, esp_err_to_name(error),
                     (unsigned)error);
            return result;
        }
        if (destination != adapter->transfer_buffer) {
            memcpy(destination, adapter->transfer_buffer, chunk_size);
        }
        destination += chunk_size;
        address += chunk_size;
        blocks_remaining -= chunk_blocks;
    }
    return EOK;
}

static int bdl_write(struct ext4_blockdev *bdev, const void *buffer, uint64_t block_id, uint32_t block_count)
{
    lwext4_port_bdl_t *adapter = adapter_from_bdev(bdev);
    const uint8_t *source = buffer;
    uint64_t address;
    size_t block_size;
    uint32_t blocks_remaining;
    int result;

    if (!valid_lower_handle(adapter) || buffer == NULL) {
        return EINVAL;
    }
    if (adapter->read_only || adapter->lower->device_flags.read_only) {
        return EROFS;
    }
    if (adapter->lower->ops->write == NULL) {
        return ENOTSUP;
    }
    result = validate_io_range(adapter, block_id, block_count, &address);
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
        esp_err_t error = adapter->lower->ops->write(adapter->lower, adapter->transfer_buffer, address, chunk_size);
        result = save_lower_result(adapter, error);
        if (result != EOK) {
            ESP_LOGE(TAG, "BDL write failed at 0x%016" PRIx64 ", size=%zu: %s (0x%x)", address, chunk_size,
                     esp_err_to_name(error), (unsigned)error);
            return result;
        }
        source += chunk_size;
        address += chunk_size;
        blocks_remaining -= chunk_blocks;
    }

    if (adapter->sync_after_write) {
        return esp_error_to_lwext4(sync_lower(adapter));
    }
    return EOK;
}

static int bdl_close(struct ext4_blockdev *bdev)
{
    lwext4_port_bdl_t *adapter = adapter_from_bdev(bdev);

    if (!valid_lower_handle(adapter)) {
        return EIO;
    }
    return esp_error_to_lwext4(sync_lower(adapter));
}

static int bdl_lock(struct ext4_blockdev *bdev)
{
    lwext4_port_bdl_t *adapter = adapter_from_bdev(bdev);

    if (adapter == NULL || adapter->lock == NULL || xSemaphoreTakeRecursive(adapter->lock, portMAX_DELAY) != pdTRUE) {
        return EIO;
    }
    return EOK;
}

static int bdl_unlock(struct ext4_blockdev *bdev)
{
    lwext4_port_bdl_t *adapter = adapter_from_bdev(bdev);

    if (adapter == NULL || adapter->lock == NULL || xSemaphoreGiveRecursive(adapter->lock) != pdTRUE) {
        return EIO;
    }
    return EOK;
}

static bool is_power_of_two(uint32_t value)
{
    return value != 0 && (value & (value - 1)) == 0;
}

static esp_err_t validate_create_args(esp_blockdev_handle_t lower, const lwext4_port_bdl_config_t *config,
                                      lwext4_port_bdl_t **out_adapter)
{
    if (out_adapter == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_adapter = NULL;

    if (lower == NULL || config == NULL || lower->ops == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (lower->ops->read == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (lower->geometry.disk_size == 0 || lower->geometry.read_size == 0 || !is_power_of_two(config->physical_block_size) ||
        config->buffer_caps == 0 || (config->buffer_caps & MALLOC_CAP_8BIT) == 0 || !is_power_of_two(config->buffer_alignment) ||
        config->physical_block_size % config->buffer_alignment != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (config->physical_block_size % lower->geometry.read_size != 0 ||
        lower->geometry.disk_size % config->physical_block_size != 0) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (!config->read_only) {
        if (lower->device_flags.read_only) {
            return ESP_ERR_INVALID_STATE;
        }
        if (lower->ops->write == NULL) {
            return ESP_ERR_NOT_SUPPORTED;
        }
        if (lower->geometry.write_size == 0 || config->physical_block_size % lower->geometry.write_size != 0) {
            return ESP_ERR_INVALID_SIZE;
        }
        if (!config->lower_device_supports_rewrite) {
            return ESP_ERR_NOT_SUPPORTED;
        }
    }
    if (config->sync_after_write && lower->ops->sync == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
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

esp_err_t lwext4_port_bdl_create(esp_blockdev_handle_t lower, const lwext4_port_bdl_config_t *config,
                                 lwext4_port_bdl_t **out_adapter)
{
    lwext4_port_bdl_t *adapter;
    esp_err_t error = validate_create_args(lower, config, out_adapter);

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

    adapter->lower = lower;
    adapter->last_lower_error = ESP_OK;
    adapter->read_only = config->read_only;
    adapter->sync_after_write = config->sync_after_write;
    adapter->iface.open = bdl_open;
    adapter->iface.bread = bdl_read;
    adapter->iface.bwrite = bdl_write;
    adapter->iface.close = bdl_close;
    adapter->iface.lock = bdl_lock;
    adapter->iface.unlock = bdl_unlock;
    adapter->iface.ph_bsize = config->physical_block_size;
    adapter->iface.ph_bcnt = lower->geometry.disk_size / config->physical_block_size;
    adapter->iface.ph_bbuf = adapter->transfer_buffer;
    adapter->iface.p_user = adapter;
    adapter->ext4.bdif = &adapter->iface;
    adapter->ext4.part_offset = 0;
    adapter->ext4.part_size = lower->geometry.disk_size;

    *out_adapter = adapter;
    return ESP_OK;
}

struct ext4_blockdev *lwext4_port_bdl_get(lwext4_port_bdl_t *adapter)
{
    return adapter != NULL ? &adapter->ext4 : NULL;
}

esp_err_t lwext4_port_bdl_sync(lwext4_port_bdl_t *adapter)
{
    esp_err_t error;

    if (adapter == NULL || adapter->lock == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTakeRecursive(adapter->lock, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }
    error = sync_lower(adapter);
    xSemaphoreGiveRecursive(adapter->lock);
    return error;
}

esp_err_t lwext4_port_bdl_last_error(const lwext4_port_bdl_t *adapter)
{
    return adapter != NULL ? adapter->last_lower_error : ESP_ERR_INVALID_ARG;
}

esp_err_t lwext4_port_bdl_destroy(lwext4_port_bdl_t *adapter)
{
    esp_err_t error;

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

    error = sync_lower(adapter);
    xSemaphoreGiveRecursive(adapter->lock);
    if (error != ESP_OK) {
        return error;
    }

    vSemaphoreDelete(adapter->lock);
    heap_caps_free(adapter->transfer_buffer);
    free(adapter);
    return ESP_OK;
}
