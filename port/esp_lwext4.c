/*
 * SPDX-FileCopyrightText: 2026 esp_lwext4 contributors
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "esp_lwext4.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>

#include "esp_log.h"
#include "esp_vfs.h"
#include "esp_vfs_ops.h"
#include "ext4.h"
#include "ext4_inode.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

typedef struct {
    ext4_file file;
    char *path;
    int flags;
    bool used;
} esp_lwext4_file_t;

typedef struct esp_lwext4 {
    char *base_path;
    char *mount_point;
    esp_lwext4_file_t *files;
    size_t max_files;
    size_t open_files;
    size_t open_dirs;
    bool read_only;
    SemaphoreHandle_t lock;
    struct esp_lwext4 *next;
} esp_lwext4_t;

typedef struct {
    DIR dir;
    ext4_dir ext4;
    struct dirent entry;
    long offset;
    esp_lwext4_t *ctx;
} esp_lwext4_dir_t;

static const char *TAG = "esp_lwext4";
static const uint8_t s_zeroes[128] = {0};
static esp_lwext4_t *s_contexts;
static SemaphoreHandle_t s_contexts_lock;
static portMUX_TYPE s_contexts_lock_guard = portMUX_INITIALIZER_UNLOCKED;

static int vfs_lwext4_open(void *ctx, const char *path, int flags, int mode);
static ssize_t vfs_lwext4_write(void *ctx, int fd, const void *data, size_t size);
static off_t vfs_lwext4_lseek(void *ctx, int fd, off_t offset, int whence);
static ssize_t vfs_lwext4_read(void *ctx, int fd, void *dst, size_t size);
static ssize_t vfs_lwext4_pread(void *ctx, int fd, void *dst, size_t size,
                                off_t offset);
static ssize_t vfs_lwext4_pwrite(void *ctx, int fd, const void *src,
                                 size_t size, off_t offset);
static int vfs_lwext4_close(void *ctx, int fd);
static int vfs_lwext4_fstat(void *ctx, int fd, struct stat *st);
static int vfs_lwext4_fsync(void *ctx, int fd);

#ifdef CONFIG_VFS_SUPPORT_DIR
static int vfs_lwext4_stat(void *ctx, const char *path, struct stat *st);
static int vfs_lwext4_link(void *ctx, const char *path,
                           const char *hardlink_path);
static int vfs_lwext4_unlink(void *ctx, const char *path);
static int vfs_lwext4_rename(void *ctx, const char *src, const char *dst);
static DIR *vfs_lwext4_opendir(void *ctx, const char *name);
static struct dirent *vfs_lwext4_readdir(void *ctx, DIR *pdir);
static int vfs_lwext4_readdir_r(void *ctx, DIR *pdir, struct dirent *entry,
                                struct dirent **out_dirent);
static long vfs_lwext4_telldir(void *ctx, DIR *pdir);
static void vfs_lwext4_seekdir(void *ctx, DIR *pdir, long offset);
static int vfs_lwext4_closedir(void *ctx, DIR *pdir);
static int vfs_lwext4_mkdir(void *ctx, const char *name, mode_t mode);
static int vfs_lwext4_rmdir(void *ctx, const char *name);
static int vfs_lwext4_access(void *ctx, const char *path, int amode);
static int vfs_lwext4_truncate(void *ctx, const char *path, off_t length);
static int vfs_lwext4_ftruncate(void *ctx, int fd, off_t length);
static int vfs_lwext4_utime(void *ctx, const char *path,
                            const struct utimbuf *times);
static bool path_has_dot_component(const char *path);
static bool path_is_mount_root(const char *relative_path);
#endif

#ifdef CONFIG_VFS_SUPPORT_DIR
static const esp_vfs_dir_ops_t s_vfs_lwext4_dir_ops = {
    .stat_p = vfs_lwext4_stat,
    .link_p = vfs_lwext4_link,
    .unlink_p = vfs_lwext4_unlink,
    .rename_p = vfs_lwext4_rename,
    .opendir_p = vfs_lwext4_opendir,
    .readdir_p = vfs_lwext4_readdir,
    .readdir_r_p = vfs_lwext4_readdir_r,
    .telldir_p = vfs_lwext4_telldir,
    .seekdir_p = vfs_lwext4_seekdir,
    .closedir_p = vfs_lwext4_closedir,
    .mkdir_p = vfs_lwext4_mkdir,
    .rmdir_p = vfs_lwext4_rmdir,
    .access_p = vfs_lwext4_access,
    .truncate_p = vfs_lwext4_truncate,
    .ftruncate_p = vfs_lwext4_ftruncate,
    .utime_p = vfs_lwext4_utime,
};
#endif

static const esp_vfs_fs_ops_t s_vfs_lwext4_ops = {
    .write_p = vfs_lwext4_write,
    .lseek_p = vfs_lwext4_lseek,
    .read_p = vfs_lwext4_read,
    .pread_p = vfs_lwext4_pread,
    .pwrite_p = vfs_lwext4_pwrite,
    .open_p = vfs_lwext4_open,
    .close_p = vfs_lwext4_close,
    .fstat_p = vfs_lwext4_fstat,
    .fsync_p = vfs_lwext4_fsync,
#ifdef CONFIG_VFS_SUPPORT_DIR
    .dir = &s_vfs_lwext4_dir_ops,
#endif
};

static int fail_with_errno(int error)
{
    errno = error != EOK ? error : EIO;
    return -1;
}

static bool take_lock(SemaphoreHandle_t lock)
{
    if (xSemaphoreTake(lock, portMAX_DELAY) == pdTRUE) {
        return true;
    }
    errno = EIO;
    return false;
}

static void give_lock(SemaphoreHandle_t lock)
{
    xSemaphoreGive(lock);
}

static SemaphoreHandle_t contexts_lock_get(void)
{
    SemaphoreHandle_t candidate;

    portENTER_CRITICAL(&s_contexts_lock_guard);
    SemaphoreHandle_t lock = s_contexts_lock;
    portEXIT_CRITICAL(&s_contexts_lock_guard);
    if (lock != NULL) {
        return lock;
    }

    candidate = xSemaphoreCreateMutex();
    if (candidate == NULL) {
        return NULL;
    }

    portENTER_CRITICAL(&s_contexts_lock_guard);
    if (s_contexts_lock == NULL) {
        s_contexts_lock = candidate;
        candidate = NULL;
    }
    lock = s_contexts_lock;
    portEXIT_CRITICAL(&s_contexts_lock_guard);

    if (candidate != NULL) {
        vSemaphoreDelete(candidate);
    }
    return lock;
}

static char *normalize_mount_point(const char *mount_point)
{
    size_t length;
    bool needs_slash;
    char *normalized;

    if (mount_point == NULL || mount_point[0] != '/') {
        return NULL;
    }

    length = strlen(mount_point);
    if (length == 0 || length > CONFIG_EXT4_MAX_MP_NAME) {
        return NULL;
    }

    needs_slash = mount_point[length - 1] != '/';
    if (needs_slash && length == CONFIG_EXT4_MAX_MP_NAME) {
        return NULL;
    }

    normalized = malloc(length + (needs_slash ? 2 : 1));
    if (normalized == NULL) {
        return NULL;
    }

    memcpy(normalized, mount_point, length);
    if (needs_slash) {
        normalized[length++] = '/';
    }
    normalized[length] = '\0';
    return normalized;
}

static char *make_path(const esp_lwext4_t *ctx, const char *vfs_path)
{
    size_t mount_length;
    size_t path_length;
    const char *suffix;
    char *path;

    if (vfs_path == NULL || vfs_path[0] != '/') {
        errno = EINVAL;
        return NULL;
    }

    mount_length = strlen(ctx->mount_point);
    suffix = vfs_path + 1;
    path_length = strlen(suffix);
    if (mount_length > SIZE_MAX - path_length - 1) {
        errno = ENAMETOOLONG;
        return NULL;
    }

    path = malloc(mount_length + path_length + 1);
    if (path == NULL) {
        errno = ENOMEM;
        return NULL;
    }

    memcpy(path, ctx->mount_point, mount_length);
    memcpy(path + mount_length, suffix, path_length + 1);
    return path;
}

static esp_lwext4_file_t *file_get(esp_lwext4_t *ctx, int fd)
{
    if (fd < 0 || (size_t)fd >= ctx->max_files || !ctx->files[fd].used) {
        errno = EBADF;
        return NULL;
    }
    return &ctx->files[fd];
}

static size_t io_size_limit(size_t size)
{
    const size_t ssize_max = SIZE_MAX >> 1;
    return size > ssize_max ? ssize_max : size;
}

static uint64_t off_t_max_value(void)
{
    return (UINT64_C(1) << (sizeof(off_t) * CHAR_BIT - 1)) - 1;
}

/*
 * lwext4 caches fsize independently in every ext4_file. Its SEEK_END uses that
 * cached value, so it is unsafe for O_APPEND when another handle has extended
 * the same inode. Refresh the size from the inode and update both cached fields
 * while the VFS mount lock excludes other VFS writes.
 */
