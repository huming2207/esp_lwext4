/*
 * SPDX-FileCopyrightText: 2026 esp_lwext4 contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Extent-tree implementation ported from ext4-rs
 * (https://github.com/yuoo655/ext4_rs), MIT License,
 * Copyright (c) 2024 yuoo655 (lenuulan@gmail.com).
 *
 * The initial implementation used ext4-rs/src/ext4_impls/extents.rs and
 * ext4-rs/src/ext4_defs/extents.rs as its permissively licensed reference.
 * Subsequent tree-splitting, validation, and removal work was reworked for
 * the ext4 on-disk tree invariants and the lwext4 internal interfaces.
 */

#include <ext4_config.h>
#include <ext4_types.h>
#include <ext4_misc.h>
#include <ext4_errno.h>
#include <ext4_debug.h>

#include <ext4_blockdev.h>
#include <ext4_trans.h>
#include <ext4_fs.h>
#include <ext4_super.h>
#include <ext4_crc32.h>
#include <ext4_balloc.h>
#include <ext4_extent.h>
#include <ext4_inode.h>

#include <string.h>

#if CONFIG_EXTENT_ENABLE && CONFIG_EXTENTS_ENABLE

#define EXT4_EXTENT_MAGIC 0xF30A
#define EXT4_EXTENT_HEADER_SIZE 12
#define EXT4_EXTENT_SIZE 12
#define EXT4_EXTENT_INDEX_SIZE 12
#define EXT_INIT_MAX_LEN 32768
#define EXT4_EXTENT_MAX_DEPTH 5
#define EXT4_EXTENT_ROOT_MAX_ENTRIES 4

struct ext4_extent_header {
    uint16_t magic;
    uint16_t entries_count;
    uint16_t max_entries_count;
    uint16_t depth;
    uint32_t generation;
};

struct ext4_extent {
    uint32_t first_block;
    uint16_t block_count;
    uint16_t start_hi;
    uint32_t start_lo;
};

struct ext4_extent_index {
    uint32_t first_block;
    uint32_t leaf_lo;
    uint16_t leaf_hi;
    uint16_t padding;
};

struct extent_path_node {
    struct ext4_extent_header header;
    struct ext4_extent_index index;
    struct ext4_extent extent;
    bool have_index;
    bool have_extent;
    uint32_t position;
    uint64_t pblock;         /* leaf: resolved data block; index: child block */
    uint64_t pblock_of_node; /* 0 means the inode root node */
};

struct extent_search_path {
    uint32_t depth;
    uint32_t maxdepth;
    struct extent_path_node path[EXT4_EXTENT_MAX_DEPTH + 1];
};

static int insert_extent(struct ext4_inode_ref *inode_ref, uint32_t first_block, const struct ext4_extent *new_extent);
static int grow_indepth(struct ext4_inode_ref *inode_ref);

static uint16_t read_le16(const uint8_t *data)
{
    uint16_t value;
    memcpy(&value, data, sizeof(value));
    return to_le16(value);
}

static uint32_t read_le32(const uint8_t *data)
{
    uint32_t value;
    memcpy(&value, data, sizeof(value));
    return to_le32(value);
}

static void write_le16(uint8_t *data, uint16_t value)
{
    value = to_le16(value);
    memcpy(data, &value, sizeof(value));
}

static void write_le32(uint8_t *data, uint32_t value)
{
    value = to_le32(value);
    memcpy(data, &value, sizeof(value));
}

static void header_load(const uint8_t *data, struct ext4_extent_header *header)
{
    header->magic = read_le16(data + 0);
    header->entries_count = read_le16(data + 2);
    header->max_entries_count = read_le16(data + 4);
    header->depth = read_le16(data + 6);
    header->generation = read_le32(data + 8);
}

static void header_store(uint8_t *data, const struct ext4_extent_header *header)
{
    write_le16(data + 0, header->magic);
    write_le16(data + 2, header->entries_count);
    write_le16(data + 4, header->max_entries_count);
    write_le16(data + 6, header->depth);
    write_le32(data + 8, header->generation);
}

static void extent_load(const uint8_t *data, struct ext4_extent *extent)
{
    extent->first_block = read_le32(data + 0);
    extent->block_count = read_le16(data + 4);
    extent->start_hi = read_le16(data + 6);
    extent->start_lo = read_le32(data + 8);
}

static void extent_store(uint8_t *data, const struct ext4_extent *extent)
{
    write_le32(data + 0, extent->first_block);
    write_le16(data + 4, extent->block_count);
    write_le16(data + 6, extent->start_hi);
    write_le32(data + 8, extent->start_lo);
}

static void index_load(const uint8_t *data, struct ext4_extent_index *index)
{
    index->first_block = read_le32(data + 0);
    index->leaf_lo = read_le32(data + 4);
    index->leaf_hi = read_le16(data + 8);
    index->padding = read_le16(data + 10);
}

static void index_store(uint8_t *data, const struct ext4_extent_index *index)
{
    write_le32(data + 0, index->first_block);
    write_le32(data + 4, index->leaf_lo);
    write_le16(data + 8, index->leaf_hi);
    write_le16(data + 10, index->padding);
}

static uint64_t extent_get_pblock(const struct ext4_extent *extent)
{
    return ((uint64_t)extent->start_hi << 32) | extent->start_lo;
}

static void extent_store_pblock(struct ext4_extent *extent, uint64_t pblock)
{
    extent->start_lo = (uint32_t)(pblock & 0xFFFFFFFFU);
    extent->start_hi = (uint16_t)(pblock >> 32);
}

static uint64_t index_get_pblock(const struct ext4_extent_index *index)
{
    return ((uint64_t)index->leaf_hi << 32) | index->leaf_lo;
}

static void index_store_pblock(struct ext4_extent_index *index, uint64_t pblock)
{
    index->leaf_lo = (uint32_t)(pblock & 0xFFFFFFFFU);
    index->leaf_hi = (uint16_t)(pblock >> 32);
}

static bool extent_is_unwritten(const struct ext4_extent *extent)
{
    return extent->block_count > EXT_INIT_MAX_LEN;
}

static uint16_t extent_get_actual_len(const struct ext4_extent *extent)
{
    return extent_is_unwritten(extent) ? (uint16_t)(extent->block_count - EXT_INIT_MAX_LEN) : extent->block_count;
}

static void extent_set_actual_len(struct ext4_extent *extent, uint16_t len)
{
    extent->block_count = len;
}

static void extent_mark_unwritten(struct ext4_extent *extent)
{
    extent->block_count |= EXT_INIT_MAX_LEN;
}

/* Root node data lives in the first 60 bytes of inode->blocks. */
static uint8_t *root_data(struct ext4_inode_ref *inode_ref)
{
    return (uint8_t *)inode_ref->inode->blocks;
}

static int require_journaled_mutation(struct ext4_inode_ref *inode_ref)
{
#if CONFIG_JOURNALING_ENABLE
    if (inode_ref->fs->jbd_journal != NULL && inode_ref->fs->curr_trans != NULL) {
        return EOK;
    }
#endif
    return ENOTSUP;
}

/* Binary search for the extent containing (or preceding) lblock. */
static void binsearch_extent(const uint8_t *data, const struct ext4_extent_header *header, uint32_t lblock,
                             struct ext4_extent *out_extent, uint32_t *out_pos)
{
    if (header->entries_count == 0) {
        extent_load(data + EXT4_EXTENT_HEADER_SIZE, out_extent);
        *out_pos = 0;
        return;
    }

    uint32_t l = 1;
    uint32_t r = header->entries_count - 1;
    while (l <= r) {
        uint32_t m = l + (r - l) / 2;
        struct ext4_extent ext;
        extent_load(data + EXT4_EXTENT_HEADER_SIZE + m * EXT4_EXTENT_SIZE, &ext);
        if (lblock < ext.first_block) {
            r = m - 1;
        } else {
            l = m + 1;
        }
    }

    uint32_t pos = l - 1;
    extent_load(data + EXT4_EXTENT_HEADER_SIZE + pos * EXT4_EXTENT_SIZE, out_extent);
    *out_pos = pos;
}

