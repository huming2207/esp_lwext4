# Porting lwext4 to `esp_blockdev`

This document records the verified interfaces, safety constraints, and design
behind `port/lwext4_port_bdl.c`. Deployment choices that depend on the selected
storage device remain explicit configuration inputs.

## 1. Freeze the API baseline

The interfaces below were checked against the local ESP-IDF v6.0.2 tree on
2026-07-31:

- `components/esp_blockdev/include/esp_blockdev.h`;
- `components/sdmmc/sdmmc_blockdev.c`;
- `components/esp_partition/partition.c`; and
- `components/wear_levelling/wl_blockdev.cpp`.

The public documentation is:

- [ESP-IDF Block Device Layer](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/storage/blockdev.html)
- [ESP-IDF Wear Levelling API](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/storage/wear-levelling.html)

Before writing the adapter against another ESP-IDF revision, verify these exact
members still exist:

```c
esp_blockdev_t::geometry
esp_blockdev_t::device_flags
esp_blockdev_t::ops

esp_blockdev_ops_t::read
esp_blockdev_ops_t::write
esp_blockdev_ops_t::sync
esp_blockdev_ops_t::release
```

Do not start this port on an IDF revision that lacks `esp_blockdev`; either move
the application to an IDF revision that has it or define a separate
compatibility project. The current component requires `esp_blockdev`.

## 2. Choose the lower device explicitly

There are two verified acquisition paths in the current IDF tree.

### SD or eMMC

After the application has initialized an `sdmmc_card_t`, acquire:

```c
esp_blockdev_handle_t media = ESP_BLOCKDEV_HANDLE_INVALID;
ESP_ERROR_CHECK(sdmmc_get_blockdev(card, &media));
```

The current SD BDL reports its card sector size for read, write, and erase
granularity. Its `sync()` is documented in the implementation as a no-op
because its writes are synchronous. The `sdmmc_card_t` must remain alive until
after the BDL handle is released.

### ESP flash partition with wear levelling

Do not connect lwext4 directly to a raw `esp_partition` BDL. lwext4 performs
ordinary block overwrites and does not issue erase-before-write operations.
Upstream also states that lwext4 is unsuitable for raw flash.

The current IDF exposes this stack:

```c
const esp_partition_t *partition =
    esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                             ESP_PARTITION_SUBTYPE_ANY,
                             "storage");
assert(partition != NULL);

esp_blockdev_handle_t partition_bdl = ESP_BLOCKDEV_HANDLE_INVALID;
esp_blockdev_handle_t wl_bdl = ESP_BLOCKDEV_HANDLE_INVALID;

ESP_ERROR_CHECK(
    esp_partition_ptr_get_blockdev(partition, &partition_bdl));
ESP_ERROR_CHECK(wl_get_blockdev(partition_bdl, &wl_bdl));
```

Confirmed limitations of the current `wl_get_blockdev()` implementation:

- it rejects a lower `disk_size` greater than `UINT32_MAX`;
- it derives usable size and erase size from the WL engine;
- it keeps, but does not own, the lower BDL handle; and
- releasing the WL BDL flushes and destroys WL but does not release the lower
  partition BDL.

Release in top-to-bottom order:

```c
ESP_ERROR_CHECK(wl_bdl->ops->release(wl_bdl));
wl_bdl = ESP_BLOCKDEV_HANDLE_INVALID;

ESP_ERROR_CHECK(partition_bdl->ops->release(partition_bdl));
partition_bdl = ESP_BLOCKDEV_HANDLE_INVALID;
```

The current WL BDL copies the lower device flags unchanged. Therefore
`erase_before_write` alone cannot be used to decide whether a WL handle offers
filesystem-style rewrite semantics. Treat “this handle is the output of
`wl_get_blockdev()`” as explicit application knowledge and test overwrite
behavior before mounting lwext4.

## 3. Out-of-tree adapter API

The adapter remains outside the upstream `lwext4/` tree:

```text
port/
├── lwext4_port_bdl.h
├── lwext4_port_bdl.c
└── lwext4_xattr_stub.c
```