static int current_file_size_locked(esp_lwext4_file_t *file,
                                    uint64_t *file_size)
{
    struct ext4_inode inode;
    struct ext4_sblock *superblock;
    uint32_t inode_number;
    int result;

    if (file_size == NULL) {
        return EINVAL;
    }
    result = ext4_raw_inode_fill(file->path, &inode_number, &inode);
    if (result != EOK) {
        return result;
    }
    if (inode_number != file->file.inode) {
        return ESTALE;
    }

    result = ext4_get_sblock(file->path, &superblock);
    if (result != EOK) {
        return result;
    }
    *file_size = ext4_inode_get_size(superblock, &inode);
    return EOK;
}

static int seek_to_current_end_locked(esp_lwext4_file_t *file)
{
    uint64_t file_size;
    int result = current_file_size_locked(file, &file_size);

    if (result != EOK) {
        return result;
    }
    if (file_size > INT64_MAX) {
        return EOVERFLOW;
    }

    file->file.fsize = file_size;
    return ext4_fseek(&file->file, (int64_t)file_size, SEEK_SET);
}

static int resize_file_locked(esp_lwext4_file_t *file, uint64_t new_size)
{
    uint64_t current_size;
    uint64_t original_position;
    int result;

    result = current_file_size_locked(file, &current_size);
    if (result != EOK) {
        return result;
    }
    if (new_size <= current_size) {
        return ext4_ftruncate(&file->file, new_size);
    }

    /*
     * lwext4 cannot seek beyond EOF and its truncate implementation is
     * shrink-only. POSIX growth therefore has to materialize the new range.
     */
    original_position = ext4_ftell(&file->file);
    file->file.fsize = current_size;
    result = ext4_fseek(&file->file, (int64_t)current_size, SEEK_SET);

    while (result == EOK && current_size < new_size) {
        size_t request = sizeof(s_zeroes);
        size_t written = 0;
        uint64_t remaining = new_size - current_size;

        if (remaining < request) {
            request = (size_t)remaining;
        }
        result = ext4_fwrite(&file->file, s_zeroes, request, &written);
        if (result == EOK && written == 0) {
            result = EIO;
        }
        current_size += written;
    }

    /*
     * ftruncate() does not alter the open-file offset. The original offset is
     * within the old EOF, so lwext4 can represent it after successful or
     * partial growth.
     */
    int restore_result = ext4_fseek(&file->file, (int64_t)original_position,
                                    SEEK_SET);
    if (result == EOK && restore_result != EOK) {
        result = restore_result;
    }
    return result;
}