/* Binary search for the index governing lblock. Returns false when none. */
static bool binsearch_idx(const uint8_t *data, bool is_root, const struct ext4_extent_header *header, uint32_t lblock,
                          uint32_t *out_pos)
{
    (void)is_root;
    if (header->entries_count == 0) {
        return false;
    }

    /* Entry zero is the leftmost child even when its key is greater than the
     * requested block. Search entries [1, n) for a better separator. */
    uint32_t l = 1;
    uint32_t r = header->entries_count - 1;
    while (l <= r) {
        uint32_t m = l + (r - l) / 2;
        struct ext4_extent_index index;
        index_load(data + EXT4_EXTENT_HEADER_SIZE + m * EXT4_EXTENT_INDEX_SIZE, &index);
        if (lblock < index.first_block) {
            if (m == 0) {
                break;
            }
            r = m - 1;
        } else {
            l = m + 1;
        }
    }

    if (l == 0) {
        return false;
    }
    *out_pos = l - 1;
    return true;
}

static int node_block_get(struct ext4_inode_ref *inode_ref, uint64_t pblock, struct ext4_block *block)
{
    return ext4_trans_block_get(inode_ref->fs->bdev, block, pblock);
}

static int node_block_put(struct ext4_inode_ref *inode_ref, struct ext4_block *block)
{
    int r = ext4_trans_set_block_dirty(block->buf);
    int put_r = ext4_block_set(inode_ref->fs->bdev, block);
    return r != EOK ? r : put_r;
}

static bool metadata_csum_enabled(struct ext4_inode_ref *inode_ref)
{
    return (ext4_get32(&inode_ref->fs->sb, features_read_only) & EXT4_FRO_COM_METADATA_CSUM) != 0;
}

static int validate_header(const uint8_t *data, bool is_root, uint32_t block_size, const struct ext4_extent_header *header)
{
    (void)data;
    uint32_t capacity = is_root ? EXT4_EXTENT_ROOT_MAX_ENTRIES : (block_size - EXT4_EXTENT_HEADER_SIZE) / EXT4_EXTENT_SIZE;

    if (header->magic != EXT4_EXTENT_MAGIC || header->depth > EXT4_EXTENT_MAX_DEPTH) {
        return EIO;
    }
    if (header->max_entries_count == 0 || header->entries_count > header->max_entries_count ||
        header->max_entries_count > capacity) {
        return EIO;
    }
    return EOK;
}

static int validate_entries(struct ext4_inode_ref *inode_ref, const uint8_t *data, const struct ext4_extent_header *header)
{
    uint64_t fs_blocks = ext4_sb_get_blocks_cnt(&inode_ref->fs->sb);
    uint64_t previous_end = 0;
    uint32_t previous_key = 0;

    for (uint32_t i = 0; i < header->entries_count; i++) {
        const uint8_t *entry = data + EXT4_EXTENT_HEADER_SIZE + i * EXT4_EXTENT_SIZE;
        if (header->depth == 0) {
            struct ext4_extent extent;
            extent_load(entry, &extent);
            uint32_t len = extent_get_actual_len(&extent);
            uint64_t logical_end = (uint64_t)extent.first_block + len;
            uint64_t physical = extent_get_pblock(&extent);

            if (len == 0 || logical_end > (uint64_t)UINT32_MAX + 1 || physical == 0 || physical >= fs_blocks ||
                len > fs_blocks - physical) {
                return EIO;
            }
            if (i > 0 && extent.first_block < previous_end) {
                return EIO;
            }
            previous_end = logical_end;
        } else {
            struct ext4_extent_index index;
            index_load(entry, &index);
            uint64_t child = index_get_pblock(&index);

            if (child == 0 || child >= fs_blocks || (i > 0 && index.first_block <= previous_key)) {
                return EIO;
            }
            previous_key = index.first_block;
        }
    }
    return EOK;
}

/* Validate the entry selected by a lookup and the ordering boundaries on
 * either side of it.  A full external node can contain hundreds of entries;
 * rescanning all of them on every data-block lookup would turn the extent
 * tree's binary search into a linear search.  Nodes are scanned fully before
 * a split copies and redistributes all of their entries. */
static int validate_selected_entry(struct ext4_inode_ref *inode_ref, const uint8_t *data, const struct ext4_extent_header *header,
                                   uint32_t pos)
{
    uint64_t fs_blocks = ext4_sb_get_blocks_cnt(&inode_ref->fs->sb);

    if (pos >= header->entries_count) {
        return EIO;
    }

    if (header->depth == 0) {
        struct ext4_extent current;
        extent_load(data + EXT4_EXTENT_HEADER_SIZE + pos * EXT4_EXTENT_SIZE, &current);
        uint32_t len = extent_get_actual_len(&current);
        uint64_t logical_end = (uint64_t)current.first_block + len;
        uint64_t physical = extent_get_pblock(&current);

        if (len == 0 || logical_end > (uint64_t)UINT32_MAX + 1 || physical == 0 || physical >= fs_blocks ||
            len > fs_blocks - physical) {
            return EIO;
        }
        if (pos > 0) {
            struct ext4_extent previous;
            extent_load(data + EXT4_EXTENT_HEADER_SIZE + (pos - 1) * EXT4_EXTENT_SIZE, &previous);
            if ((uint64_t)previous.first_block + extent_get_actual_len(&previous) > current.first_block) {
                return EIO;
            }
        }
        if (pos + 1 < header->entries_count) {
            struct ext4_extent next;
            extent_load(data + EXT4_EXTENT_HEADER_SIZE + (pos + 1) * EXT4_EXTENT_SIZE, &next);
            if (logical_end > next.first_block) {
                return EIO;
            }
        }
        return EOK;
    }

    struct ext4_extent_index current;
    index_load(data + EXT4_EXTENT_HEADER_SIZE + pos * EXT4_EXTENT_INDEX_SIZE, &current);
    uint64_t child = index_get_pblock(&current);
    if (child == 0 || child >= fs_blocks) {
        return EIO;
    }
    if (pos > 0) {
        struct ext4_extent_index previous;
        index_load(data + EXT4_EXTENT_HEADER_SIZE + (pos - 1) * EXT4_EXTENT_INDEX_SIZE, &previous);
        if (previous.first_block >= current.first_block) {
            return EIO;
        }
    }
    if (pos + 1 < header->entries_count) {
        struct ext4_extent_index next;
        index_load(data + EXT4_EXTENT_HEADER_SIZE + (pos + 1) * EXT4_EXTENT_INDEX_SIZE, &next);
        if (current.first_block >= next.first_block) {
            return EIO;
        }
    }
    return EOK;
}

static uint32_t extent_block_checksum(struct ext4_inode_ref *inode_ref, const uint8_t *data, uint32_t tail_offset);

static int verify_extent_block_checksum(struct ext4_inode_ref *inode_ref, const uint8_t *data)
{
    struct ext4_extent_header header;
    uint32_t tail_offset;
    uint32_t expected;
    uint32_t block_size = ext4_sb_get_block_size(&inode_ref->fs->sb);

    if (!metadata_csum_enabled(inode_ref)) {
        return EOK;
    }
    header_load(data, &header);
    if (header.magic != EXT4_EXTENT_MAGIC) {
        return EIO;
    }
    tail_offset = EXT4_EXTENT_HEADER_SIZE + header.max_entries_count * EXT4_EXTENT_SIZE;
    if (tail_offset > block_size || tail_offset + sizeof(uint32_t) > block_size) {
        return EIO;
    }
    expected = extent_block_checksum(inode_ref, data, tail_offset);
    return read_le32(data + tail_offset) == expected ? EOK : EIO;
}

