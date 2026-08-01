/*
 * SPDX-FileCopyrightText: 2026 esp_lwext4 contributors
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "lwext4_port_format.h"

#include <errno.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "ext4_errno.h"
#include "ext4_mkfs.h"

#define LWEXT4_PORT_FORMAT_PROGRESS_INTERVAL_BLOCKS 4096U

static const char *TAG = "lwext4_format";

static bool is_power_of_two(uint32_t value)
{
    return value != 0 && (value & (value - 1)) == 0;
}

typedef struct lwext4_port_format_wrapper {
    struct ext4_blockdev device;
    struct ext4_blockdev_iface iface;
    struct ext4_blockdev *inner;
    uint32_t blocks_since_progress;
    uint64_t blocks_read;
    uint64_t blocks_written;
    void (*progress)(void *arg, uint64_t bytes_read, uint64_t bytes_written);
    void *progress_arg;
} lwext4_port_format_wrapper_t;

static lwext4_port_format_wrapper_t *format_wrapper_from_bdev(struct ext4_blockdev *bdev)
{
    if (bdev == NULL || bdev->bdif == NULL) {
        return NULL;
    }
    return bdev->bdif->p_user;
}

static int format_wrapper_open(struct ext4_blockdev *bdev)
{
    lwext4_port_format_wrapper_t *wrapper = format_wrapper_from_bdev(bdev);

    if (wrapper == NULL || wrapper->inner == NULL || wrapper->inner->bdif == NULL) {
        return EINVAL;
    }
    return wrapper->inner->bdif->open(wrapper->inner);
}

static int format_wrapper_close(struct ext4_blockdev *bdev)
{
    lwext4_port_format_wrapper_t *wrapper = format_wrapper_from_bdev(bdev);

    if (wrapper == NULL || wrapper->inner == NULL || wrapper->inner->bdif == NULL) {
        return EINVAL;
    }
    return wrapper->inner->bdif->close(wrapper->inner);
}

static void format_wrapper_report(lwext4_port_format_wrapper_t *wrapper)
{
    wrapper->blocks_since_progress = 0;
    wrapper->progress(wrapper->progress_arg, wrapper->blocks_read * wrapper->iface.ph_bsize,
                      wrapper->blocks_written * wrapper->iface.ph_bsize);
}

static void format_wrapper_account(lwext4_port_format_wrapper_t *wrapper, uint32_t block_count)
{
    if (wrapper->progress == NULL) {
        return;
    }
    if (UINT32_MAX - wrapper->blocks_since_progress < block_count) {
        wrapper->blocks_since_progress = LWEXT4_PORT_FORMAT_PROGRESS_INTERVAL_BLOCKS;
    } else {
        wrapper->blocks_since_progress += block_count;
    }
    if (wrapper->blocks_since_progress >= LWEXT4_PORT_FORMAT_PROGRESS_INTERVAL_BLOCKS) {
        format_wrapper_report(wrapper);
    }
}

static int format_wrapper_read(struct ext4_blockdev *bdev, void *buffer, uint64_t block_id, uint32_t block_count)
{
    lwext4_port_format_wrapper_t *wrapper = format_wrapper_from_bdev(bdev);
    int rc = EOK;

    if (wrapper == NULL || wrapper->inner == NULL || wrapper->inner->bdif == NULL) {
        return EINVAL;
    }
    if (wrapper->inner->bdif->lock != NULL) {
        rc = wrapper->inner->bdif->lock(wrapper->inner);
        if (rc != EOK) {
            return rc;
        }
    }
    rc = wrapper->inner->bdif->bread(wrapper->inner, buffer, block_id, block_count);
    if (wrapper->inner->bdif->unlock != NULL) {
        int unlock_rc = wrapper->inner->bdif->unlock(wrapper->inner);
        if (rc == EOK) {
            rc = unlock_rc;
        }
    }
    if (rc == EOK) {
        wrapper->blocks_read += block_count;
        format_wrapper_account(wrapper, block_count);
    }
    return rc;
}

static int format_wrapper_write(struct ext4_blockdev *bdev, const void *buffer, uint64_t block_id, uint32_t block_count)
{
    lwext4_port_format_wrapper_t *wrapper = format_wrapper_from_bdev(bdev);
    int rc = EOK;

    if (wrapper == NULL || wrapper->inner == NULL || wrapper->inner->bdif == NULL) {
        return EINVAL;
    }
    if (wrapper->inner->bdif->lock != NULL) {
        rc = wrapper->inner->bdif->lock(wrapper->inner);
        if (rc != EOK) {
            return rc;
        }
    }
    rc = wrapper->inner->bdif->bwrite(wrapper->inner, buffer, block_id, block_count);
    if (wrapper->inner->bdif->unlock != NULL) {
        int unlock_rc = wrapper->inner->bdif->unlock(wrapper->inner);
        if (rc == EOK) {
            rc = unlock_rc;
        }
    }
    if (rc == EOK) {
        wrapper->blocks_written += block_count;
        format_wrapper_account(wrapper, block_count);
    }
    return rc;
}

static void format_wrapper_init(lwext4_port_format_wrapper_t *wrapper, struct ext4_blockdev *inner,
                                void (*progress)(void *arg, uint64_t bytes_read, uint64_t bytes_written), void *progress_arg)
{
    memset(wrapper, 0, sizeof(*wrapper));
    wrapper->inner = inner;
    wrapper->progress = progress;
    wrapper->progress_arg = progress_arg;
    wrapper->iface.open = format_wrapper_open;
    wrapper->iface.bread = format_wrapper_read;
    wrapper->iface.bwrite = format_wrapper_write;
    wrapper->iface.close = format_wrapper_close;
    wrapper->iface.ph_bsize = inner->bdif->ph_bsize;
    wrapper->iface.ph_bcnt = inner->bdif->ph_bcnt;
    wrapper->iface.ph_bbuf = inner->bdif->ph_bbuf;
    wrapper->iface.p_user = wrapper;
    wrapper->device.bdif = &wrapper->iface;
    wrapper->device.part_offset = inner->part_offset;
    wrapper->device.part_size = inner->part_size;
}

esp_err_t lwext4_port_format_blockdev(struct ext4_blockdev *bdev, const lwext4_port_bdl_format_config_t *config)
{
    struct ext4_fs *fs;
    struct ext4_mkfs_info info;
    lwext4_port_format_wrapper_t wrapper;
    uint32_t feature_set;
    int rc;

    if (bdev == NULL || bdev->bdif == NULL || config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    fs = calloc(1, sizeof(*fs));
    if (fs == NULL) {
        return ESP_ERR_NO_MEM;
    }

    memset(&info, 0, sizeof(info));
    info.len = bdev->part_size;
    info.block_size = config->block_size != 0 ? config->block_size : 4096;
    info.inode_size = config->inode_size;
    info.inodes = config->inodes;
    info.journal_blocks = config->journal_blocks;
    info.journal = config->journal;
    info.label = config->label != NULL ? config->label : "";

    if (info.block_size < 1024 || info.block_size > EXT4_MAX_BLOCK_SIZE || info.block_size > UINT32_MAX / 8) {
        ESP_LOGE(TAG, "Invalid block size: %" PRIu32 " (supported range 1024..%u)", info.block_size, EXT4_MAX_BLOCK_SIZE);
        free(fs);
        return ESP_ERR_INVALID_SIZE;
    }
    if (info.inode_size != 0 &&
        (info.inode_size < 128 || info.inode_size > info.block_size || !is_power_of_two(info.inode_size))) {
        ESP_LOGE(TAG, "Invalid inode size: %" PRIu32 " (zero or a power of two between 128 and block_size)", info.inode_size);
        free(fs);
        return ESP_ERR_INVALID_ARG;
    }

#if CONFIG_LWEXT4_FORMAT_ROUND_TO_FULL_GROUPS
    /*
     * The pinned lwext4 formatter computes the final partial block group's
     * descriptor free-block count from a full group, while the bitmap marks
     * out-of-range blocks as used. e2fsck therefore reports a free-block
     * count mismatch on devices that do not end on a group boundary. Round
     * the length down to whole groups to keep the metadata consistent.
     */
    info.blocks_per_group = info.block_size * 8; /* matches lwext4 compute_blocks_per_group() */
    const uint64_t group_bytes = (uint64_t)info.blocks_per_group * info.block_size;
    const uint64_t rounded_len = (info.len / group_bytes) * group_bytes;
    if (rounded_len == 0) {
        ESP_LOGE(TAG, "Device is smaller than one block group (%" PRIu64 " MiB) and cannot be rounded to a full group",
                 group_bytes / (1024U * 1024U));
        free(fs);
        return ESP_ERR_INVALID_SIZE;
    }
    if (rounded_len != info.len) {
        ESP_LOGW(TAG, "Rounding filesystem length from %" PRIu64 " MiB to %" PRIu64 " MiB to avoid a partial final block group",
                 info.len / (1024U * 1024U), rounded_len / (1024U * 1024U));
        info.len = rounded_len;
    }