static int stat_path_locked(const char *path, struct stat *st)
{
    struct ext4_inode inode;
    struct ext4_sblock *superblock;
    struct ext4_mount_stats mount_stats;
    uint32_t inode_number;
    uint32_t mode;
    int result;

    memset(st, 0, sizeof(*st));

    result = ext4_raw_inode_fill(path, &inode_number, &inode);
    if (result != EOK) {
        return fail_with_errno(result);
    }

    result = ext4_get_sblock(path, &superblock);
    if (result != EOK) {
        return fail_with_errno(result);
    }

    mode = ext4_inode_get_mode(superblock, &inode);
    st->st_ino = inode_number;
    st->st_mode = mode;
    st->st_nlink = ext4_inode_get_links_cnt(&inode);
    st->st_uid = ext4_inode_get_uid(&inode);
    st->st_gid = ext4_inode_get_gid(&inode);
    uint64_t file_size = ext4_inode_get_size(superblock, &inode);
    if (file_size > off_t_max_value()) {
        return fail_with_errno(EOVERFLOW);
    }
    st->st_size = (off_t)file_size;
    st->st_atime = ext4_inode_get_access_time(&inode);
    st->st_mtime = ext4_inode_get_modif_time(&inode);
    st->st_ctime = ext4_inode_get_change_inode_time(&inode);
    st->st_blocks = (file_size + 511) / 512;

    if (ext4_mount_point_stats(path, &mount_stats) == EOK) {
        st->st_blksize = mount_stats.block_size;
    }
    return 0;
}

static int vfs_lwext4_open(void *opaque, const char *path, int flags, int mode)
{
    esp_lwext4_t *ctx = opaque;
    esp_lwext4_file_t *slot = NULL;
    char *full_path;
    int result;
    int fd = -1;

    (void)mode;
    if (ctx->read_only && ((flags & O_ACCMODE) != O_RDONLY ||
                           (flags & (O_CREAT | O_TRUNC)) != 0)) {
        return fail_with_errno(EROFS);
    }

    full_path = make_path(ctx, path);
    if (full_path == NULL) {
        return -1;
    }
    if (!take_lock(ctx->lock)) {
        free(full_path);
        return -1;
    }

    for (size_t i = 0; i < ctx->max_files; ++i) {
        if (!ctx->files[i].used) {
            slot = &ctx->files[i];
            fd = (int)i;
            break;
        }
    }
    if (slot == NULL) {
        give_lock(ctx->lock);
        free(full_path);
        return fail_with_errno(EMFILE);
    }

    if ((flags & (O_CREAT | O_EXCL)) == (O_CREAT | O_EXCL)) {
        result = ext4_inode_exist(full_path, EXT4_DE_UNKNOWN);
        if (result == EOK) {
            give_lock(ctx->lock);
            free(full_path);
            return fail_with_errno(EEXIST);
        }
        if (result != ENOENT) {
            give_lock(ctx->lock);
            free(full_path);
            return fail_with_errno(result);
        }
    } else {
        result = ext4_inode_exist(full_path, EXT4_DE_DIR);
        if (result == EOK) {
            give_lock(ctx->lock);
            free(full_path);
            return fail_with_errno(EISDIR);
        }
        if (result != ENOENT) {
            give_lock(ctx->lock);
            free(full_path);
            return fail_with_errno(result);
        }
    }

    memset(&slot->file, 0, sizeof(slot->file));
    result = ext4_fopen2(&slot->file, full_path, flags);
    if (result != EOK) {
        give_lock(ctx->lock);
        free(full_path);
        return fail_with_errno(result);
    }

    slot->path = full_path;
    slot->flags = flags;
    slot->used = true;
    ++ctx->open_files;
    give_lock(ctx->lock);
    return fd;
}

static ssize_t vfs_lwext4_write(void *opaque, int fd, const void *data,
                                size_t size)
{
    esp_lwext4_t *ctx = opaque;
    esp_lwext4_file_t *file;
    size_t written = 0;
    int result;

    if (ctx->read_only) {
        return fail_with_errno(EROFS);
    }
    if (!take_lock(ctx->lock)) {
        return -1;
    }
    file = file_get(ctx, fd);
    if (file == NULL) {
        give_lock(ctx->lock);
        return -1;
    }
    if ((file->flags & O_ACCMODE) == O_RDONLY) {
        give_lock(ctx->lock);
        return fail_with_errno(EBADF);
    }
    if ((file->flags & O_APPEND) != 0) {
        result = seek_to_current_end_locked(file);
        if (result != EOK) {
            give_lock(ctx->lock);
            return fail_with_errno(result);
        }
    }

    result = ext4_fwrite(&file->file, data, io_size_limit(size), &written);
    give_lock(ctx->lock);
    if (result != EOK) {
        return fail_with_errno(result);
    }
    return (ssize_t)written;
}

static ssize_t vfs_lwext4_read(void *opaque, int fd, void *dst, size_t size)
{
    esp_lwext4_t *ctx = opaque;
    esp_lwext4_file_t *file;
    size_t read_count = 0;
    int result;

    if (!take_lock(ctx->lock)) {
        return -1;
    }
    file = file_get(ctx, fd);
    if (file == NULL) {
        give_lock(ctx->lock);
        return -1;
    }
    if ((file->flags & O_ACCMODE) == O_WRONLY) {
        give_lock(ctx->lock);
        return fail_with_errno(EBADF);
    }

    result = ext4_fread(&file->file, dst, io_size_limit(size), &read_count);
    give_lock(ctx->lock);
    if (result != EOK) {
        return fail_with_errno(result);
    }
    return (ssize_t)read_count;
}

static off_t seek_locked(esp_lwext4_file_t *file, off_t offset, int whence)
{
    int64_t lwext4_offset = offset;
    uint64_t position;
    int result;

    if (whence == SEEK_END) {
        if (offset > 0) {
            return fail_with_errno(EINVAL);
        }
        if (offset == (off_t)(-off_t_max_value() - 1)) {
            return fail_with_errno(EINVAL);
        }
        lwext4_offset = -(int64_t)offset;
    } else if (whence != SEEK_SET && whence != SEEK_CUR) {
        return fail_with_errno(EINVAL);
    }

    result = ext4_fseek(&file->file, lwext4_offset, (uint32_t)whence);
    if (result != EOK) {
        return fail_with_errno(result);
    }

    position = ext4_ftell(&file->file);
    if (position > off_t_max_value()) {
        return fail_with_errno(EOVERFLOW);
    }
    return (off_t)position;
}