static int find_extent(struct ext4_inode_ref *inode_ref, uint32_t lblock, struct extent_search_path *path)
{
    uint8_t *root = root_data(inode_ref);
    struct ext4_extent_header header;
    uint8_t *node_data = root;
    bool is_root = true;
    uint64_t pblock_of_node = 0;
    uint32_t depth;
    struct ext4_block held;
    bool has_held = false;
    int r = EOK;

    memset(path, 0, sizeof(*path));
    header_load(node_data, &header);
    r = validate_header(node_data, true, ext4_sb_get_block_size(&inode_ref->fs->sb), &header);
    if (r != EOK) {
        return r;
    }
    r = validate_entries(inode_ref, node_data, &header);
    if (r != EOK) {
        return r;
    }
    path->maxdepth = header.depth;
    depth = header.depth;

    while (depth > 0) {
        uint32_t pos;
        struct ext4_extent_index index;
        struct ext4_block block;

        /* node_data is valid: the root, or the block held from the previous
         * iteration. */
        if (!binsearch_idx(node_data, is_root, &header, lblock, &pos)) {
            r = ENOENT;
            goto fail;
        }
        r = validate_selected_entry(inode_ref, node_data, &header, pos);
        if (r != EOK) {
            goto fail;
        }
        index_load(node_data + EXT4_EXTENT_HEADER_SIZE + pos * EXT4_EXTENT_INDEX_SIZE, &index);

        struct extent_path_node *node = &path->path[path->depth];
        node->header = header;
        node->index = index;
        node->have_index = true;
        node->position = pos;
        node->pblock = index_get_pblock(&index);
        node->pblock_of_node = pblock_of_node;

        uint64_t next_block = index_get_pblock(&index);
        r = node_block_get(inode_ref, next_block, &block);
        if (r != EOK) {
            goto fail;
        }
        r = verify_extent_block_checksum(inode_ref, block.data);
        if (r != EOK) {
            ext4_block_set(inode_ref->fs->bdev, &block);
            goto fail;
        }
        if (has_held) {
            ext4_block_set(inode_ref->fs->bdev, &held);
        }
        held = block;
        has_held = true;
        node_data = block.data;
        is_root = false;
        header_load(node_data, &header);
        r = validate_header(node_data, false, ext4_sb_get_block_size(&inode_ref->fs->sb), &header);
        if (r != EOK) {
            ext4_block_set(inode_ref->fs->bdev, &held);
            has_held = false;
            goto fail;
        }
        if (header.depth != depth - 1) {
            ext4_block_set(inode_ref->fs->bdev, &held);
            has_held = false;
            r = EIO;
            goto fail;
        }
        depth--;
        path->depth++;
        pblock_of_node = next_block;
    }

    struct extent_path_node *leaf = &path->path[path->depth];
    leaf->header = header;
    leaf->have_extent = true;
    leaf->pblock_of_node = pblock_of_node;
    binsearch_extent(node_data, &header, lblock, &leaf->extent, &leaf->position);
    if (header.entries_count > 0) {
        r = validate_selected_entry(inode_ref, node_data, &header, leaf->position);
    } else {
        leaf->have_extent = false;
    }
    if (has_held) {
        ext4_block_set(inode_ref->fs->bdev, &held);
    }
    if (r != EOK) {
        return r;
    }
    if (!leaf->have_extent) {
        leaf->pblock = 0;
        return EOK;
    }
    if (lblock >= leaf->extent.first_block && lblock < leaf->extent.first_block + extent_get_actual_len(&leaf->extent)) {
        leaf->pblock = extent_get_pblock(&leaf->extent) + (lblock - leaf->extent.first_block);
    } else {
        leaf->pblock = 0;
    }
    return EOK;

fail:
    if (has_held) {
        ext4_block_set(inode_ref->fs->bdev, &held);
    }
    return r;
}

static bool can_merge(const struct ext4_extent *ex1, const struct ext4_extent *ex2)
{
    uint16_t len1;
    uint16_t len2;
    uint32_t max_len;

    if (extent_is_unwritten(ex1) != extent_is_unwritten(ex2)) {
        return false;
    }
    len1 = extent_get_actual_len(ex1);
    len2 = extent_get_actual_len(ex2);
    if (ex1->first_block + len1 != ex2->first_block) {
        return false;
    }
    /* An unwritten length of 32768 cannot be encoded: the high flag would be
     * indistinguishable from an initialized length. */
    max_len = extent_is_unwritten(ex1) ? EXT_INIT_MAX_LEN - 1 : EXT_INIT_MAX_LEN;
    if ((uint32_t)len1 + len2 > max_len) {
        return false;
    }
    return extent_get_pblock(ex1) + len1 == extent_get_pblock(ex2);
}

static int set_extent_block_checksum(struct ext4_inode_ref *inode_ref, uint8_t *data, bool is_root);

/* Load one non-root node entry from disk. */
static int node_get_extent(struct ext4_inode_ref *inode_ref, const struct extent_path_node *node, uint32_t pos,
                           struct ext4_extent *out)
{
    if (node->pblock_of_node == 0) {
        extent_load(root_data(inode_ref) + EXT4_EXTENT_HEADER_SIZE + pos * EXT4_EXTENT_SIZE, out);
        return EOK;
    }
    struct ext4_block block;
    int r = node_block_get(inode_ref, node->pblock_of_node, &block);
    if (r != EOK) {
        return r;
    }
    extent_load(block.data + EXT4_EXTENT_HEADER_SIZE + pos * EXT4_EXTENT_SIZE, out);
    return ext4_block_set(inode_ref->fs->bdev, &block);
}

static int node_get_index(struct ext4_inode_ref *inode_ref, const struct extent_path_node *node, uint32_t pos,
                          struct ext4_extent_index *out)
{
    if (node->pblock_of_node == 0) {
        index_load(root_data(inode_ref) + EXT4_EXTENT_HEADER_SIZE + pos * EXT4_EXTENT_INDEX_SIZE, out);
        return EOK;
    }
    struct ext4_block block;
    int r = node_block_get(inode_ref, node->pblock_of_node, &block);
    if (r != EOK) {
        return r;
    }
    index_load(block.data + EXT4_EXTENT_HEADER_SIZE + pos * EXT4_EXTENT_INDEX_SIZE, out);
    return ext4_block_set(inode_ref->fs->bdev, &block);
}

static int leftmost_extent_below(struct ext4_inode_ref *inode_ref, const struct ext4_extent_index *start, uint16_t expected_depth,
                                 struct ext4_extent *out)
{
    uint64_t pblock = index_get_pblock(start);
    uint32_t block_size = ext4_sb_get_block_size(&inode_ref->fs->sb);

    for (;;) {
        struct ext4_block block;
        struct ext4_extent_header header;
        int r = node_block_get(inode_ref, pblock, &block);
        if (r != EOK) {
            return r;
        }
        r = verify_extent_block_checksum(inode_ref, block.data);
        if (r == EOK) {
            header_load(block.data, &header);
            r = validate_header(block.data, false, block_size, &header);
        }
        if (r == EOK && header.depth != expected_depth) {
            r = EIO;
        }
        if (r == EOK) {
            r = validate_entries(inode_ref, block.data, &header);
        }
        if (r != EOK || header.entries_count == 0) {
            int put_r = ext4_block_set(inode_ref->fs->bdev, &block);
            return r != EOK ? r : (put_r != EOK ? put_r : EIO);
        }

        if (header.depth == 0) {
            extent_load(block.data + EXT4_EXTENT_HEADER_SIZE, out);
            return ext4_block_set(inode_ref->fs->bdev, &block);
        }

        struct ext4_extent_index next;
        index_load(block.data + EXT4_EXTENT_HEADER_SIZE, &next);
        pblock = index_get_pblock(&next);
        expected_depth--;
        r = ext4_block_set(inode_ref->fs->bdev, &block);
        if (r != EOK) {
            return r;
        }
    }
}

static int find_next_extent(struct ext4_inode_ref *inode_ref, const struct extent_search_path *path, struct ext4_extent *out)
{
    const struct extent_path_node *leaf = &path->path[path->depth];
    if (leaf->position + 1 < leaf->header.entries_count) {
        return node_get_extent(inode_ref, leaf, leaf->position + 1, out);
    }

    for (uint32_t level = path->depth; level > 0; level--) {
        const struct extent_path_node *parent = &path->path[level - 1];
        if (parent->position + 1 < parent->header.entries_count) {
            struct ext4_extent_index sibling;
            int r = node_get_index(inode_ref, parent, parent->position + 1, &sibling);
            if (r != EOK) {
                return r;
            }
            if (parent->header.depth == 0) {
                return EIO;
            }
            return leftmost_extent_below(inode_ref, &sibling, (uint16_t)(parent->header.depth - 1), out);
        }
    }
    return ENOENT;
}

/*
 * Merge right_ext into left_ext. When the leaf is a non-root block, the
 * merged extent is written back to the node at node->position.
 */