#endif

#if CONFIG_LWEXT4_FEATURE_SET_EXT4
    feature_set = F_SET_EXT4;
#elif CONFIG_LWEXT4_FEATURE_SET_EXT3
    feature_set = F_SET_EXT3;
#else
    feature_set = F_SET_EXT2;
#endif
    if (config->feature_set != 0) {
        feature_set = config->feature_set;
    }

    ESP_LOGW(TAG, "Formatting %" PRIu64 " MiB as %s: block_size=%" PRIu32 ", inodes=%" PRIu32 ", journal_blocks=%" PRIu32,
             info.len / (1024U * 1024U),
             feature_set == F_SET_EXT4   ? "ext4"
             : feature_set == F_SET_EXT3 ? "ext3"
                                         : "ext2",
             info.block_size, info.inodes, info.journal_blocks);

    format_wrapper_init(&wrapper, bdev, config->progress, config->progress_arg);

    if (bdev->bdif->lock != NULL) {
        rc = bdev->bdif->lock(bdev);
        if (rc != EOK) {
            free(fs);
            return rc == ENOMEM ? ESP_ERR_NO_MEM : ESP_FAIL;
        }
    }
    rc = ext4_mkfs(fs, &wrapper.device, &info, (int)feature_set);
    if (bdev->bdif->unlock != NULL) {
        int unlock_rc = bdev->bdif->unlock(bdev);
        if (rc == EOK) {
            rc = unlock_rc;
        }
    }
    free(fs);

    if (rc != EOK) {
        ESP_LOGE(TAG, "ext4_mkfs failed: lwext4 rc=%d", rc);
        return ESP_FAIL;
    }
    return ESP_OK;
}
