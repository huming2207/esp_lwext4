/*
 * SPDX-FileCopyrightText: 2026 esp_lwext4 contributors
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef LWEXT4_PORT_BDL_H_
#define LWEXT4_PORT_BDL_H_

#include <stdbool.h>
#include <stdint.h>

#include "esp_blockdev.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "ext4_blockdev.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct lwext4_port_bdl lwext4_port_bdl_t;

/**
 * @brief Configuration for an lwext4-to-ESP-IDF BDL adapter.
 */
typedef struct {
    /**
     * Physical block size exposed to lwext4.
     *
     * This value must be a power of two, a multiple of the lower device's
     * read size and (for writable adapters) write size, and must divide the
     * lower device's disk size exactly.
     */
    uint32_t physical_block_size;

    /**
     * ESP-IDF heap capabilities for the physical-block transfer buffer.
     *
     * MALLOC_CAP_8BIT is required. Add capabilities required by the lower
     * driver, such as MALLOC_CAP_DMA and MALLOC_CAP_INTERNAL for an SDMMC
     * target that cannot DMA from PSRAM.
     */
    uint32_t buffer_caps;

    /**
     * Required byte alignment of the physical-block transfer buffer.
     *
     * This must be a nonzero power of two and divide physical_block_size.
     * Supply the alignment required by the selected lower driver and target.
     */
    uint32_t buffer_alignment;

    /**
     * Number of physical blocks held by the aligned transfer buffer.
     *
     * Contiguous reads and writes are sent to the lower BDL in chunks of at
     * most this many blocks. Values greater than one allow devices such as SD
     * cards to use multi-block commands. Zero selects one block for backward
     * compatible behavior.
     */
    uint32_t transfer_buffer_blocks;

    /** Reject all writes through this adapter. */
    bool read_only;

    /**
     * Call the lower BDL sync operation after every successful lwext4 write.
     *
     * Creation fails if this is true and the lower device has no sync
     * operation.
     */
    bool sync_after_write;

    /**
     * Caller attestation that the lower BDL supports ordinary overwrites.
     *
     * This must be true for writable adapters. Raw erase-before-write flash
     * is not suitable for lwext4; use a rewrite-capable translation layer
     * such as wear levelling first. This is explicit because some stacked
     * BDL implementations preserve lower-device flags.
     */
    bool lower_device_supports_rewrite;
} lwext4_port_bdl_config_t;

/**
 * @brief Configuration for formatting an lwext4 filesystem on a BDL adapter.
 *
 * Zero-valued numeric fields select the lwext4 defaults: 4096-byte blocks,
 * 256-byte inodes, and capacity-derived inode and journal sizes. The
 * capacity-derived defaults can be very large on big devices, so supply
 * explicit values for production use.
 */
typedef struct {
    /**
     * Filesystem block size in bytes.
     *
     * Must be a power of two, a multiple of the lower device's read and
     * write sizes, and divide the device size exactly. Zero selects the
     * lwext4 default (4096).
     */
    uint32_t block_size;

    /** Inode size in bytes. Zero selects the lwext4 default (256). */
    uint32_t inode_size;

    /**
     * Total inode count.
     *
     * Zero selects the lwext4 capacity-derived default, which can be
     * enormous on large cards.
     */
    uint32_t inodes;

    /**
     * Journal size in filesystem blocks.
     *
     * Zero selects the lwext4 default. Ignored when journal is false.
     */
    uint32_t journal_blocks;

    /** Create a journaled filesystem. */
    bool journal;

    /** Optional volume label. NULL selects an empty label. */
    const char *label;

    /**
     * lwext4 feature set: F_SET_EXT2 or F_SET_EXT3.
     *
     * Zero derives the value from the component's LWEXT4_FEATURE_SET
     * configuration. F_SET_EXT4 is not supported by this build.
     */
    uint32_t feature_set;

    /**
     * Optional formatting progress callback.
     *
     * Called approximately every 4096 physical blocks with the total bytes
     * read and written so far. The callback may yield to the scheduler or
     * report progress. May be NULL.
     */
    void (*progress)(void *arg, uint64_t bytes_read, uint64_t bytes_written);

    /** Argument passed to progress. */
    void *progress_arg;
} lwext4_port_bdl_format_config_t;

/**
 * @brief Create an lwext4 block-device adapter over an ESP-IDF BDL handle.
 *
 * The adapter borrows @p lower. It never releases the lower handle, which
 * must remain valid until after lwext4_port_bdl_destroy() succeeds.
 */
esp_err_t lwext4_port_bdl_create(esp_blockdev_handle_t lower, const lwext4_port_bdl_config_t *config,
                                 lwext4_port_bdl_t **out_adapter);

/**
 * @brief Get the lwext4 block-device descriptor owned by an adapter.
 *
 * The returned pointer remains owned by the adapter.
 */
struct ext4_blockdev *lwext4_port_bdl_get(lwext4_port_bdl_t *adapter);

/**
 * @brief Flush the lower BDL, if it supplies a sync operation.
 */
esp_err_t lwext4_port_bdl_sync(lwext4_port_bdl_t *adapter);

/**
 * @brief Format the device behind an adapter.
 *
 * The adapter must not be in use: formatting fails with
 * ESP_ERR_INVALID_STATE while ext4_mount holds the device. The lower BDL is
 * synced before and after formatting. Formatting is synchronous and can take
 * a long time on large devices; supply progress to keep the scheduler alive.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG for an invalid configuration
 *      - ESP_ERR_INVALID_STATE if the device is mounted, read-only, or the
 *        lower device rejects writes
 *      - ESP_ERR_INVALID_SIZE for a geometry or block-size mismatch
 *      - ESP_ERR_NO_MEM if the temporary filesystem state cannot be allocated
 *      - ESP_FAIL if ext4_mkfs fails
 */
esp_err_t lwext4_port_bdl_format(lwext4_port_bdl_t *adapter, const lwext4_port_bdl_format_config_t *config);

/**
 * @brief Get the most recent result returned by a lower BDL operation.
 *
 * ESP_OK means either that no lower operation has failed or that the most
 * recent lower operation succeeded.
 */
esp_err_t lwext4_port_bdl_last_error(const lwext4_port_bdl_t *adapter);

/**
 * @brief Flush and destroy an adapter without releasing its lower BDL.
 *
 * Returns ESP_ERR_INVALID_STATE while lwext4 still references the block
 * device. If the final lower sync fails, the adapter remains allocated so the
 * caller may retry or inspect lwext4_port_bdl_last_error(). The caller must
 * unmount and unregister the lwext4 device before destroying the adapter.
 */
esp_err_t lwext4_port_bdl_destroy(lwext4_port_bdl_t *adapter);

#ifdef __cplusplus
}
#endif

#endif /* LWEXT4_PORT_BDL_H_ */