static int merge_extent(struct ext4_inode_ref *inode_ref, struct extent_search_path *path, struct ext4_extent *left_ext,
                        const struct ext4_extent *right_ext, uint32_t target_pos)
{
    struct extent_path_node *leaf = &path->path[path->depth];
    uint16_t len = (uint16_t)(extent_get_actual_len(left_ext) + extent_get_actual_len(right_ext));
    bool unwritten = extent_is_unwritten(left_ext);

    extent_set_actual_len(left_ext, len);
    if (unwritten) {
        extent_mark_unwritten(left_ext);
    }

    if (leaf->header.max_entries_count > EXT4_EXTENT_ROOT_MAX_ENTRIES) {
        struct ext4_block block;
        int r = node_block_get(inode_ref, leaf->pblock_of_node, &block);
        if (r != EOK) {
            return r;
        }
        uint8_t *data = block.data + EXT4_EXTENT_HEADER_SIZE + target_pos * EXT4_EXTENT_SIZE;
        extent_store(data, left_ext);
        r = set_extent_block_checksum(inode_ref, block.data, false);
        if (r != EOK) {
            ext4_block_set(inode_ref->fs->bdev, &block);
            return r;
        }
        return node_block_put(inode_ref, &block);
    }
    return EOK;
}

static int insert_new_extent(struct ext4_inode_ref *inode_ref, struct extent_search_path *path,
                             const struct ext4_extent *new_extent);
static int correct_indexes(struct ext4_inode_ref *inode_ref, struct extent_search_path *path, uint32_t child_level,
                           uint32_t new_first_block);

/* First position in an entry array whose first_block exceeds first_block. */
static uint32_t entry_insert_pos(const uint8_t *data, uint32_t count, uint32_t first_block)
{
    uint32_t pos = 0;
    while (pos < count) {
        if (read_le32(data + pos * EXT4_EXTENT_SIZE) > first_block) {
            break;
        }
        pos++;
    }
    return pos;
}

static int free_new_metadata_block(struct ext4_inode_ref *inode_ref, ext4_fsblk_t block, int primary_error)
{
    int free_error = ext4_balloc_free_block(inode_ref, block);
    return primary_error != EOK ? primary_error : free_error;
}

/* Insert one child separator into a node that is known to have space. */
static int insert_index_entry(struct ext4_inode_ref *inode_ref, struct extent_search_path *path, uint32_t level,
                              uint32_t first_block, uint64_t pblock)
{
    struct extent_path_node *node = &path->path[level];
    struct ext4_block block;
    uint8_t *data;
    bool at_root = node->pblock_of_node == 0;
    int r;

    if (node->header.depth == 0 || node->header.entries_count >= node->header.max_entries_count) {
        return ENOSPC;
    }
    if (at_root) {
        data = root_data(inode_ref);
    } else {
        r = node_block_get(inode_ref, node->pblock_of_node, &block);
        if (r != EOK) {
            return r;
        }
        data = block.data;
        struct ext4_extent_header disk_header;
        header_load(data, &disk_header);
        r = validate_header(data, false, ext4_sb_get_block_size(&inode_ref->fs->sb), &disk_header);
        if (r == EOK) {
            r = validate_entries(inode_ref, data, &disk_header);
        }
        if (r != EOK || disk_header.depth != node->header.depth || disk_header.entries_count != node->header.entries_count ||
            disk_header.max_entries_count != node->header.max_entries_count) {
            ext4_block_set(inode_ref->fs->bdev, &block);
            return r != EOK ? r : EIO;
        }
    }

    uint32_t pos = entry_insert_pos(data + EXT4_EXTENT_HEADER_SIZE, node->header.entries_count, first_block);
    if (pos > 0 && read_le32(data + EXT4_EXTENT_HEADER_SIZE + (pos - 1) * EXT4_EXTENT_INDEX_SIZE) == first_block) {
        if (!at_root) {
            ext4_block_set(inode_ref->fs->bdev, &block);
        }
        return EEXIST;
    }
    if (pos < node->header.entries_count) {
        memmove(data + EXT4_EXTENT_HEADER_SIZE + (pos + 1) * EXT4_EXTENT_INDEX_SIZE,
                data + EXT4_EXTENT_HEADER_SIZE + pos * EXT4_EXTENT_INDEX_SIZE,
                (node->header.entries_count - pos) * EXT4_EXTENT_INDEX_SIZE);
    }
    struct ext4_extent_index new_index = {.first_block = first_block};
    index_store_pblock(&new_index, pblock);
    index_store(data + EXT4_EXTENT_HEADER_SIZE + pos * EXT4_EXTENT_INDEX_SIZE, &new_index);
    struct ext4_extent_header header = node->header;
    header.entries_count = (uint16_t)(header.entries_count + 1);
    header_store(data, &header);

    if (at_root) {
        inode_ref->dirty = true;
        return EOK;
    }
    r = set_extent_block_checksum(inode_ref, data, false);
    if (r != EOK) {
        ext4_block_set(inode_ref->fs->bdev, &block);
        return r;
    }
    return node_block_put(inode_ref, &block);
}

/* Split a full non-root node without inserting a data extent. The parent is
 * guaranteed to have space by ensure_insert_room(). The right node is made
 * reachable before the left node is shortened, so a failed final write does
 * not orphan the moved mappings. */
static int split_full_node(struct ext4_inode_ref *inode_ref, struct extent_search_path *path, uint32_t level)
{
    struct extent_path_node *node = &path->path[level];
    struct extent_path_node *parent;
    uint32_t block_size = ext4_sb_get_block_size(&inode_ref->fs->sb);
    uint32_t capacity = (block_size - EXT4_EXTENT_HEADER_SIZE) / EXT4_EXTENT_SIZE;
    uint32_t entries = node->header.entries_count;
    uint32_t mid = entries / 2;
    ext4_fsblk_t new_block_addr;
    struct ext4_block source_block;
    struct ext4_block right_block;
    uint32_t first_right;
    int r;

    if (level == 0 || node->pblock_of_node == 0 || entries < 2 || entries != node->header.max_entries_count) {
        return EINVAL;
    }
    parent = &path->path[level - 1];
    if (parent->header.entries_count >= parent->header.max_entries_count) {
        return ENOSPC;
    }

    r = node_block_get(inode_ref, node->pblock_of_node, &source_block);
    if (r != EOK) {
        return r;
    }
    struct ext4_extent_header source_header;
    header_load(source_block.data, &source_header);
    r = validate_header(source_block.data, false, block_size, &source_header);
    if (r == EOK) {
        r = validate_entries(inode_ref, source_block.data, &source_header);
    }
    if (r != EOK || source_header.depth != node->header.depth || source_header.entries_count != entries ||
        source_header.max_entries_count != node->header.max_entries_count) {
        ext4_block_set(inode_ref->fs->bdev, &source_block);
        return r != EOK ? r : EIO;
    }
    r = ext4_balloc_alloc_block(inode_ref, ext4_fs_inode_to_goal_block(inode_ref), &new_block_addr);
    if (r != EOK) {
        ext4_block_set(inode_ref->fs->bdev, &source_block);
        return r;
    }
    r = ext4_trans_block_get_noread(inode_ref->fs->bdev, &right_block, new_block_addr);
    if (r != EOK) {
        ext4_block_set(inode_ref->fs->bdev, &source_block);
        return free_new_metadata_block(inode_ref, new_block_addr, r);
    }
    memset(right_block.data, 0, block_size);

    uint32_t right_entries = entries - mid;
    struct ext4_extent_header right_header = {
        .magic = EXT4_EXTENT_MAGIC,
        .entries_count = (uint16_t)right_entries,
        .max_entries_count = (uint16_t)capacity,
        .depth = node->header.depth,
        .generation = 0,
    };
    header_store(right_block.data, &right_header);
    memcpy(right_block.data + EXT4_EXTENT_HEADER_SIZE, source_block.data + EXT4_EXTENT_HEADER_SIZE + mid * EXT4_EXTENT_SIZE,
           right_entries * EXT4_EXTENT_SIZE);
    first_right = read_le32(right_block.data + EXT4_EXTENT_HEADER_SIZE);

    r = ext4_block_set(inode_ref->fs->bdev, &source_block);
    if (r != EOK) {
        ext4_block_set(inode_ref->fs->bdev, &right_block);
        return free_new_metadata_block(inode_ref, new_block_addr, r);
    }
    r = set_extent_block_checksum(inode_ref, right_block.data, false);
    if (r != EOK) {
        ext4_block_set(inode_ref->fs->bdev, &right_block);
        return free_new_metadata_block(inode_ref, new_block_addr, r);
    }
    r = node_block_put(inode_ref, &right_block);
    if (r != EOK) {
        return free_new_metadata_block(inode_ref, new_block_addr, r);
    }

    r = insert_index_entry(inode_ref, path, level - 1, first_right, new_block_addr);
    if (r != EOK) {
        return free_new_metadata_block(inode_ref, new_block_addr, r);
    }

    r = node_block_get(inode_ref, node->pblock_of_node, &source_block);
    if (r != EOK) {
        return r;
    }
    struct ext4_extent_header left_header = node->header;
    left_header.entries_count = (uint16_t)mid;
    header_store(source_block.data, &left_header);
    memset(source_block.data + EXT4_EXTENT_HEADER_SIZE + mid * EXT4_EXTENT_SIZE, 0, (entries - mid) * EXT4_EXTENT_SIZE);
    r = set_extent_block_checksum(inode_ref, source_block.data, false);
    if (r != EOK) {
        ext4_block_set(inode_ref->fs->bdev, &source_block);
        return r;
    }
    return node_block_put(inode_ref, &source_block);
}