This is project API, not an existing Espressif API:

```c
typedef struct lwext4_port_bdl lwext4_port_bdl_t;

typedef struct {
    uint32_t physical_block_size;
    uint32_t buffer_caps;
    uint32_t buffer_alignment;
    uint32_t transfer_buffer_blocks;
    bool read_only;
    bool sync_after_write;
    bool lower_device_supports_rewrite;
} lwext4_port_bdl_config_t;

esp_err_t lwext4_port_bdl_create(
    esp_blockdev_handle_t lower,
    const lwext4_port_bdl_config_t *config,
    lwext4_port_bdl_t **out_adapter);

struct ext4_blockdev *lwext4_port_bdl_get(
    lwext4_port_bdl_t *adapter);

esp_err_t lwext4_port_bdl_sync(
    lwext4_port_bdl_t *adapter);

esp_err_t lwext4_port_bdl_destroy(
    lwext4_port_bdl_t *adapter);
```

The ownership rule is:

- the adapter borrows `lower`;
- creating the adapter never consumes or releases it;
- destroying the adapter flushes and frees only adapter-owned state; and
- the application releases the lower BDL separately.

This matches the current IDF BDL stack style and avoids an unmount operation
unexpectedly destroying a caller-owned SD, WL, or partition handle.

## 4. Validate the lower BDL before allocating

`lwext4_port_bdl_create()` rejects the device unless every applicable
condition is proven:

1. `lower`, `config`, and `out_adapter` are non-null.
2. `lower->ops` and `lower->ops->read` are non-null.
3. For a writable mount, `lower->ops->write` is non-null and
   `device_flags.read_only` is false.
4. `geometry.disk_size` and `geometry.read_size` are nonzero.
5. For a writable mount, `geometry.write_size` is nonzero.
6. `physical_block_size` is explicitly supplied; do not guess it.
7. `physical_block_size` is a power of two and fits in `uint32_t`.
8. It is a multiple of both `read_size` and, for writable devices,
   `write_size`.
9. `disk_size` is a multiple of `physical_block_size`.
10. The device has been explicitly classified as supporting rewrites. Reject
    raw erase-before-write flash.
11. `buffer_caps` explicitly includes `MALLOC_CAP_8BIT` and any lower-driver
    requirements such as DMA-capable internal memory.
12. `buffer_alignment` is a power of two, divides `physical_block_size`, and
    satisfies the selected lower driver and target.
13. `physical_block_size * transfer_buffer_blocks` fits in `size_t`; zero
    transfer blocks selects a one-block compatibility buffer.
14. If durability requires `sync`, `lower->ops->sync` is non-null.

For SD/eMMC, 512 bytes will commonly be the correct physical block size, but
the adapter must use the geometry reported by the actual card and the explicit
configuration rather than hard-coding that value.

The power-of-two requirement comes from the pinned lwext4 implementation:
partial-block access uses a mask based on `ph_bsize - 1`.

## 5. Allocate the lwext4-facing object

The adapter needs, at minimum:

```c
struct lwext4_port_bdl {
    struct ext4_blockdev ext4;
    struct ext4_blockdev_iface iface;
    esp_blockdev_handle_t lower;
    uint8_t *transfer_buffer;
    /* A FreeRTOS recursive mutex. */
    esp_err_t last_lower_error;
    bool read_only;
    bool sync_after_write;
};
```

Initialize the pinned lwext4 fields as follows:

```c
adapter->iface.open = adapter_open;
adapter->iface.bread = adapter_bread;
adapter->iface.bwrite = adapter_bwrite;
adapter->iface.close = adapter_close;
adapter->iface.lock = adapter_lock;
adapter->iface.unlock = adapter_unlock;
adapter->iface.ph_bsize = config->physical_block_size;
adapter->iface.ph_bcnt =
    lower->geometry.disk_size / config->physical_block_size;
adapter->iface.ph_bbuf = adapter->transfer_buffer;
adapter->iface.p_user = adapter;

adapter->ext4.bdif = &adapter->iface;
adapter->ext4.part_offset = 0;
adapter->ext4.part_size = lower->geometry.disk_size;
```