static off_t vfs_lwext4_lseek(void *opaque, int fd, off_t offset, int whence)
{
    esp_lwext4_t *ctx = opaque;
    esp_lwext4_file_t *file;
    off_t result;

    if (!take_lock(ctx->lock)) {
        return -1;
    }
    file = file_get(ctx, fd);
    if (file == NULL) {
        give_lock(ctx->lock);
        return -1;
    }
    result = seek_locked(file, offset, whence);
    give_lock(ctx->lock);
    return result;
}

static ssize_t positioned_io(void *opaque, int fd, void *buffer, size_t size,
                             off_t offset, bool write_operation)
{
    esp_lwext4_t *ctx = opaque;
    esp_lwext4_file_t *file;
    uint64_t original_position;
    size_t transferred = 0;
    int result;
    int restore_result;

    if (offset < 0) {
        return fail_with_errno(EINVAL);
    }
    if (write_operation && ctx->read_only) {
        return fail_with_errno(EROFS);
    }
    if (!take_lock(ctx->lock)) {
        return -1;
    }
    file = file_get(ctx, fd);
    if (file == NULL) {
        give_lock(ctx->lock);
        return -1;
    }
    if ((write_operation && (file->flags & O_ACCMODE) == O_RDONLY) ||
        (!write_operation && (file->flags & O_ACCMODE) == O_WRONLY)) {
        give_lock(ctx->lock);
        return fail_with_errno(EBADF);
    }

    original_position = ext4_ftell(&file->file);
    if (original_position > INT64_MAX) {
        give_lock(ctx->lock);
        return fail_with_errno(EOVERFLOW);
    }
    result = ext4_fseek(&file->file, offset, SEEK_SET);
    if (result == EOK) {
        if (write_operation) {
            result = ext4_fwrite(&file->file, buffer, io_size_limit(size),
                                 &transferred);
        } else {
            result = ext4_fread(&file->file, buffer, io_size_limit(size),
                                &transferred);
        }
    }
    restore_result = ext4_fseek(&file->file, (int64_t)original_position,
                                SEEK_SET);
    give_lock(ctx->lock);

    if (result != EOK) {
        return fail_with_errno(result);
    }
    if (restore_result != EOK) {
        return fail_with_errno(restore_result);
    }
    return (ssize_t)transferred;
}

static ssize_t vfs_lwext4_pread(void *ctx, int fd, void *dst, size_t size,
                                off_t offset)
{
    return positioned_io(ctx, fd, dst, size, offset, false);
}

static ssize_t vfs_lwext4_pwrite(void *ctx, int fd, const void *src,
                                 size_t size, off_t offset)
{
    return positioned_io(ctx, fd, (void *)src, size, offset, true);
}

static int vfs_lwext4_close(void *opaque, int fd)
{
    esp_lwext4_t *ctx = opaque;
    esp_lwext4_file_t *file;
    char *path;
    int result;

    if (!take_lock(ctx->lock)) {
        return -1;
    }
    file = file_get(ctx, fd);
    if (file == NULL) {
        give_lock(ctx->lock);
        return -1;
    }

    result = ext4_fclose(&file->file);
    if (result != EOK) {
        give_lock(ctx->lock);
        return fail_with_errno(result);
    }

    path = file->path;
    memset(file, 0, sizeof(*file));
    --ctx->open_files;
    give_lock(ctx->lock);
    free(path);
    return 0;
}

static int vfs_lwext4_fstat(void *opaque, int fd, struct stat *st)
{
    esp_lwext4_t *ctx = opaque;
    esp_lwext4_file_t *file;
    int result;

    if (st == NULL) {
        return fail_with_errno(EINVAL);
    }
    if (!take_lock(ctx->lock)) {
        return -1;
    }
    file = file_get(ctx, fd);
    result = file != NULL ? stat_path_locked(file->path, st) : -1;
    give_lock(ctx->lock);
    return result;
}

static int vfs_lwext4_fsync(void *opaque, int fd)
{
    esp_lwext4_t *ctx = opaque;
    int result;

    if (!take_lock(ctx->lock)) {
        return -1;
    }
    if (file_get(ctx, fd) == NULL) {
        give_lock(ctx->lock);
        return -1;
    }
    result = ext4_cache_flush(ctx->mount_point);
    give_lock(ctx->lock);
    return result == EOK ? 0 : fail_with_errno(result);
}

#ifdef CONFIG_VFS_SUPPORT_DIR

static int vfs_lwext4_stat(void *opaque, const char *path, struct stat *st)
{
    esp_lwext4_t *ctx = opaque;
    char *full_path;
    int result;

    if (st == NULL) {
        return fail_with_errno(EINVAL);
    }
    full_path = make_path(ctx, path);
    if (full_path == NULL) {
        return -1;
    }
    if (!take_lock(ctx->lock)) {
        free(full_path);
        return -1;
    }
    result = stat_path_locked(full_path, st);
    give_lock(ctx->lock);
    free(full_path);
    return result;
}

static int two_path_operation(esp_lwext4_t *ctx, const char *first,
                              const char *second,
                              int (*operation)(const char *, const char *))
{
    char *first_path = make_path(ctx, first);
    char *second_path;
    int result;

    if (first_path == NULL) {
        return -1;
    }
    second_path = make_path(ctx, second);
    if (second_path == NULL) {
        free(first_path);
        return -1;
    }
    if (!take_lock(ctx->lock)) {
        free(second_path);
        free(first_path);
        return -1;
    }
    result = operation(first_path, second_path);
    give_lock(ctx->lock);
    free(second_path);
    free(first_path);
    return result == EOK ? 0 : fail_with_errno(result);
}

typedef struct {
    uint32_t inode_number;
    bool is_directory;
} path_info_t;

static int path_info_locked(const char *path, path_info_t *info)
{
    struct ext4_inode inode;
    struct ext4_sblock *superblock;
    int result;

    result = ext4_raw_inode_fill(path, &info->inode_number, &inode);
    if (result != EOK) {
        return result;
    }
    result = ext4_get_sblock(path, &superblock);
    if (result != EOK) {
        return result;
    }
    info->is_directory =
        ext4_inode_type(superblock, &inode) == EXT4_INODE_MODE_DIRECTORY;
    return EOK;
}