/* Make every node on the insertion path non-full before adding the data
 * extent. Structural changes are followed by a fresh lookup. */
static int ensure_insert_room(struct ext4_inode_ref *inode_ref, uint32_t first_block, struct extent_search_path *result)
{
    for (uint32_t attempt = 0; attempt < EXT4_EXTENT_MAX_DEPTH * 2 + 4; attempt++) {
        struct extent_search_path path;
        int r = find_extent(inode_ref, first_block, &path);
        if (r != EOK) {
            return r;
        }

        struct extent_path_node *root = &path.path[0];
        if (root->header.entries_count >= root->header.max_entries_count) {
            r = grow_indepth(inode_ref);
            if (r != EOK) {
                return r;
            }
            continue;
        }

        bool split = false;
        for (uint32_t level = 1; level <= path.depth; level++) {
            struct extent_path_node *node = &path.path[level];
            if (node->header.entries_count >= node->header.max_entries_count) {
                r = split_full_node(inode_ref, &path, level);
                if (r != EOK) {
                    return r;
                }
                split = true;
                break;
            }
        }
        if (split) {
            continue;
        }

        *result = path;
        return EOK;
    }
    return EIO;
}

static int grow_indepth(struct ext4_inode_ref *inode_ref)
{
    struct ext4_block new_block;
    ext4_fsblk_t new_block_addr;
    uint8_t *root = root_data(inode_ref);
    struct ext4_extent_header old_header;
    uint32_t block_size = ext4_sb_get_block_size(&inode_ref->fs->sb);
    uint32_t old_depth;
    int r;

    header_load(root, &old_header);
    old_depth = old_header.depth;
    if (old_depth >= EXT4_EXTENT_MAX_DEPTH) {
        return EFBIG;
    }

    r = ext4_balloc_alloc_block(inode_ref, ext4_fs_inode_to_goal_block(inode_ref), &new_block_addr);
    if (r != EOK) {
        return r;
    }
    r = ext4_trans_block_get_noread(inode_ref->fs->bdev, &new_block, new_block_addr);
    if (r != EOK) {
        ext4_balloc_free_block(inode_ref, new_block_addr);
        return r;
    }
    memset(new_block.data, 0, block_size);

    uint32_t old_entries = old_header.entries_count;
    uint32_t first_logical_block = 0;
    if (old_entries > 0) {
        struct ext4_extent first_extent;
        extent_load(root + EXT4_EXTENT_HEADER_SIZE, &first_extent);
        first_logical_block = first_extent.first_block;
    }

    struct ext4_extent_header new_header = {
        .magic = EXT4_EXTENT_MAGIC,
        .entries_count = (uint16_t)old_entries,
        .max_entries_count = (uint16_t)((block_size - EXT4_EXTENT_HEADER_SIZE) / EXT4_EXTENT_SIZE),
        .depth = (uint16_t)old_depth,
        .generation = 0,
    };
    header_store(new_block.data, &new_header);
    if (old_entries > 0) {
        memcpy(new_block.data + EXT4_EXTENT_HEADER_SIZE, root + EXT4_EXTENT_HEADER_SIZE, old_entries * EXT4_EXTENT_SIZE);
    }

    r = set_extent_block_checksum(inode_ref, new_block.data, false);
    if (r != EOK) {
        ext4_block_set(inode_ref->fs->bdev, &new_block);
        ext4_balloc_free_block(inode_ref, new_block_addr);
        return r;
    }
    r = node_block_put(inode_ref, &new_block);
    if (r != EOK) {
        ext4_balloc_free_block(inode_ref, new_block_addr);
        return r;
    }

    struct ext4_extent_header root_header;
    header_load(root, &root_header);
    root_header.entries_count = 1;
    root_header.max_entries_count = EXT4_EXTENT_ROOT_MAX_ENTRIES;
    root_header.depth = (uint16_t)(old_depth + 1);
    header_store(root, &root_header);
    memset(root + EXT4_EXTENT_HEADER_SIZE, 0, 60 - EXT4_EXTENT_HEADER_SIZE);

    struct ext4_extent_index root_index = {
        .first_block = first_logical_block,
    };
    index_store_pblock(&root_index, new_block_addr);
    index_store(root + EXT4_EXTENT_HEADER_SIZE, &root_index);
    inode_ref->dirty = true;
    return EOK;
}

static int insert_new_extent(struct ext4_inode_ref *inode_ref, struct extent_search_path *path,
                             const struct ext4_extent *new_extent)
{
    struct extent_path_node *node = &path->path[path->depth];
    uint8_t *root = root_data(inode_ref);
    int r;

    if (path->depth == 0) {
        if (node->header.entries_count == 0) {
            extent_store(root + EXT4_EXTENT_HEADER_SIZE, new_extent);
            struct ext4_extent_header header = node->header;
            header.entries_count = 1;
            header_store(root, &header);
            inode_ref->dirty = true;
            return EOK;
        }
        if (node->header.entries_count == node->header.max_entries_count) {
            r = grow_indepth(inode_ref);
            if (r != EOK) {
                return r;
            }
            return insert_extent(inode_ref, new_extent->first_block, new_extent);
        }

        uint32_t pos;
        if (node->have_extent && node->position == 0 && new_extent->first_block < node->extent.first_block) {
            pos = 0; /* before the first extent */
        } else {
            pos = node->position + 1;
        }
        /* ext4-rs writes the entry without shifting; shift to keep the array
         * ordered on disk. */
        if (pos < node->header.entries_count) {
            memmove(root + EXT4_EXTENT_HEADER_SIZE + (pos + 1) * EXT4_EXTENT_SIZE,
                    root + EXT4_EXTENT_HEADER_SIZE + pos * EXT4_EXTENT_SIZE,
                    (node->header.entries_count - pos) * EXT4_EXTENT_SIZE);
        }
        extent_store(root + EXT4_EXTENT_HEADER_SIZE + pos * EXT4_EXTENT_SIZE, new_extent);
        struct ext4_extent_header header = node->header;
        header.entries_count = (uint16_t)(header.entries_count + 1);
        header_store(root, &header);
        inode_ref->dirty = true;
        return EOK;
    }

    struct ext4_block block;
    r = node_block_get(inode_ref, node->pblock_of_node, &block);
    if (r != EOK) {
        return r;
    }
    uint32_t pos;
    if (node->have_extent && node->position == 0 && new_extent->first_block < node->extent.first_block) {
        pos = 0; /* before the first extent */
    } else {
        pos = node->position + 1;
    }
    if (pos < node->header.entries_count) {
        memmove(block.data + EXT4_EXTENT_HEADER_SIZE + (pos + 1) * EXT4_EXTENT_SIZE,
                block.data + EXT4_EXTENT_HEADER_SIZE + pos * EXT4_EXTENT_SIZE,
                (node->header.entries_count - pos) * EXT4_EXTENT_SIZE);
    }
    extent_store(block.data + EXT4_EXTENT_HEADER_SIZE + pos * EXT4_EXTENT_SIZE, new_extent);
    struct ext4_extent_header header = node->header;
    header.entries_count = (uint16_t)(header.entries_count + 1);
    header_store(block.data, &header);
    r = set_extent_block_checksum(inode_ref, block.data, false);
    if (r != EOK) {
        ext4_block_set(inode_ref->fs->bdev, &block);
        return r;
    }
    r = node_block_put(inode_ref, &block);
    if (r != EOK) {
        return r;
    }
    if (pos == 0) {
        return correct_indexes(inode_ref, path, path->depth, new_extent->first_block);
    }
    return EOK;
}

