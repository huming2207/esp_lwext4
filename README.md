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

The component currently provides the build and configuration layer only. It
does not yet provide an `esp_blockdev` adapter or an ESP VFS adapter. The
implementation plan for the former is in
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
xattrs and extents, use ESP-IDF's C library definitions, and use conservative
aligned-access code.

Build with:

```sh
idf.py build
```

## How the external build works

The component follows ESP-IDF's
[ExternalProject fully overridden build pattern](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-guides/build-system.html#fully-overriding-the-component-build-process):

1. `idf_component_register()` creates a configuration-only component.
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
| `CONFIG_LWEXT4_USE_USER_MALLOC` | `CONFIG_USE_USER_MALLOC` |
| `CONFIG_LWEXT4_BIG_ENDIAN` | `CONFIG_BIG_ENDIAN` when enabled |

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