static int directory_is_empty_locked(const char *path, bool *empty)
{
    ext4_dir directory;
    const ext4_direntry *entry;
    int result = ext4_dir_open(&directory, path);

    if (result != EOK) {
        return result;
    }

    *empty = true;
    while ((entry = ext4_dir_entry_next(&directory)) != NULL) {
        bool dot = entry->name_length == 1 && entry->name[0] == '.';
        bool dot_dot = entry->name_length == 2 &&
                       entry->name[0] == '.' && entry->name[1] == '.';
        if (!dot && !dot_dot) {
            *empty = false;
            break;
        }
    }
    return ext4_dir_close(&directory);
}

static bool path_is_strict_descendant(const char *parent, const char *child)
{
    while (*parent != '\0') {
        while (*parent == '/') {
            ++parent;
        }
        while (*child == '/') {
            ++child;
        }
        if (*parent == '\0') {
            break;
        }

        const char *parent_end = strchr(parent, '/');
        const char *child_end = strchr(child, '/');
        size_t parent_length =
            parent_end != NULL ? (size_t)(parent_end - parent)
                               : strlen(parent);
        size_t child_length =
            child_end != NULL ? (size_t)(child_end - child) : strlen(child);

        if (parent_length != child_length ||
            memcmp(parent, child, parent_length) != 0) {
            return false;
        }
        parent += parent_length;
        child += child_length;
    }

    while (*child == '/') {
        ++child;
    }
    return *child != '\0';
}

static int rename_locked(const char *src, const char *dst)
{
    path_info_t source;
    path_info_t destination;
    bool destination_empty;
    int result;

    /*
     * Preserve lwext4's single-transaction fast path when the destination
     * does not exist. EEXIST is the only case requiring emulation.
     */
    result = ext4_frename(src, dst);
    if (result != EEXIST) {
        return result;
    }

    result = path_info_locked(src, &source);
    if (result != EOK) {
        return result;
    }
    result = path_info_locked(dst, &destination);
    if (result != EOK) {
        return result;
    }

    /* POSIX rename is a successful no-op when both names identify one inode. */
    if (source.inode_number == destination.inode_number) {
        return EOK;
    }
    if (source.is_directory != destination.is_directory) {
        return source.is_directory ? ENOTDIR : EISDIR;
    }

    if (source.is_directory) {
        /*
         * Removing the destination before discovering a source/destination
         * ancestry error would lose data. Dot components are rejected because
         * the public lwext4 API does not expose canonical path resolution.
         */
        if (path_has_dot_component(src) || path_has_dot_component(dst) ||
            path_is_strict_descendant(src, dst)) {
            return EINVAL;
        }
        result = directory_is_empty_locked(dst, &destination_empty);
        if (result != EOK) {
            return result;
        }
        if (!destination_empty) {
            return ENOTEMPTY;
        }
        result = ext4_dir_rm(dst);
    } else {
        result = ext4_fremove(dst);
    }
    if (result != EOK) {
        return result;
    }

    /*
     * lwext4 has no public atomic replace operation. If this second rename
     * fails, the old destination has already been removed.
     */
    return ext4_frename(src, dst);
}

static int vfs_lwext4_link(void *opaque, const char *path,
                           const char *hardlink_path)
{
    esp_lwext4_t *ctx = opaque;
    if (ctx->read_only) {
        return fail_with_errno(EROFS);
    }
    return two_path_operation(ctx, path, hardlink_path, ext4_flink);
}

static int vfs_lwext4_unlink(void *opaque, const char *path)
{
    esp_lwext4_t *ctx = opaque;
    char *full_path;
    int result;

    if (ctx->read_only) {
        return fail_with_errno(EROFS);
    }
    full_path = make_path(ctx, path);
    if (full_path == NULL) {
        return -1;
    }
    if (!take_lock(ctx->lock)) {
        free(full_path);
        return -1;
    }
    result = ext4_inode_exist(full_path, EXT4_DE_DIR);
    if (result == EOK) {
        result = EISDIR;
    } else {
        result = ext4_fremove(full_path);
    }
    give_lock(ctx->lock);
    free(full_path);
    return result == EOK ? 0 : fail_with_errno(result);
}

static int vfs_lwext4_rename(void *opaque, const char *src, const char *dst)
{
    esp_lwext4_t *ctx = opaque;
    if (ctx->read_only) {
        return fail_with_errno(EROFS);
    }
    if (path_is_mount_root(src) || path_is_mount_root(dst)) {
        return fail_with_errno(EBUSY);
    }
    return two_path_operation(ctx, src, dst, rename_locked);
}

static DIR *vfs_lwext4_opendir(void *opaque, const char *name)
{
    esp_lwext4_t *ctx = opaque;
    esp_lwext4_dir_t *dir;
    char *full_path;
    int result;

    full_path = make_path(ctx, name);
    if (full_path == NULL) {
        return NULL;
    }
    dir = calloc(1, sizeof(*dir));
    if (dir == NULL) {
        free(full_path);
        errno = ENOMEM;
        return NULL;
    }
    if (!take_lock(ctx->lock)) {
        free(dir);
        free(full_path);
        return NULL;
    }
    result = ext4_dir_open(&dir->ext4, full_path);
    if (result == EOK) {
        dir->ctx = ctx;
        ++ctx->open_dirs;
    }
    give_lock(ctx->lock);
    free(full_path);

    if (result != EOK) {
        free(dir);
        fail_with_errno(result);
        return NULL;
    }
    return &dir->dir;
}

static unsigned char dirent_type(uint8_t inode_type)
{
    switch (inode_type) {
    case EXT4_DE_REG_FILE:
        return DT_REG;
    case EXT4_DE_DIR:
        return DT_DIR;
    case EXT4_DE_CHRDEV:
        return DT_CHR;
    case EXT4_DE_BLKDEV:
        return DT_BLK;
    case EXT4_DE_FIFO:
        return DT_FIFO;
    case EXT4_DE_SOCK:
        return DT_SOCK;
    case EXT4_DE_SYMLINK:
        return DT_LNK;
    default:
        return DT_UNKNOWN;
    }
}