Allocate `ph_bbuf` with `physical_block_size * transfer_buffer_blocks` bytes
and the caller-supplied `buffer_caps` and `buffer_alignment`. The
implementation uses this as a bounce buffer for every lower read and write.
This prevents lwext4 cache placement, including PSRAM, from violating
lower-driver DMA, alignment, or internal-memory requirements. A zero transfer
block count selects one block for compatibility.

## 6. Implement read and write translation

lwext4 calls:

```c
bread(bdev, buffer, block_id, block_count);
bwrite(bdev, buffer, block_id, block_count);
```

The BDL uses byte addresses and lengths. Translate only after overflow and
bounds checks:

```c
byte_address = block_id * physical_block_size;
byte_length = block_count * physical_block_size;
```

Split a request into chunks no larger than `transfer_buffer_blocks`. Each read
uses:

```c
esp_err_t err = lower->ops->read(lower,
                                 transfer_buffer,
                                 transfer_buffer_size,
                                 byte_address,
                                 chunk_size);
```

It then copies that chunk into the lwext4 destination. Each write first copies
one lwext4 source chunk into `transfer_buffer`, then uses:

```c
esp_err_t err = lower->ops->write(lower,
                                  transfer_buffer,
                                  byte_address,
                                  chunk_size);
```

Before multiplying, prove:

- `block_id <= disk_size / physical_block_size`;
- `block_count` does not carry the range past the device end; and
- `byte_length <= SIZE_MAX`.

Do not truncate the BDL's 64-bit address to 32 bits in the adapter. The pinned
lwext4 interface already accepts a 64-bit block ID and the BDL accepts a
64-bit byte address.

## 7. Define error mapping

lwext4 returns errno-style positive integers while BDL returns `esp_err_t`.
Choose and test a mapping. A suitable initial policy is:

| BDL result | lwext4 result |
| --- | --- |
| `ESP_OK` | `EOK` |
| `ESP_ERR_INVALID_ARG`, `ESP_ERR_INVALID_SIZE` | `EINVAL` |
| `ESP_ERR_NO_MEM` | `ENOMEM` |
| `ESP_ERR_NOT_SUPPORTED` | `ENOTSUP` |
| `ESP_ERR_NOT_FOUND` | `ENOENT` |
| write attempted on read-only media | `EROFS` |
| all other lower I/O failures | `EIO` |

Save the original `esp_err_t` in `last_lower_error` before mapping it. This
preserves diagnostics that would otherwise be lost behind `EIO`.

This table is a port design choice, not a rule imposed by ESP-IDF.

## 8. Implement open, close, locking, and sync

`open()` should validate that the borrowed handle is still present. It should
not reacquire or take ownership of the BDL.

`close()` should:

1. call the lower `sync()` when available;
2. return the mapped result; and
3. leave the lower BDL allocated.

`lock()` and `unlock()` should use one adapter-owned recursive mutex. lwext4's
physical-interface reference counter and shared scratch buffer make concurrent
access unsafe without serialization.

There is no `sync` callback in the pinned lwext4 block-device interface. The
wrapper-level `lwext4_port_bdl_sync()` calls the lower BDL's `sync()`.
Application durability checkpoints should perform:

```c
int rc = ext4_cache_flush(mount_point);
if (rc == EOK) {
    ESP_ERROR_CHECK(lwext4_port_bdl_sync(adapter));
}
```

For shutdown:

```c
ext4_cache_write_back(mount_point, false);
ext4_journal_stop(mount_point);
ext4_umount(mount_point);          /* adapter close() syncs lower */
ext4_device_unregister(device_name);
lwext4_port_bdl_destroy(adapter);
lower->ops->release(lower);        /* caller-owned handle */
```

### Durability point that must not be assumed

The pinned lwext4 interface has no explicit flush/barrier operation at journal
commit boundaries. A cached BDL can therefore weaken journal ordering even
though lwext4's own block cache is in write-through mode.

Use one of these proven policies:

- a lower BDL whose `write()` does not return until the data is durably ordered;
  the current SD BDL states that its writes are synchronous; or
