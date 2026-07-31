/*
 * SPDX-FileCopyrightText: 2026 esp_lwext4 contributors
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <ext4_errno.h>
#include <ext4_types.h>
#include <ext4_fs.h>
#include <ext4_xattr.h>

/*
 * ext4.c always contains the public xattr entry points, even when
 * CONFIG_XATTR_ENABLE is zero. These symbols keep the BSD-only build linkable
 * without compiling lwext4's GPL-licensed ext4_xattr.c. They intentionally do
 * not implement xattr parsing or storage.
 */
const char *ext4_extract_xattr_name(const char *full_name, size_t full_name_len, uint8_t *name_index, size_t *name_len,
                                    bool *found)
{
    (void)full_name;
    (void)full_name_len;
    (void)name_index;
    (void)name_len;
    if (found) {
        *found = false;
    }
    return NULL;
}

const char *ext4_get_xattr_name_prefix(uint8_t name_index, size_t *ret_prefix_len)
{
    (void)name_index;
    if (ret_prefix_len) {
        *ret_prefix_len = 0;
    }
    return "";
}

int ext4_xattr_list(struct ext4_inode_ref *inode_ref, struct ext4_xattr_list_entry *list, size_t *list_len)
{
    (void)inode_ref;
    (void)list;
    (void)list_len;
    return ENOTSUP;
}

int ext4_xattr_get(struct ext4_inode_ref *inode_ref, uint8_t name_index, const char *name, size_t name_len, void *buf,
                   size_t buf_size, size_t *data_size)
{
    (void)inode_ref;
    (void)name_index;
    (void)name;
    (void)name_len;
    (void)buf;
    (void)buf_size;
    (void)data_size;
    return ENOTSUP;
}

int ext4_xattr_remove(struct ext4_inode_ref *inode_ref, uint8_t name_index, const char *name, size_t name_len)
{
    (void)inode_ref;
    (void)name_index;
    (void)name;
    (void)name_len;
    return ENOTSUP;
}

int ext4_xattr_set(struct ext4_inode_ref *inode_ref, uint8_t name_index, const char *name, size_t name_len, const void *value,
                   size_t value_len)
{
    (void)inode_ref;
    (void)name_index;
    (void)name;
    (void)name_len;
    (void)value;
    (void)value_len;
    return ENOTSUP;
}