static int insert_extent(struct ext4_inode_ref *inode_ref, uint32_t first_block, const struct ext4_extent *new_extent)
{
    struct extent_search_path path;
    struct extent_path_node *leaf;
    int r;

    r = find_extent(inode_ref, first_block, &path);
    if (r != EOK) {
        return r;
    }
    leaf = &path.path[path.depth];

    if (leaf->header.entries_count == 0) {
        r = insert_new_extent(inode_ref, &path, new_extent);
        return r;
    }

    if (leaf->have_extent) {
        struct ext4_extent ex = leaf->extent;
        uint32_t pos = leaf->position;
        uint32_t last_pos = leaf->header.entries_count - 1;

        /* Reject overlapping insertion: new_extent starts inside an existing
         * extent. */
        if (first_block >= ex.first_block && first_block < ex.first_block + extent_get_actual_len(&ex)) {
            return EEXIST;
        }

        if (can_merge(&ex, new_extent)) {
            r = merge_extent(inode_ref, &path, &ex, new_extent, pos);
            if (r != EOK) {
                return r;
            }
            if (leaf->pblock_of_node == 0) {
                extent_store(root_data(inode_ref) + EXT4_EXTENT_HEADER_SIZE + pos * EXT4_EXTENT_SIZE, &ex);
                inode_ref->dirty = true;
            }
            return EOK;
        }

        if (pos < last_pos && ex.first_block + extent_get_actual_len(&ex) < new_extent->first_block) {
            struct ext4_extent next;
            r = node_get_extent(inode_ref, leaf, pos + 1, &next);
            if (r != EOK) {
                return r;
            }
            if (can_merge(new_extent, &next)) {
                struct ext4_extent merged = *new_extent;
                r = merge_extent(inode_ref, &path, &merged, &next, pos + 1);
                if (r != EOK) {
                    return r;
                }
                if (leaf->pblock_of_node == 0) {
                    extent_store(root_data(inode_ref) + EXT4_EXTENT_HEADER_SIZE + (pos + 1) * EXT4_EXTENT_SIZE, &merged);
                    inode_ref->dirty = true;
                }
                return EOK;
            }
        }

        if (pos > 0 && new_extent->first_block + extent_get_actual_len(new_extent) < ex.first_block) {
            struct ext4_extent prev;
            r = node_get_extent(inode_ref, leaf, pos - 1, &prev);
            if (r != EOK) {
                return r;
            }
            if (can_merge(&prev, new_extent)) {
                r = merge_extent(inode_ref, &path, &prev, new_extent, pos - 1);
                if (r != EOK) {
                    return r;
                }
                if (leaf->pblock_of_node == 0) {
                    extent_store(root_data(inode_ref) + EXT4_EXTENT_HEADER_SIZE + (pos - 1) * EXT4_EXTENT_SIZE, &prev);
                    inode_ref->dirty = true;
                }
                return EOK;
            }
        }
    }

    if (leaf->header.entries_count < leaf->header.max_entries_count) {
        r = insert_new_extent(inode_ref, &path, new_extent);
    } else {
        r = ensure_insert_room(inode_ref, first_block, &path);
        if (r != EOK) {
            return r;
        }
        leaf = &path.path[path.depth];
        if (leaf->header.entries_count >= leaf->header.max_entries_count) {
            return EIO;
        }
        r = insert_new_extent(inode_ref, &path, new_extent);
    }
    return r;
}

static int remove_blocks(struct ext4_inode_ref *inode_ref, const struct ext4_extent *extent, uint32_t from, uint32_t to)
{
    uint32_t len = to - from + 1;
    uint32_t num = from - extent->first_block;
    ext4_fsblk_t start = (ext4_fsblk_t)(extent_get_pblock(extent) + num);
    return ext4_balloc_free_blocks(inode_ref, start, len);
}

static int write_index_at(struct ext4_inode_ref *inode_ref, const struct extent_path_node *node, uint32_t pos,
                          const struct ext4_extent_index *index)
{
    if (node->pblock_of_node == 0) {
        index_store(root_data(inode_ref) + EXT4_EXTENT_HEADER_SIZE + pos * EXT4_EXTENT_INDEX_SIZE, index);
        inode_ref->dirty = true;
        return EOK;
    }

    struct ext4_block block;
    int r = node_block_get(inode_ref, node->pblock_of_node, &block);
    if (r != EOK) {
        return r;
    }
    index_store(block.data + EXT4_EXTENT_HEADER_SIZE + pos * EXT4_EXTENT_INDEX_SIZE, index);
    r = set_extent_block_checksum(inode_ref, block.data, false);
    if (r != EOK) {
        ext4_block_set(inode_ref->fs->bdev, &block);
        return r;
    }
    return node_block_put(inode_ref, &block);
}

static int correct_indexes(struct ext4_inode_ref *inode_ref, struct extent_search_path *path, uint32_t child_level,
                           uint32_t new_first_block)
{
    while (child_level > 0) {
        uint32_t parent = child_level - 1;
        struct extent_path_node *parent_node = &path->path[parent];
        struct ext4_extent_index updated = parent_node->index;
        updated.first_block = new_first_block;
        int r = write_index_at(inode_ref, parent_node, parent_node->position, &updated);
        if (r != EOK) {
            return r;
        }
        if (parent_node->position != 0) {
            break;
        }
        child_level--;
    }
    return EOK;
}

static int remove_index(struct ext4_inode_ref *inode_ref, struct extent_search_path *path, uint32_t depth);

static int remove_leaf(struct ext4_inode_ref *inode_ref, struct extent_search_path *path, uint32_t from, uint32_t to)
{
    struct extent_path_node *leaf = &path->path[path->depth];
    uint8_t *root = root_data(inode_ref);
    struct ext4_block block;
    uint8_t *data;
    bool at_root = leaf->pblock_of_node == 0;
    uint32_t entry_count = leaf->header.entries_count;
    uint32_t pos = leaf->position;
    uint32_t new_entry_count = entry_count;
    uint32_t first_after = 0;
    int r;

    if (at_root) {
        data = root;
    } else {
        r = node_block_get(inode_ref, leaf->pblock_of_node, &block);
        if (r != EOK) {
            return r;
        }
        data = block.data;
    }

    /* Walk entries and rebuild the array starting at pos. */
    uint32_t write = pos;
    uint32_t i = pos;
    for (; i < entry_count; i++) {
        struct ext4_extent ex;
        uint16_t len;
        uint32_t start;
        uint32_t new_start;
        uint64_t newblock;

        extent_load(data + EXT4_EXTENT_HEADER_SIZE + i * EXT4_EXTENT_SIZE, &ex);
        if (ex.first_block > to) {
            break;
        }
        len = extent_get_actual_len(&ex);
        start = ex.first_block;
        new_start = ex.first_block;
        newblock = extent_get_pblock(&ex);

        if (start + len - 1 < from) {
            /* Keep the entry, compacting it forward when earlier entries
             * were removed. */
            extent_store(data + EXT4_EXTENT_HEADER_SIZE + write * EXT4_EXTENT_SIZE, &ex);
            write++;
            continue;
        }

        uint16_t new_len = 0;
        if (start < from) {
            len = (uint16_t)(len - (from - start));
            new_len = (uint16_t)(from - start);
            start = from;
        } else if (start + len - 1 > to) {
            new_len = (uint16_t)(start + len - 1 - to);
            len = (uint16_t)(len - new_len);
            new_start = to + 1;
            newblock += (to + 1 - start);
        }

        r = remove_blocks(inode_ref, &ex, start, start + len - 1);
        if (r != EOK) {
            if (!at_root) {
                ext4_block_set(inode_ref->fs->bdev, &block);
            }
            return r;
        }

        if (new_len == 0) {
            new_entry_count--;
            continue;
        }

        ex.first_block = new_start;
        bool unwritten = extent_is_unwritten(&ex);
        extent_store_pblock(&ex, newblock);
        extent_set_actual_len(&ex, new_len);
        if (unwritten) {
            extent_mark_unwritten(&ex);
        }
        extent_store(data + EXT4_EXTENT_HEADER_SIZE + write * EXT4_EXTENT_SIZE, &ex);
        write++;
    }
    /* Shift any entries after the last processed one into the compacted
     * region (ext4-rs leaves the tail in place). */
    if (write != i && i < entry_count) {
        memmove(data + EXT4_EXTENT_HEADER_SIZE + write * EXT4_EXTENT_SIZE, data + EXT4_EXTENT_HEADER_SIZE + i * EXT4_EXTENT_SIZE,
                (entry_count - i) * EXT4_EXTENT_SIZE);
    }

    struct ext4_extent_header header = leaf->header;
    header.entries_count = (uint16_t)new_entry_count;
    header_store(data, &header);
    if (new_entry_count > 0) {
        first_after = read_le32(data + EXT4_EXTENT_HEADER_SIZE);
    }

    if (at_root) {
        inode_ref->dirty = true;
    } else {
        r = set_extent_block_checksum(inode_ref, data, false);
        if (r != EOK) {
            ext4_block_set(inode_ref->fs->bdev, &block);
            return r;
        }
        r = node_block_put(inode_ref, &block);
        if (r != EOK) {
            return r;
        }
    }

    if (pos == 0 && new_entry_count > 0) {
        r = correct_indexes(inode_ref, path, path->depth, first_after);
        if (r != EOK) {
            return r;
        }
    }

    if (new_entry_count == 0) {
        if (at_root) {
            return EOK;
        }
        return remove_index(inode_ref, path, path->depth - 1);
    }
    return EOK;
}