static int vfs_lwext4_readdir_r(void *opaque, DIR *pdir,
                                struct dirent *entry,
                                struct dirent **out_dirent)
{
    esp_lwext4_t *ctx = opaque;
    esp_lwext4_dir_t *dir = (esp_lwext4_dir_t *)pdir;
    const ext4_direntry *source;

    if (pdir == NULL || entry == NULL || out_dirent == NULL ||
        dir->ctx != ctx) {
        return EINVAL;
    }
    if (!take_lock(ctx->lock)) {
        return errno;
    }
    source = ext4_dir_entry_next(&dir->ext4);
    if (source == NULL) {
        *out_dirent = NULL;
        give_lock(ctx->lock);
        return 0;
    }

    memset(entry, 0, sizeof(*entry));
    entry->d_ino = source->inode;
    entry->d_type = dirent_type(source->inode_type);
    memcpy(entry->d_name, source->name, source->name_length);
    entry->d_name[source->name_length] = '\0';
    ++dir->offset;
    *out_dirent = entry;
    give_lock(ctx->lock);
    return 0;
}

static struct dirent *vfs_lwext4_readdir(void *ctx, DIR *pdir)
{
    esp_lwext4_dir_t *dir = (esp_lwext4_dir_t *)pdir;
    struct dirent *result = NULL;
    int error = vfs_lwext4_readdir_r(ctx, pdir, &dir->entry, &result);

    if (error != 0) {
        errno = error;
        return NULL;
    }
    return result;
}

static long vfs_lwext4_telldir(void *opaque, DIR *pdir)
{
    esp_lwext4_t *ctx = opaque;
    esp_lwext4_dir_t *dir = (esp_lwext4_dir_t *)pdir;
    long offset;

    if (pdir == NULL || dir->ctx != ctx) {
        errno = EINVAL;
        return -1;
    }
    if (!take_lock(ctx->lock)) {
        return -1;
    }
    offset = dir->offset;
    give_lock(ctx->lock);
    return offset;
}

static void vfs_lwext4_seekdir(void *opaque, DIR *pdir, long offset)
{
    esp_lwext4_t *ctx = opaque;
    esp_lwext4_dir_t *dir = (esp_lwext4_dir_t *)pdir;

    if (pdir == NULL || dir->ctx != ctx || offset < 0) {
        errno = EINVAL;
        return;
    }
    if (!take_lock(ctx->lock)) {
        return;
    }

    if (offset < dir->offset) {
        ext4_dir_entry_rewind(&dir->ext4);
        dir->offset = 0;
    }
    while (dir->offset < offset) {
        if (ext4_dir_entry_next(&dir->ext4) == NULL) {
            break;
        }
        ++dir->offset;
    }
    give_lock(ctx->lock);
}

static int vfs_lwext4_closedir(void *opaque, DIR *pdir)
{
    esp_lwext4_t *ctx = opaque;
    esp_lwext4_dir_t *dir = (esp_lwext4_dir_t *)pdir;
    int result;

    if (pdir == NULL || dir->ctx != ctx) {
        return fail_with_errno(EINVAL);
    }
    if (!take_lock(ctx->lock)) {
        return -1;
    }
    result = ext4_dir_close(&dir->ext4);
    if (result == EOK) {
        --ctx->open_dirs;
        dir->ctx = NULL;
    }
    give_lock(ctx->lock);
    if (result != EOK) {
        return fail_with_errno(result);
    }
    free(dir);
    return 0;
}

static int vfs_lwext4_mkdir(void *opaque, const char *name, mode_t mode)
{
    esp_lwext4_t *ctx = opaque;
    char *full_path;
    int result;

    if (ctx->read_only) {
        return fail_with_errno(EROFS);
    }
    full_path = make_path(ctx, name);
    if (full_path == NULL) {
        return -1;
    }
    if (!take_lock(ctx->lock)) {
        free(full_path);
        return -1;
    }
    result = ext4_inode_exist(full_path, EXT4_DE_UNKNOWN);
    if (result == EOK) {
        result = EEXIST;
    } else if (result == ENOENT) {
        result = ext4_dir_mk(full_path);
    }
    if (result == EOK) {
        result = ext4_mode_set(full_path, mode);
    }
    give_lock(ctx->lock);
    free(full_path);
    return result == EOK ? 0 : fail_with_errno(result);
}

static int vfs_lwext4_rmdir(void *opaque, const char *name)
{
    esp_lwext4_t *ctx = opaque;
    char *full_path;
    bool empty;
    int result;

    if (ctx->read_only) {
        return fail_with_errno(EROFS);
    }
    if (path_is_mount_root(name)) {
        return fail_with_errno(EBUSY);
    }
    full_path = make_path(ctx, name);
    if (full_path == NULL) {
        return -1;
    }
    if (!take_lock(ctx->lock)) {
        free(full_path);
        return -1;
    }

    result = directory_is_empty_locked(full_path, &empty);
    if (result == EOK) {
        result = empty ? ext4_dir_rm(full_path) : ENOTEMPTY;
    }

    give_lock(ctx->lock);
    free(full_path);
    return result == EOK ? 0 : fail_with_errno(result);
}

static int vfs_lwext4_access(void *opaque, const char *path, int amode)
{
    esp_lwext4_t *ctx = opaque;
    struct stat st;
    int result;

    if ((amode & ~(R_OK | W_OK | X_OK)) != 0) {
        return fail_with_errno(EINVAL);
    }
    if ((amode & W_OK) != 0 && ctx->read_only) {
        return fail_with_errno(EROFS);
    }
    result = vfs_lwext4_stat(ctx, path, &st);
    if (result != 0 || amode == F_OK) {
        return result;
    }

    if (((amode & R_OK) != 0 &&
         (st.st_mode & (S_IRUSR | S_IRGRP | S_IROTH)) == 0) ||
        ((amode & W_OK) != 0 &&
         (st.st_mode & (S_IWUSR | S_IWGRP | S_IWOTH)) == 0) ||
        ((amode & X_OK) != 0 &&
         (st.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) == 0)) {
        return fail_with_errno(EACCES);
    }
    return 0;
}