- `sync_after_write=true`, making every successful adapter `bwrite()` call
  immediately invoke the lower `sync()`.

The second policy is conservative and may be slow. Do not relax it for the WL
BDL until power-loss tests prove the required journal ordering.

## 9. Register, mount, recover, and start the journal

With an already-created adapter:

```c
struct ext4_blockdev *bdev = lwext4_port_bdl_get(adapter);

int rc = ext4_device_register(bdev, "storage");
if (rc != EOK) {
    /* unwind adapter and BDL handles */
}

rc = ext4_mount("storage", "/data/", false);
if (rc != EOK) {
    ext4_device_unregister("storage");
    /* unwind */
}

rc = ext4_recover("/data/");
if (rc != EOK) {
    ext4_umount("/data/");
    ext4_device_unregister("storage");
    /* preserve the medium for diagnosis */
}

rc = ext4_journal_start("/data/");
if (rc != EOK) {
    ext4_umount("/data/");
    ext4_device_unregister("storage");
    /* unwind */
}
```

This order is confirmed by lwext4's own test code:

1. `ext4_device_register`;
2. `ext4_mount`;
3. `ext4_recover`; and
4. `ext4_journal_start`.

Do not enable write-back caching until the basic, power-loss-safe path works.
When it is later enabled, disable it before journal stop and unmount.

## 10. Format only with explicit geometry

Formatting is destructive and must be a separate application action, never an
automatic response to an arbitrary mount failure.

For the intended BSD-only format:

```c
struct ext4_fs fs;
struct ext4_mkfs_info info = {
    .len = lower->geometry.disk_size,
    .block_size = 4096,
    .journal = true,
    .label = "storage",
};

int rc = ext4_mkfs(&fs, bdev, &info, F_SET_EXT3);
```

The 4096-byte filesystem block size shown here is an explicit project choice,
not a universal default. Before using it, prove it is a multiple of the
adapter's physical block size and measure its RAM cost. The pinned formatter
defaults zero `block_size` to 4096, but production code should state the choice
explicitly.

After formatting, sync the lower BDL and inspect an image or removable device
on Linux:

```sh
sudo dumpe2fs -h /dev/sdX1
sudo e2fsck -f -n /dev/sdX1
```

Require `has_journal`, require a clean `e2fsck`, and reject the `extent` feature.

## 11. Test in layers

Do not begin with application file operations. Use these gates.

### Adapter I/O tests

1. Read the first and last valid physical block.
2. Reject one-past-end and multiplication-overflow requests.
3. Write, read back, overwrite with different bits, and read back again.
4. Verify alignment rejection for every lower geometry.
5. Inject every relevant `esp_err_t` and verify mapping plus
   `last_lower_error`.
6. Run two tasks against one adapter and verify serialization.

### Filesystem compatibility tests

1. Format as `F_SET_EXT3` with journaling.
2. Mount, recover, and start the journal.
3. Create directories and bounded log files.
4. Flush, stop the journal, unmount, and sync.
5. Run Linux `dumpe2fs` and `e2fsck -f -n`.
6. Confirm the archive and final map contain no GPL objects.

### Power-loss tests

Cut power or inject failure after each lower write and sync boundary. For every
resulting image:

1. preserve the original;
2. run non-modifying `e2fsck`;
3. repair a copy;
4. remount it through the ESP adapter; and
5. verify committed application records.

Repeat for SD/eMMC and WL separately. Passing on one provider does not prove
the other provider's cache, erase, or ordering behavior.

## 12. Deployment decisions still required

These cannot be derived from the component and must be answered for each
storage stack before creating an adapter:

1. Is the first target SD/eMMC or an ESP flash partition through WL?
2. What physical block size should the adapter require for that exact provider?
3. Is `sync_after_write` acceptable for the first correctness build?
4. Does the selected storage driver require DMA-capable buffers?
5. Which filesystem block size and cache size fit the target's memory and
   workload?

The build system does not depend on these choices. The adapter validates the
choices supplied through its configuration, but hardware testing is still
required.