static int remove_index_block(struct ext4_inode_ref *inode_ref, const struct ext4_extent_index *index)
{
    return ext4_balloc_free_blocks(inode_ref, index_get_pblock(index), 1);
}

static int remove_index(struct ext4_inode_ref *inode_ref, struct extent_search_path *path, uint32_t depth)
{
    struct extent_path_node *node = &path->path[depth];
    uint8_t *root = root_data(inode_ref);
    struct ext4_block block;
    uint8_t *data;
    bool at_root = node->pblock_of_node == 0;
    uint32_t pos = node->position;
    uint32_t entries = node->header.entries_count;
    uint32_t first_after = 0;
    int r;

    if (at_root) {
        data = root;
    } else {
        r = node_block_get(inode_ref, node->pblock_of_node, &block);
        if (r != EOK) {
            return r;
        }
        data = block.data;
    }

    if (pos != entries - 1) {
        memmove(data + EXT4_EXTENT_HEADER_SIZE + pos * EXT4_EXTENT_INDEX_SIZE,
                data + EXT4_EXTENT_HEADER_SIZE + (pos + 1) * EXT4_EXTENT_INDEX_SIZE,
                (entries - pos - 1) * EXT4_EXTENT_INDEX_SIZE);
        memset(data + EXT4_EXTENT_HEADER_SIZE + (entries - 1) * EXT4_EXTENT_INDEX_SIZE, 0, EXT4_EXTENT_INDEX_SIZE);
    }

    struct ext4_extent_header header = node->header;
    header.entries_count = (uint16_t)(entries - 1);
    header_store(data, &header);
    if (header.entries_count > 0) {
        first_after = read_le32(data + EXT4_EXTENT_HEADER_SIZE);
    }

    if (at_root) {
        inode_ref->dirty = true;
    } else {
        r = set_extent_block_checksum(inode_ref, data, false);
        if (r != EOK) {
            ext4_block_set(inode_ref->fs->bdev, &block);
            return r;
        }
        r = node_block_put(inode_ref, &block);
        if (r != EOK) {
            return r;
        }
    }

    r = remove_index_block(inode_ref, &node->index);
    if (r != EOK) {
        return r;
    }

    if (pos == 0 && header.entries_count > 0) {
        r = correct_indexes(inode_ref, path, depth, first_after);
        if (r != EOK) {
            return r;
        }
    }

    if (header.entries_count == 0) {
        if (at_root) {
            /* Collapse an empty index root back to an empty leaf. */
            struct ext4_extent_header root_header;
            header_load(root, &root_header);
            root_header.depth = 0;
            header_store(root, &root_header);
            inode_ref->dirty = true;
        } else {
            /* Recursively remove the now-empty index node from its parent. */
            return remove_index(inode_ref, path, depth - 1);
        }
    }
    return EOK;
}

static int remove_middle_of_extent(struct ext4_inode_ref *inode_ref, uint32_t from, uint32_t to)
{
    struct extent_search_path path;
    int r = ensure_insert_room(inode_ref, from, &path);
    if (r != EOK) {
        return r;
    }

    struct extent_path_node *leaf = &path.path[path.depth];
    struct ext4_extent original = leaf->extent;
    uint32_t original_len = extent_get_actual_len(&original);
    uint64_t original_end = (uint64_t)original.first_block + original_len - 1;
    if (leaf->header.entries_count >= leaf->header.max_entries_count || original.first_block >= from || to >= original_end) {
        return EIO;
    }

    struct ext4_extent prefix = original;
    struct ext4_extent suffix = {0};
    bool unwritten = extent_is_unwritten(&original);
    extent_set_actual_len(&prefix, (uint16_t)(from - original.first_block));
    if (unwritten) {
        extent_mark_unwritten(&prefix);
    }
    suffix.first_block = to + 1;
    extent_set_actual_len(&suffix, (uint16_t)(original_end - to));
    extent_store_pblock(&suffix, extent_get_pblock(&original) + (to + 1 - original.first_block));
    if (unwritten) {
        extent_mark_unwritten(&suffix);
    }

    struct ext4_block block;
    uint8_t *data;
    bool at_root = leaf->pblock_of_node == 0;
    if (at_root) {
        data = root_data(inode_ref);
    } else {
        r = node_block_get(inode_ref, leaf->pblock_of_node, &block);
        if (r != EOK) {
            return r;
        }
        data = block.data;
    }

    uint32_t pos = leaf->position;
    uint32_t entries = leaf->header.entries_count;
    if (pos + 1 < entries) {
        memmove(data + EXT4_EXTENT_HEADER_SIZE + (pos + 2) * EXT4_EXTENT_SIZE,
                data + EXT4_EXTENT_HEADER_SIZE + (pos + 1) * EXT4_EXTENT_SIZE, (entries - pos - 1) * EXT4_EXTENT_SIZE);
    }
    extent_store(data + EXT4_EXTENT_HEADER_SIZE + pos * EXT4_EXTENT_SIZE, &prefix);
    extent_store(data + EXT4_EXTENT_HEADER_SIZE + (pos + 1) * EXT4_EXTENT_SIZE, &suffix);
    struct ext4_extent_header header = leaf->header;
    header.entries_count = (uint16_t)(entries + 1);
    header_store(data, &header);

    if (at_root) {
        inode_ref->dirty = true;
    } else {
        r = set_extent_block_checksum(inode_ref, data, false);
        if (r != EOK) {
            ext4_block_set(inode_ref->fs->bdev, &block);
            return r;
        }
        r = node_block_put(inode_ref, &block);
        if (r != EOK) {
            return r;
        }
    }

    return remove_blocks(inode_ref, &original, from, to);
}

static uint32_t extent_block_checksum(struct ext4_inode_ref *inode_ref, const uint8_t *data, uint32_t tail_offset)
{
    uint32_t checksum = EXT4_CRC32_INIT;
    uint32_t ino_index = inode_ref->index;
    uint32_t ino_gen = inode_ref->inode->generation;

    checksum = ext4_crc32c(checksum, inode_ref->fs->sb.uuid, UUID_SIZE);
    checksum = ext4_crc32c(checksum, &ino_index, 4);
    checksum = ext4_crc32c(checksum, &ino_gen, 4);
    checksum = ext4_crc32c(checksum, data, tail_offset);
    return checksum;
}