static int vfs_lwext4_ftruncate(void *opaque, int fd, off_t length)
{
    esp_lwext4_t *ctx = opaque;
    esp_lwext4_file_t *file;
    int result;

    if (length < 0) {
        return fail_with_errno(EINVAL);
    }
    if (ctx->read_only) {
        return fail_with_errno(EROFS);
    }
    if (!take_lock(ctx->lock)) {
        return -1;
    }
    file = file_get(ctx, fd);
    if (file == NULL) {
        give_lock(ctx->lock);
        return -1;
    }
    if ((file->flags & O_ACCMODE) == O_RDONLY) {
        give_lock(ctx->lock);
        return fail_with_errno(EBADF);
    }
    result = resize_file_locked(file, (uint64_t)length);
    give_lock(ctx->lock);
    return result == EOK ? 0 : fail_with_errno(result);
}

static int vfs_lwext4_truncate(void *opaque, const char *path, off_t length)
{
    esp_lwext4_t *ctx = opaque;
    esp_lwext4_file_t file;
    char *full_path;
    int result;

    if (length < 0) {
        return fail_with_errno(EINVAL);
    }
    if (ctx->read_only) {
        return fail_with_errno(EROFS);
    }
    full_path = make_path(ctx, path);
    if (full_path == NULL) {
        return -1;
    }
    if (!take_lock(ctx->lock)) {
        free(full_path);
        return -1;
    }
    memset(&file, 0, sizeof(file));
    file.path = full_path;
    file.flags = O_RDWR;
    file.used = true;
    result = ext4_fopen2(&file.file, full_path, O_RDWR);
    if (result == EOK) {
        result = resize_file_locked(&file, (uint64_t)length);
        ext4_fclose(&file.file);
    }
    give_lock(ctx->lock);
    free(full_path);
    return result == EOK ? 0 : fail_with_errno(result);
}

static int vfs_lwext4_utime(void *opaque, const char *path,
                            const struct utimbuf *times)
{
    esp_lwext4_t *ctx = opaque;
    struct utimbuf now;
    char *full_path;
    int result;

    if (ctx->read_only) {
        return fail_with_errno(EROFS);
    }
    if (times == NULL) {
        time_t current = time(NULL);
        now.actime = current;
        now.modtime = current;
        times = &now;
    }
    if (times->actime < 0 || times->modtime < 0 ||
        (uint64_t)times->actime > UINT32_MAX ||
        (uint64_t)times->modtime > UINT32_MAX) {
        return fail_with_errno(EOVERFLOW);
    }

    full_path = make_path(ctx, path);
    if (full_path == NULL) {
        return -1;
    }
    if (!take_lock(ctx->lock)) {
        free(full_path);
        return -1;
    }
    result = ext4_atime_set(full_path, (uint32_t)times->actime);
    if (result == EOK) {
        result = ext4_mtime_set(full_path, (uint32_t)times->modtime);
    }
    give_lock(ctx->lock);
    free(full_path);
    return result == EOK ? 0 : fail_with_errno(result);
}

#endif /* CONFIG_VFS_SUPPORT_DIR */

static void context_free(esp_lwext4_t *ctx)
{
    if (ctx == NULL) {
        return;
    }
    if (ctx->lock != NULL) {
        vSemaphoreDelete(ctx->lock);
    }
    free(ctx->files);
    free(ctx->mount_point);
    free(ctx->base_path);
    free(ctx);
}

static esp_err_t lwext4_error_to_esp(int error)
{
    switch (error) {
    case EOK:
        return ESP_OK;
    case EINVAL:
    case ENOTDIR:
    case EISDIR:
        return ESP_ERR_INVALID_ARG;
    case ENOENT:
        return ESP_ERR_NOT_FOUND;
    case ENOMEM:
        return ESP_ERR_NO_MEM;
    case EROFS:
        return ESP_ERR_INVALID_STATE;
    case ENOTSUP:
        return ESP_ERR_NOT_SUPPORTED;
    default:
        return ESP_FAIL;
    }
}

static bool path_has_dot_component(const char *path)
{
    const char *component = path;

    while (*component != '\0') {
        while (*component == '/') {
            ++component;
        }
        const char *end = component;
        while (*end != '\0' && *end != '/') {
            ++end;
        }
        size_t length = (size_t)(end - component);
        if ((length == 1 && component[0] == '.') ||
            (length == 2 && component[0] == '.' && component[1] == '.')) {
            return true;
        }
        component = end;
    }
    return false;
}

static bool path_is_mount_root(const char *relative_path)
{
    const char *component = relative_path;
    size_t depth = 0;

    while (*component != '\0') {
        while (*component == '/') {
            ++component;
        }
        const char *end = component;
        while (*end != '\0' && *end != '/') {
            ++end;
        }

        size_t length = (size_t)(end - component);
        if (length == 0 || (length == 1 && component[0] == '.')) {
            /* No change in depth. */
        } else if (length == 2 && component[0] == '.' &&
                   component[1] == '.') {
            if (depth != 0) {
                --depth;
            }
        } else {
            ++depth;
        }
        component = end;
    }
    return depth == 0;
}

