# esp_lwext4 - WIP

ESP-IDF component build wrapper for the vendored lwext4 submodule.

This README, the CMake build scripts, the test code and the `port/lwext4_xattr_stub.c` 
are vibe-coded by ChatGPT Codex. The rest of the porting code will be written by human (me).

This port also focus on avoid GPL pollution, so that it's friendly for propiretary 
or source-available projects. Thus I let Codex to make a `port/lwext4_xattr_stub.c` 
to avoid linking against `lwext4_xattr` and `lwext4_extent` GPL licensed code. 

## Licence

The ESP-IDF port code in this repository is licensed under the
[BSD 3-Clause License](LICENSE).

The `lwext4/` Git submodule is third-party software and retains its upstream
copyright and licensing. This project deliberately excludes its two
GPL-licensed implementation files from the firmware build, as explained below.

The component provides the build and configuration layer plus an ESP-IDF VFS
adapter for an already-mounted lwext4 filesystem. It does not yet provide an
`esp_blockdev` adapter. The implementation plan for that adapter is in
[`docs/esp-blockdev-porting.md`](docs/esp-blockdev-porting.md).

## Supported baseline

This build was verified with:

- ESP-IDF v6.0.2, target `esp32`;
- lwext4 commit `58bcf89a121b72d4fb66334f1693d3b30e4cb9c5`;
- CMake/Ninja through `idf.py`; and
- the BSD-only ext3 configuration described in the earlier project notes.

`esp_blockdev` is present in this ESP-IDF baseline. An older ESP-IDF tree that
does not contain the `esp_blockdev` component is not supported by the current
component metadata.

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

Open **lwext4 configuration**. The defaults select journaled ext3, disable
xattrs and extents, allocate from internal SRAM through ESP-IDF's heap
capability API, and use conservative aligned-access code.

Build with:

```sh
idf.py build
```

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

### Known rename limitation

The pinned lwext4 public API cannot atomically replace an existing rename
destination. Consequently, `rename()` through this adapter returns `EEXIST`
when the destination already exists. The adapter deliberately does not emulate
replacement by unlinking the destination first, because a subsequent rename
failure or power loss would destroy the original destination.

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

The upstream submodule is not modified. Its GPL-licensed `ext4_extent.c` and
`ext4_xattr.c` are not compiled, archived, or linked.

`CONFIG_LWEXT4_EXTENTS_ENABLE` and `CONFIG_LWEXT4_XATTR_ENABLE` exist as hidden,
fixed-off Kconfig symbols so their upstream inputs are explicit. The external
build also fails if either value is forced on. The ext4 feature-set choice is
not exposed because its supported-feature mask includes extents.

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