static int set_extent_block_checksum(struct ext4_inode_ref *inode_ref, uint8_t *data, bool is_root)
{
    struct ext4_extent_header header;
    uint32_t tail_offset;
    uint32_t checksum;
    uint32_t block_size = ext4_sb_get_block_size(&inode_ref->fs->sb);

    if (is_root) {
        return EOK;
    }
    if (!(ext4_get32(&inode_ref->fs->sb, features_read_only) & EXT4_FRO_COM_METADATA_CSUM)) {
        return EOK;
    }

    header_load(data, &header);
    if (header.magic != EXT4_EXTENT_MAGIC) {
        return EINVAL;
    }
    tail_offset = EXT4_EXTENT_HEADER_SIZE + header.max_entries_count * EXT4_EXTENT_SIZE;
    if (tail_offset > block_size || tail_offset + sizeof(uint32_t) > block_size) {
        return EIO;
    }
    checksum = extent_block_checksum(inode_ref, data, tail_offset);
    write_le32(data + tail_offset, checksum);
    return EOK;
}

void ext4_extent_tree_init(struct ext4_inode_ref *inode_ref)
{
    struct ext4_extent_header header = {
        .magic = EXT4_EXTENT_MAGIC,
        .entries_count = 0,
        .max_entries_count = EXT4_EXTENT_ROOT_MAX_ENTRIES,
        .depth = 0,
        .generation = 0,
    };

    header_store(root_data(inode_ref), &header);
    memset(root_data(inode_ref) + EXT4_EXTENT_HEADER_SIZE, 0, 60 - EXT4_EXTENT_HEADER_SIZE);
    ext4_inode_set_flag(inode_ref->inode, EXT4_INODE_FLAG_EXTENTS);
    inode_ref->dirty = true;
}

int ext4_extent_get_blocks(struct ext4_inode_ref *inode_ref, ext4_lblk_t iblock, uint32_t max_blocks, ext4_fsblk_t *result,
                           bool create, uint32_t *blocks_count)
{
    struct extent_search_path path;
    struct extent_path_node *leaf;
    int r;

    if (result != NULL) {
        *result = 0;
    }
    if (blocks_count != NULL) {
        *blocks_count = 0;
    }

    r = find_extent(inode_ref, iblock, &path);
    if (r != EOK) {
        return r;
    }
    leaf = &path.path[path.depth];

    if (leaf->have_extent && leaf->header.entries_count > 0 && leaf->extent.first_block <= iblock &&
        iblock < leaf->extent.first_block + extent_get_actual_len(&leaf->extent)) {
        if (extent_is_unwritten(&leaf->extent)) {
            if (create) {
                return ENOTSUP;
            }
            return EOK; /* unwritten extents read as zeroes */
        }
        uint32_t off = iblock - leaf->extent.first_block;
        if (result != NULL) {
            *result = (ext4_fsblk_t)(extent_get_pblock(&leaf->extent) + off);
        }
        if (blocks_count != NULL) {
            uint32_t avail = extent_get_actual_len(&leaf->extent) - off;
            *blocks_count = max_blocks < avail ? max_blocks : avail;
        }
        return EOK;
    }

    if (!create) {
        return EOK; /* sparse */
    }

    r = require_journaled_mutation(inode_ref);
    if (r != EOK) {
        return r;
    }

    /* Allocate as many contiguous blocks as possible, capped at the largest
     * encodable initialized extent and at the next logical mapping. */
    uint32_t cap = max_blocks < EXT_INIT_MAX_LEN ? max_blocks : EXT_INIT_MAX_LEN;
    if (leaf->have_extent && leaf->header.entries_count > 0) {
        if (iblock < leaf->extent.first_block) {
            uint32_t gap = leaf->extent.first_block - iblock;
            cap = cap < gap ? cap : gap;
        } else if (leaf->position + 1 < leaf->header.entries_count) {
            struct ext4_extent next;
            r = node_get_extent(inode_ref, leaf, leaf->position + 1, &next);
            if (r != EOK) {
                return r;
            }
            uint32_t gap = next.first_block - iblock;
            cap = cap < gap ? cap : gap;
        } else {
            struct ext4_extent next;
            r = find_next_extent(inode_ref, &path, &next);
            if (r == EOK) {
                if (next.first_block <= iblock) {
                    return EIO;
                }
                uint32_t gap = next.first_block - iblock;
                cap = cap < gap ? cap : gap;
            } else if (r != ENOENT) {
                return r;
            }
        }
    }
    if (cap == 0) {
        return EEXIST;
    }

    ext4_fsblk_t goal = ext4_fs_inode_to_goal_block(inode_ref);
    if (leaf->have_extent && leaf->header.entries_count > 0 &&
        leaf->extent.first_block + extent_get_actual_len(&leaf->extent) == iblock) {
        /* Continue after the previous extent so sequential writes merge. */
        goal = extent_get_pblock(&leaf->extent) + extent_get_actual_len(&leaf->extent);
    }
    ext4_fsblk_t first = 0;
    uint32_t count = 0;
    while (count < cap) {
        ext4_fsblk_t candidate = 0;
        r = ext4_balloc_alloc_block(inode_ref, goal, &candidate);
        if (r != EOK) {
            if (count == 0) {
                return r;
            }
            if (r != ENOSPC) {
                int free_r = ext4_balloc_free_blocks(inode_ref, first, count);
                return free_r != EOK ? free_r : r;
            }
            break;
        }
        if (count == 0) {
            first = candidate;
            count = 1;
        } else if (candidate == first + count) {
            count++;
        } else {
            r = ext4_balloc_free_block(inode_ref, candidate);
            if (r != EOK) {
                int free_r = ext4_balloc_free_blocks(inode_ref, first, count);
                return free_r != EOK ? free_r : r;
            }
            break;
        }
        goal = candidate + 1;
    }
    if (count == 0) {
        return ENOSPC;
    }

    struct ext4_extent new_extent = {
        .first_block = iblock,
        .block_count = (uint16_t)count,
    };
    extent_store_pblock(&new_extent, first);
    r = insert_extent(inode_ref, iblock, &new_extent);
    if (r != EOK) {
        int free_r = ext4_balloc_free_blocks(inode_ref, first, count);
        return free_r != EOK ? free_r : r;
    }
    if (result != NULL) {
        *result = first;
    }
    if (blocks_count != NULL) {
        *blocks_count = count;
    }
    return EOK;
}

int ext4_extent_remove_space(struct ext4_inode_ref *inode_ref, ext4_lblk_t from, ext4_lblk_t to)
{
    int transaction_error = require_journaled_mutation(inode_ref);
    if (transaction_error != EOK) {
        return transaction_error;
    }

    while (from <= to) {
        struct extent_search_path path;
        struct extent_path_node *leaf;
        int r = find_extent(inode_ref, from, &path);
        if (r != EOK) {
            return r;
        }
        leaf = &path.path[path.depth];

        if (!leaf->have_extent || leaf->header.entries_count == 0 || leaf->extent.first_block > to) {
            /* No mapping at this logical block (sparse) or past the range. */
            break;
        }

        uint32_t ex_first = leaf->extent.first_block;
        uint32_t ex_last = ex_first + extent_get_actual_len(&leaf->extent) - 1;

        /* Middle removal fully inside one extent: split it into prefix and
         * suffix. */
        if (ex_first < from && to < ex_last) {
            return remove_middle_of_extent(inode_ref, from, to);
        }

        if (ex_last < from) {
            /* The search returned the extent preceding a hole. Advance to the
             * next mapped extent, if any, so from strictly progresses. */
            struct ext4_extent next;
            r = find_next_extent(inode_ref, &path, &next);
            if (r == ENOENT) {
                break;
            }
            if (r != EOK) {
                return r;
            }
            from = next.first_block;
            continue;
        }

        uint32_t remove_to = ex_last < to ? ex_last : to;

        r = remove_leaf(inode_ref, &path, from, remove_to);
        if (r != EOK) {
            return r;
        }
        if (remove_to == UINT32_MAX) {
            break;
        }
        from = remove_to + 1;
    }
    return EOK;
}

#endif /* CONFIG_EXTENT_ENABLE && CONFIG_EXTENTS_ENABLE */