esp_err_t esp_vfs_lwext4_register(const esp_vfs_lwext4_conf_t *conf)
{
    struct ext4_mount_stats stats;
    esp_lwext4_t *ctx;
    SemaphoreHandle_t contexts_lock;
    int flags = ESP_VFS_FLAG_STATIC | ESP_VFS_FLAG_CONTEXT_PTR;
    esp_err_t error;

    if (conf == NULL || conf->base_path == NULL ||
        conf->mount_point == NULL || conf->max_files == 0 ||
        conf->max_files > INT_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t base_path_length = strlen(conf->base_path);
    size_t mount_point_length = strlen(conf->mount_point);
    if (base_path_length < 2 || base_path_length > ESP_VFS_PATH_MAX ||
        conf->base_path[0] != '/' ||
        conf->base_path[base_path_length - 1] == '/' ||
        mount_point_length == 0 ||
        mount_point_length > CONFIG_EXT4_MAX_MP_NAME ||
        conf->mount_point[0] != '/' ||
        (conf->mount_point[mount_point_length - 1] != '/' &&
         mount_point_length == CONFIG_EXT4_MAX_MP_NAME)) {
        return ESP_ERR_INVALID_ARG;
    }

    ctx = calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        return ESP_ERR_NO_MEM;
    }
    ctx->base_path = strdup(conf->base_path);
    ctx->mount_point = normalize_mount_point(conf->mount_point);
    ctx->files = calloc(conf->max_files, sizeof(*ctx->files));
    ctx->lock = xSemaphoreCreateMutex();
    ctx->max_files = conf->max_files;
    ctx->read_only = conf->read_only;
    if (ctx->base_path == NULL || ctx->mount_point == NULL ||
        ctx->files == NULL || ctx->lock == NULL) {
        context_free(ctx);
        return ESP_ERR_NO_MEM;
    }

    if (ext4_mount_point_stats(ctx->mount_point, &stats) != EOK) {
        context_free(ctx);
        return ESP_ERR_INVALID_STATE;
    }

    contexts_lock = contexts_lock_get();
    if (contexts_lock == NULL) {
        context_free(ctx);
        return ESP_ERR_NO_MEM;
    }
    if (!take_lock(contexts_lock)) {
        context_free(ctx);
        return ESP_FAIL;
    }
    for (esp_lwext4_t *item = s_contexts; item != NULL; item = item->next) {
        if (strcmp(item->base_path, ctx->base_path) == 0 ||
            strcmp(item->mount_point, ctx->mount_point) == 0) {
            give_lock(contexts_lock);
            context_free(ctx);
            return ESP_ERR_INVALID_STATE;
        }
    }

    if (ctx->read_only) {
        flags |= ESP_VFS_FLAG_READONLY_FS;
    }
    error = esp_vfs_register_fs(ctx->base_path, &s_vfs_lwext4_ops, flags, ctx);
    if (error == ESP_OK) {
        ctx->next = s_contexts;
        s_contexts = ctx;
        ESP_LOGI(TAG, "registered lwext4 mount %s at %s",
                 ctx->mount_point, ctx->base_path);
    }
    give_lock(contexts_lock);

    if (error != ESP_OK) {
        context_free(ctx);
    }
    return error;
}

esp_err_t esp_vfs_lwext4_unregister(const char *base_path)
{
    SemaphoreHandle_t contexts_lock;
    esp_lwext4_t **link;
    esp_lwext4_t *ctx;
    esp_err_t error;

    if (base_path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    contexts_lock = contexts_lock_get();
    if (contexts_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (!take_lock(contexts_lock)) {
        return ESP_FAIL;
    }

    for (link = &s_contexts; *link != NULL; link = &(*link)->next) {
        if (strcmp((*link)->base_path, base_path) == 0) {
            break;
        }
    }
    ctx = *link;
    if (ctx == NULL) {
        give_lock(contexts_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (!take_lock(ctx->lock)) {
        give_lock(contexts_lock);
        return ESP_FAIL;
    }
    if (ctx->open_files != 0 || ctx->open_dirs != 0) {
        give_lock(ctx->lock);
        give_lock(contexts_lock);
        return ESP_ERR_INVALID_STATE;
    }

    error = esp_vfs_unregister_fs(base_path);
    if (error == ESP_OK) {
        *link = ctx->next;
    }
    give_lock(ctx->lock);
    give_lock(contexts_lock);

    if (error == ESP_OK) {
        ESP_LOGI(TAG, "unregistered lwext4 VFS at %s", base_path);
        context_free(ctx);
    }
    return error;
}

esp_err_t esp_vfs_lwext4_rmdir_recurse(const char *path)
{
    SemaphoreHandle_t contexts_lock;
    esp_lwext4_t *ctx = NULL;
    const char *relative_path = NULL;
    size_t matched_prefix_length = 0;
    char *lwext4_path;
    int result;

    if (path == NULL || path[0] != '/') {
        return ESP_ERR_INVALID_ARG;
    }

    contexts_lock = contexts_lock_get();
    if (contexts_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (!take_lock(contexts_lock)) {
        return ESP_FAIL;
    }

    for (esp_lwext4_t *item = s_contexts; item != NULL; item = item->next) {
        size_t prefix_length = strlen(item->base_path);
        if (strncmp(path, item->base_path, prefix_length) == 0 &&
            (path[prefix_length] == '\0' || path[prefix_length] == '/') &&
            prefix_length > matched_prefix_length) {
            ctx = item;
            relative_path = path + prefix_length;
            matched_prefix_length = prefix_length;
        }
    }
    if (ctx == NULL) {
        give_lock(contexts_lock);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!take_lock(ctx->lock)) {
        give_lock(contexts_lock);
        return ESP_FAIL;
    }
    give_lock(contexts_lock);

    if (path_is_mount_root(relative_path) ||
        path_has_dot_component(relative_path)) {
        give_lock(ctx->lock);
        return ESP_ERR_INVALID_ARG;
    }
    if (ctx->read_only || ctx->open_files != 0 || ctx->open_dirs != 0) {
        give_lock(ctx->lock);
        return ESP_ERR_INVALID_STATE;
    }

    lwext4_path = make_path(ctx, relative_path);
    if (lwext4_path == NULL) {
        esp_err_t error = errno == ENOMEM ? ESP_ERR_NO_MEM
                                          : ESP_ERR_INVALID_ARG;
        give_lock(ctx->lock);
        return error;
    }

    result = ext4_inode_exist(lwext4_path, EXT4_DE_DIR);
    if (result == ENOENT) {
        int any_type_result =
            ext4_inode_exist(lwext4_path, EXT4_DE_UNKNOWN);
        if (any_type_result == EOK) {
            result = ENOTDIR;
        } else {
            result = any_type_result;
        }
    }
    if (result == EOK) {
        result = ext4_dir_rm(lwext4_path);
    }

    free(lwext4_path);
    give_lock(ctx->lock);
    return lwext4_error_to_esp(result);
}
