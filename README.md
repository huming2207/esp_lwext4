# esp_lwext4 - WIP

ESP-IDF component build wrapper for the vendored lwext4 submodule.

This README, the CMake build scripts, the test code and the `port/lwext4_xattr_stub.c` 
are vibe-coded by ChatGPT Codex. The rest of the porting code will be written by human (me).

The build keeps the vendored GPL extent and xattr implementation files behind
an explicit source-selection boundary. The default port supplies an xattr stub
and an experimental extent implementation based on the MIT-licensed
[ext4-rs project by yuoo655](https://github.com/yuoo655/ext4_rs).

## Project state

- [x] Basic functionalities
- [x] Tested on real device (see https://github.com/huming2207/esp_lwext4_demo)
- [x] Pass e2fsck and dumpe2fs
- [x] Experimental extent implementation based on [ext4-rs](https://github.com/yuoo655/ext4_rs)
- [x] Real-card depth-2 extent creation, remount, data verification and e2fsck
- [ ] Accidental power-failure test

## Why need this 

For a few reasons:

1. LittleFS is very slow on SD card, see: https://github.com/espressif/esp-idf/tree/v6.0.2/examples/storage/perf_benchmark
2. FatFS doesn't have native journaling, may corrupt upon accidental power failure
3. LittleFS cannot be recognised on Linux hosts (unless using FUSE or separate userspace dump tools).
4. Ext3/4 FS itself, and `lwext4` library are both tested on embedded devices

## Benchmark result:

On a ESP32-P4 Rev 1.0 (Alientek DNESP32P4M) board + Sandisk Extreme U3/A1 32GB MicroSD card, using 4-bit access, PSRAM enabled:

```
I (1544) main_task: Calling app_main()
Name: SE32G
Type: SDHC
Speed: 40.00 MHz (limit: 40.00 MHz)
Size: 30436MB
CSD: ver=2, sector_size=512, capacity=62333952 read_bl_len=9
SSR: bus_width=4
I (6964) esp_lwext4: registered lwext4 mount /ext/ at /sd
I (6964) demo_storage: Mounted ext filesystem at /sd (lwext4 mount /ext/)
I (8364) esp_lwext4: unregistered lwext4 VFS at /sd
I (8424) esp_lwext4: registered lwext4 mount /ext/ at /sd
I (8424) demo_storage: Unmount/remount cycle completed
I (9534) demo_tests: Sequential: write 10.21 MiB/s, durable write 10.21 MiB/s, read 13.71 MiB/s, verify 15.33 MiB/s (8192 KiB)
I (9814) esp_lwext4: unregistered lwext4 VFS at /sd
I (9824) esp_lwext4: registered lwext4 mount /ext/ at /sd
I (9824) demo_storage: Unmount/remount cycle completed
I (10064) demo_tests: Random 4 KiB: durable write 918.6 IOPS, verified read 1079.1 IOPS
I (16724) esp_lwext4: unregistered lwext4 VFS at /sd
I (17054) esp_lwext4: registered lwext4 mount /ext/ at /sd
I (17054) demo_storage: Unmount/remount cycle completed
I (24234) esp_lwext4: unregistered lwext4 VFS at /sd
I (24574) esp_lwext4: registered lwext4 mount /ext/ at /sd
I (24574) demo_storage: Unmount/remount cycle completed
I (31114) esp_lwext4: unregistered lwext4 VFS at /sd
I (31464) esp_lwext4: registered lwext4 mount /ext/ at /sd
I (31464) demo_storage: Unmount/remount cycle completed
I (39114) esp_lwext4: unregistered lwext4 VFS at /sd
I (39484) esp_lwext4: registered lwext4 mount /ext/ at /sd
I (39484) demo_storage: Unmount/remount cycle completed
I (39574) demo_tests: Stress: 2000 operations passed in 29.50 s (seed=0x051a7e55)
I (39574) lwext4_demo: All enabled tests passed; filesystem remains mounted at /sd
I (39574) main_task: Returned from app_main()
```

## Licence

The ESP-IDF port code in this repository is licensed under the
[BSD 3-Clause License](LICENSE).
Third-party notices used by the out-of-tree port are collected in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

The `lwext4/` Git submodule is third-party software and retains its upstream
copyright and licensing. The normal ext4-rs-based configuration excludes the
two GPL-licensed implementation files from the firmware build; the explicitly
selectable upstream extent configuration does not, as explained below.

The component provides the build and configuration layer, an `esp_blockdev`
adapter, and an ESP-IDF VFS adapter. The design and storage-safety background
for the block-device adapter is in
[`docs/esp-blockdev-porting.md`](docs/esp-blockdev-porting.md).

## Supported baseline

This build was verified with:

- ESP-IDF v6.0.2, target `esp32p4`;
- lwext4 commit `58bcf89a121b72d4fb66334f1693d3b30e4cb9c5`;
- CMake/Ninja through `idf.py`; and
- the direct SDMMC adapter with journaled ext3 and experimental ext4
  configurations described below.

`esp_blockdev` is present in this ESP-IDF baseline. An older ESP-IDF tree that
does not contain the `esp_blockdev` component is not supported by the current
component metadata.

## Formatter group rounding

`CONFIG_LWEXT4_FORMAT_ROUND_TO_FULL_GROUPS` (enabled by default) rounds the
formatted filesystem length down to a whole number of block groups before
calling `ext4_mkfs()`. The pinned lwext4 formatter writes an incorrect
free-block count in the final partial block group's descriptor, so `e2fsck`
reports a mismatch on devices that do not end on a group boundary. Rounding
keeps the on-disk metadata consistent at the cost of up to one block group of
capacity (128 MiB for 4096-byte blocks). Disable the option to keep the full
device size, but the resulting metadata is inconsistent: lwext4 runs, while
Linux filesystem checks report a free-block count mismatch. Disabling is
intended for diagnostics only, not for production or interoperability use.

## Experimental extent implementation

`CONFIG_LWEXT4_EXTENTS_MIT` supplies the three extent hooks expected by the
unchanged lwext4 core. Writable extent allocation, splitting, and removal are
accepted only while an lwext4 journal transaction is active; a filesystem
without an active journal receives `ENOTSUP` rather than an unsafe
best-effort mutation. Reading initialized extents is supported. Unwritten
extents read as holes, but writing/converting them is not implemented.

The common 256 MiB benchmark file was observed as four inline extents, so it
does not exercise an external leaf. A separate host regression test forced
about 920 non-mergeable extents per file, reached a depth-2 tree, verified
data, truncated across leaf boundaries, truncated to zero and rewrote, then
unlinked a depth-2 file. ASan/UBSan reported no error and `e2fsck -f -n`
passed after unmount.

The ESP32-P4 demo subsequently created two real-card files with 920 one-block
extents each. Both survived sync and remount with exact data, and their logical
ranges covered blocks 0 through 919 without gaps or overlaps. Each tree used
one intermediate index block and five leaf blocks containing 170, 170, 170,
170, and 240 extents. All data and extent-tree metadata blocks were distinct,
the filesystem remained clean, and `e2fsck -f -n` reported the depth histogram
`12/1/2` without an integrity error.

e2fsck does report that these two level-2 trees "could be narrower". The
current half-split policy reaches depth 2 before those leaves are densely
packed; 920 extents could instead fit in three 340-entry leaves at depth 1.
This is a tree-density and lookup-performance limitation, not evidence of
corruption. These results are useful coverage, not a production or power-loss
qualification. See [CAVEATS.md](doc/CAVEATS.md) for the remaining limits.

## Add the component to a project

Place this repository under the application's `components/` directory:

```text
my_app/
├── CMakeLists.txt
├── components/
│   └── esp_lwext4/
│       ├── CMakeLists.txt
│       ├── Kconfig
│       └── lwext4/
└── main/
```

If it remains elsewhere, add its absolute directory to
`EXTRA_COMPONENT_DIRS` before including ESP-IDF's `project.cmake`.

The consuming component declares:

```cmake
idf_component_register(
    SRCS "my_storage.c"
    REQUIRES esp_lwext4
)
```

It can then include lwext4 headers normally:

```c
#include "ext4.h"
#include "ext4_mkfs.h"
```

Configure the component with:

```sh
idf.py menuconfig
```

Open **lwext4 configuration**. The defaults select the journaled ext3 on-disk
feature set, keep xattrs disabled, select the experimental extent
implementation for projects that opt into ext4, allocate from internal SRAM
through ESP-IDF's heap capability API, and use conservative aligned-access
code.

Build with:

```sh
idf.py build
```

## Create an lwext4 block device directly from SDMMC

For an SD card, particularly on an ESP-IDF v6.0 release without the fix for
[esp-idf#18875](https://github.com/espressif/esp-idf/issues/18875), prefer the
direct adapter. It maps lwext4 physical block numbers one-to-one to SD sector
numbers and calls `sdmmc_read_sectors()` and `sdmmc_write_sectors()` without
forming a potentially overflowing byte address.

Initialize the SDMMC host, slot, and `sdmmc_card_t` first, then create and
register the adapter:

```c
#include "lwext4_port_sdmmc.h"

lwext4_port_sdmmc_t *adapter = NULL;
const lwext4_port_sdmmc_config_t sdmmc_config = {
    .physical_block_size = card.csd.sector_size,
    .buffer_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT,
    .buffer_alignment = verified_dma_alignment,
    .transfer_buffer_blocks = 16,
};

ESP_ERROR_CHECK(lwext4_port_sdmmc_create(&card, &sdmmc_config, &adapter));

int rc = ext4_device_register(lwext4_port_sdmmc_get(adapter), "storage");
if (rc != EOK) {
    ESP_ERROR_CHECK(lwext4_port_sdmmc_destroy(adapter));
    /* Deinitialize the caller-owned SDMMC card and host here. */
}
```

The adapter borrows the initialized card and requires exclusive access to it.
It owns only its recursive lock and aligned internal/DMA transfer buffer;
lwext4 cache buffers may still reside in PSRAM because all SD transfers bounce
through that buffer. Contiguous requests are split into chunks of at most
`transfer_buffer_blocks` sectors.

On shutdown, close all files, unregister VFS, stop the journal, unmount, and
unregister the lwext4 device before destroying the adapter. Deinitialize the
caller-owned SDMMC card/host last. `lwext4_port_sdmmc_sync()` is intentionally
a no-op apart from serialization because ESP-IDF's SDMMC sector writes return
synchronously; still call `ext4_cache_flush()` first to flush lwext4's own
cache.

## Create an lwext4 block device from generic BDL

> [!WARNING]
> ESP-IDF v6.0.2's SDMMC handle returned by `sdmmc_get_blockdev()` truncates
> 64-bit byte addresses before converting them to sectors on 32-bit targets.
> Cards larger than 4 GiB can therefore wrap I/O onto earlier sectors and be
> silently corrupted. `CONFIG_LWEXT4_BLOCK_DEV_CACHE_SIZE=64` may mask the
> observed failure by changing writeback order, but it does not make this path
> safe. Espressif tracks the defect as
> [esp-idf#18875](https://github.com/espressif/esp-idf/issues/18875); it is
> fixed upstream but, at the time of writing, has not been backported to the
> ESP-IDF v6.0 release line. Patch or bypass the affected SDMMC BDL and reformat
> media that was written through it. See
> [`doc/CAVEATS.md`](doc/CAVEATS.md) for the root cause, evidence, and safe
> alternatives.

`lwext4_port_bdl_create()` wraps a caller-owned `esp_blockdev` handle. The
caller explicitly supplies the physical block size, transfer-buffer heap
capabilities, read-only mode, durability policy, and confirmation that the
selected BDL stack supports ordinary overwrites:

```c
#include "lwext4_port_bdl.h"

lwext4_port_bdl_t *adapter = NULL;
const lwext4_port_bdl_config_t bdl_config = {
    .physical_block_size = media->geometry.read_size,
    .buffer_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT,
    .buffer_alignment = verified_dma_alignment,
    .transfer_buffer_blocks = 16,
    .read_only = false,
    .sync_after_write = true,
    .lower_device_supports_rewrite = true,
};

ESP_ERROR_CHECK(lwext4_port_bdl_create(media, &bdl_config, &adapter));

int rc = ext4_device_register(lwext4_port_bdl_get(adapter), "storage");
if (rc != EOK) {
    ESP_ERROR_CHECK(lwext4_port_bdl_destroy(adapter));
    /* Release the caller-owned media handle here. */
}
```

The transfer buffer is also lwext4's physical-block scratch buffer. It is
allocated with the requested capabilities and alignment. Every lower read and
write is bounced through it, so lwext4 caches may use PSRAM while a lower
driver uses a DMA/internal-memory buffer. Contiguous transfers are issued in
chunks of up to `transfer_buffer_blocks`; set it to zero for single-block
compatibility behavior.

For writable use, `lower_device_supports_rewrite` must be true. Do not set it
for raw erase-before-write flash. Put a proven rewrite-capable translation
layer, such as wear levelling, below this adapter first. The adapter cannot
infer this solely from BDL flags because stacked devices may preserve the
physical lower device's flags.

The adapter borrows `media`; it never calls the lower handle's `release`
operation. Shutdown order is:

```c
ESP_ERROR_CHECK(esp_vfs_lwext4_unregister("/data"));
ESP_ERROR_CHECK(ext4_umount("/ext/"));
ESP_ERROR_CHECK(ext4_device_unregister("storage"));
ESP_ERROR_CHECK(lwext4_port_bdl_destroy(adapter));
ESP_ERROR_CHECK(media->ops->release(media));
```

Call `ext4_cache_flush()` before `lwext4_port_bdl_sync()` for an explicit
durability checkpoint. `sync_after_write` is conservative and can be expensive;
disable it only when the selected lower device's completed writes already
provide the ordering and durability required by lwext4's journal.

## Register an lwext4 mount with VFS

Mount lwext4 first, then register the same lwext4 mount point at an ESP-IDF VFS
prefix:

```c
#include "esp_lwext4.h"
#include "ext4.h"

ESP_ERROR_CHECK(ext4_mount("storage", "/ext/", false));

const esp_vfs_lwext4_conf_t vfs_conf = {
    .base_path = "/data",
    .mount_point = "/ext",
    .max_files = 8,
    .read_only = false,
};
ESP_ERROR_CHECK(esp_vfs_lwext4_register(&vfs_conf));
```

Standard calls such as `open("/data/file.txt", ...)`, `fopen()`, `stat()`, and
`opendir()` then operate on the lwext4 mount. During shutdown, close all VFS
files and directories before unregistering and unmounting:

```c
ESP_ERROR_CHECK(esp_vfs_lwext4_unregister("/data"));
ESP_ERROR_CHECK(ext4_umount("/ext/"));
```

The VFS adapter does not own the lwext4 mount or its block device. Its
`read_only` setting must match the mode passed to `ext4_mount()`.

### Rename replacement limitation

`rename()` replaces an existing file destination or an existing empty
directory destination, including the common log-rotation pattern. The pinned
lwext4 public API has no atomic replace operation, however, so replacement is
implemented as destination removal followed by the native rename. A failure or
power loss between those operations can leave the destination absent. Renaming
to a non-empty directory fails with `ENOTEMPTY`, and file/directory type
mismatches are rejected.

### Recursive directory removal

POSIX `rmdir()` remains non-recursive. For explicitly recursive removal on a
registered lwext4 VFS path, close all VFS handles on that mount and call:

```c
ESP_ERROR_CHECK(esp_vfs_lwext4_rmdir_recurse("/data/cache"));
```

The helper rejects paths outside a registered lwext4 VFS, the mount root,
read-only mounts, and paths containing `.` or `..` components.

## How the external build works

The component follows ESP-IDF's
[ExternalProject fully overridden build pattern](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-guides/build-system.html#fully-overriding-the-component-build-process):

1. `idf_component_register()` builds only the ESP-IDF-facing VFS adapter.
2. `ExternalProject_Add()` configures the wrapper in `cmake/lwext4/` with the
   same ESP-IDF toolchain.
3. The wrapper generates `generated/ext4_config.h` from Kconfig values.
4. It compiles a deliberately explicit source list into `liblwext4.a`.
5. ESP-IDF imports that archive and propagates its public include directories.

`BUILD_ALWAYS TRUE` causes the nested incremental build to run on each top-level
build. Nested Ninja still recompiles only changed inputs.

The source list must remain explicit. Do not replace it with a glob or
`aux_source_directory()`.

## Licence and feature boundary

The upstream submodule is not modified. Its GPL-licensed `ext4_xattr.c` is not
compiled, archived, or linked.

Extents support is selectable in menuconfig under **Extents implementation**:

- **Disabled** keeps the build free of extent code and limits the feature set
  to ext2/ext3.
- **MIT port** (default) compiles `port/lwext4_extent.c`, an MIT-licensed
  extent-tree implementation based on
  [ext4-rs by yuoo655](https://github.com/yuoo655/ext4_rs). The archive does
  not contain lwext4's GPL extent object.
- **Upstream lwext4** compiles the vendored GPL-2.0-or-later
  `ext4_extent.c`; the resulting binary must comply with GPL-2.0-or-later.

The ext4 on-disk feature set is only offered when an extents implementation is
selected. `CONFIG_LWEXT4_XATTR_ENABLE` remains a hidden, fixed-off symbol.
The external build fails if the ext4 feature set is selected without an
extents implementation.

The out-of-tree `port/lwext4_xattr_stub.c` resolves symbols that `ext4.c`
references even when xattrs are disabled. It implements no xattr storage.

This is a technical build boundary, not legal advice.

## Kconfig mapping

ESP-IDF component symbols are namespaced as `CONFIG_LWEXT4_*`. The generated
header maps them to the unmodified upstream macro names:

| ESP-IDF Kconfig symbol | Generated lwext4 macro |
| --- | --- |
| feature-set choice | `CONFIG_EXT_FEATURE_SET_LVL` |
| `CONFIG_LWEXT4_JOURNALING_ENABLE` | `CONFIG_JOURNALING_ENABLE` |
| `CONFIG_LWEXT4_XATTR_ENABLE` | `CONFIG_XATTR_ENABLE` |
| `CONFIG_LWEXT4_EXTENTS_ENABLE` | `CONFIG_EXTENTS_ENABLE` |
| `CONFIG_LWEXT4_HAVE_OWN_ERRNO` | `CONFIG_HAVE_OWN_ERRNO` |
| `CONFIG_LWEXT4_DEBUG_PRINTF` | `CONFIG_DEBUG_PRINTF` |
| `CONFIG_LWEXT4_DEBUG_ASSERT` | `CONFIG_DEBUG_ASSERT` |
| `CONFIG_LWEXT4_HAVE_OWN_ASSERT` | `CONFIG_HAVE_OWN_ASSERT` |
| `CONFIG_LWEXT4_BLOCK_DEV_ENABLE_STATS` | `CONFIG_BLOCK_DEV_ENABLE_STATS` |
| `CONFIG_LWEXT4_BLOCK_DEV_CACHE_SIZE` | `CONFIG_BLOCK_DEV_CACHE_SIZE` |
| `CONFIG_LWEXT4_EXT4_MAX_BLOCKDEV_NAME` | `CONFIG_EXT4_MAX_BLOCKDEV_NAME` |
| `CONFIG_LWEXT4_EXT4_BLOCKDEVS_COUNT` | `CONFIG_EXT4_BLOCKDEVS_COUNT` |
| `CONFIG_LWEXT4_EXT4_MAX_MP_NAME` | `CONFIG_EXT4_MAX_MP_NAME` |
| `CONFIG_LWEXT4_EXT4_MOUNTPOINTS_COUNT` | `CONFIG_EXT4_MOUNTPOINTS_COUNT` |
| `CONFIG_LWEXT4_HAVE_OWN_OFLAGS` | `CONFIG_HAVE_OWN_OFLAGS` |
| `CONFIG_LWEXT4_MAX_TRUNCATE_SIZE` | `CONFIG_MAX_TRUNCATE_SIZE` |
| `CONFIG_LWEXT4_UNALIGNED_ACCESS` | `CONFIG_UNALIGNED_ACCESS` |
| `CONFIG_LWEXT4_USE_INTERNAL_SRAM` | `CONFIG_LWEXT4_USE_INTERNAL_SRAM` |
| `CONFIG_LWEXT4_USE_PSRAM` | `CONFIG_LWEXT4_USE_PSRAM` |
| `CONFIG_LWEXT4_USE_PREFER_PSRAM` | `CONFIG_LWEXT4_USE_PREFER_PSRAM` |
| `CONFIG_LWEXT4_BIG_ENDIAN` | `CONFIG_BIG_ENDIAN` when enabled |

The allocator choice always sets `CONFIG_USE_USER_MALLOC=1`; the out-of-tree
`port/lwext4_port_esp_heap.c` implementation supplies the four `ext4_user_*`
hooks. Internal SRAM uses `heap_caps_*()` with
`MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT`, PSRAM uses the corresponding
`MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT` capabilities, and the preferred mode
tries PSRAM before internal SRAM.

`CONFIG_USE_DEFAULT_CFG` is fixed to zero by the external build because the
generated configuration is mandatory.

These macros are computed by upstream `ext4_config.h`, so they are intentionally
not independent Kconfig settings:

- `CONFIG_SUPPORTED_FCOM`;
- `CONFIG_SUPPORTED_FINCOM`;
- `CONFIG_SUPPORTED_FRO_COM`;
- `CONFIG_DIR_INDEX_ENABLE`;
- `CONFIG_EXTENT_ENABLE`; and
- `CONFIG_META_CSUM_ENABLE`.

Making them independent would permit internally inconsistent feature masks.

## Build smoke test

The repository includes an ESP-IDF link test:

```sh
cd test_apps/build_smoke
idf.py set-target esp32
idf.py build
```

It references a function in `ext4.c`, so the final firmware link checks the
external archive and the disabled-xattr stubs rather than merely proving that a
static archive can be created.
