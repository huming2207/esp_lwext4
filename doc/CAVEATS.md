# Caveats

## ESP-IDF v6.0.2 SDMMC BDL corrupts media larger than 4 GiB

### Scope

ESP-IDF v6.0.2's `sdmmc_get_blockdev()` implementation is unsafe for SD media
larger than 4 GiB on 32-bit ESP targets. Its block-device operations accept a
64-bit byte address, but the helper in
`components/sdmmc/sdmmc_blockdev.c` narrows that address to `size_t` before
dividing it by the sector size:

```c
size_t start_sector_num = (size_t) addr / sector_size;
size_t last_byte_addr = (size_t) (addr + data_len - 1);
```

`size_t` is 32 bits on ESP32-P4. Consequently, byte addresses at or above
4 GiB wrap modulo 4 GiB before they are converted to SD sectors. The operation
still targets a valid sector and normally returns success, so neither the SD
driver nor heap poisoning reports an error.

Espressif tracks this defect as
[esp-idf#18875 (IDFGH-18017)](https://github.com/espressif/esp-idf/issues/18875).
The issue is marked closed and done, and the defect has been fixed upstream.
At the time of writing, the fix has not been backported to the ESP-IDF v6.0
release line; the v6.0.2 source used for this investigation still contains the
unsafe casts. Projects based on v6.0 must inspect their exact ESP-IDF revision
rather than assuming that closure of the upstream issue includes a release
branch backport.

This is not a narrowing bug in `lwext4_port_bdl.c`. That adapter validates its
range and calculates the lower byte address in `uint64_t`. The truncation
occurs afterward, inside the lower ESP-IDF SDMMC `esp_blockdev` returned by
`sdmmc_get_blockdev()`. Other lower BDL implementations must be audited
separately and should not be assumed to have the same defect.

Reads are affected as well as writes. Read-only use avoids modifying the card,
but data above the wrap boundary is still read from the wrong location.

### Observed failure

The failure was reproduced with:

- ESP-IDF v6.0.2 on ESP32-P4;
- a 30,436 MiB SDHC card with 512-byte sectors;
- an ext3 filesystem with 4,096-byte blocks;
- 32,768 filesystem blocks and 288 inodes per block group; and
- `CONFIG_LWEXT4_BLOCK_DEV_CACHE_SIZE=256` during formatting.

The sequential verification failed at file byte offset `119468032`. That is
file logical block:

```text
119468032 / 4096 = 29167
```

`debugfs bmap` mapped file logical block 29167 to filesystem physical block
98308. The filesystem layout reported block 98308 as block group 3's block
bitmap:

```text
group 3 start:         3 * 32768       = 98304
group 3 block bitmap:  98304 + 4       = 98308
```

The 4 GiB byte-address wrap corresponds to 1,048,576 filesystem blocks, or
exactly 32 block groups:

```text
4 GiB / 4096 bytes = 1048576 filesystem blocks
1048576 / 32768     = 32 block groups
```

Block group 227's inode bitmap is logical filesystem block 7,438,340. After
the ESP-IDF address truncation, it aliases group 3's block bitmap exactly:

```text
group 227 inode bitmap:  227 * 32768 + 4       = 7438340
wrapped block:           7438340 mod 1048576   = 98308
group 3 block bitmap:    3 * 32768 + 4         = 98308
```

An inode bitmap for 288 inodes begins with 288 clear allocation bits and has
the remaining padding bits set. When that block overwrites group 3's block
bitmap, group 3 appears to have exactly its first 288 blocks free. The
benchmark file's allocation map confirmed that it received physical blocks
98304 through 98591 inclusive: exactly 288 blocks, including group 3's backup
superblock, descriptor table, allocation bitmaps, and inode table.

Once the file allocator marked those blocks used, the block bitmap became all
`0xff`. Reading the file block mapped onto block 98308 therefore returned the
bitmap rather than the written test pattern, producing the deterministic
mismatch at byte 119468032.

An offline `e2fsck -fn` also found invalid inode contents beginning at inode
289 and extensive metadata corruption. Values consisting of repeated `0xce`
bytes matched ESP-IDF comprehensive heap poisoning's allocated-memory fill
pattern. Heap poisoning did not report a boundary violation because the root
failure is valid I/O sent to the wrong sector, not an out-of-bounds heap
access.

The demo reports this verification failure as `ESP_ERR_INVALID_CRC`. In this
case that value is an application-level data-mismatch result; it is not an
SDMMC wire CRC error and does not indicate signal-integrity trouble.

### Why a block cache size of 64 can appear to work

`CONFIG_LWEXT4_BLOCK_DEV_CACHE_SIZE` changes which logical metadata buffers
remain cached, when dirty buffers are evicted, and the order in which they are
written. lwext4 correctly treats group 3's block bitmap and group 227's inode
bitmap as distinct logical blocks. The faulty lower SDMMC BDL collapses them
onto the same physical sector only when it converts their byte addresses.

With a 64-entry cache, more frequent eviction produces a different sequence
of colliding writes. A later write may happen to leave the low block-group
metadata needed by the current benchmark in a usable state. A 256-entry cache
retains a different set of buffers until later writeback, exposing the group 3
collision described above. Formatting statistics also differ because the two
cache sizes produce different hit and eviction patterns.

This is masking, not correctness. It is impossible for a cache setting to
preserve both logical blocks when the lower layer maps them to the same
physical sector. A benchmark that passes with 64 entries does not prove that
other low-group metadata is intact, and any access intended for data above
4 GiB still wraps. Do not use cache size 64 as a workaround.

Formatting in a separate boot does not help. The formatter writes metadata
throughout the full device, so it persists the aliased metadata before the
filesystem is first mounted. Changing the cache size or disabling formatting
on the next boot cannot repair the on-disk damage.

### Preferred fix in ESP-IDF

Keep the address in `uint64_t` until after division by the sector size. Only
the sector number and sector count should be narrowed, after proving that they
fit the types accepted by `sdmmc_read_sectors()`, `sdmmc_write_sectors()`, and
`sdmmc_erase_sectors()`.

When possible, update to an ESP-IDF revision containing the resolution for
[esp-idf#18875](https://github.com/espressif/esp-idf/issues/18875). ESP-IDF v6.0
users must currently carry an equivalent local patch or bypass the affected
SDMMC BDL until Espressif backports that fix. Confirm the actual source: a
version label or closed issue alone is not sufficient.

A corrected helper has this shape:

```c
static esp_err_t calculate_start_sector_num_and_sector_count(
    size_t sector_size,
    uint64_t addr,
    size_t data_len,
    size_t *out_start_sector_num,
    size_t *out_num_of_sectors)
{
    if (sector_size == 0 || addr % sector_size != 0 ||
        data_len % sector_size != 0) {
        return ESP_ERR_INVALID_ARG;
    }

    uint64_t first = addr / sector_size;
    uint64_t count = data_len / sector_size;

    if (first > SIZE_MAX || count > SIZE_MAX ||
        count > SIZE_MAX - (size_t)first) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (out_start_sector_num != NULL) {
        *out_start_sector_num = (size_t)first;
    }
    if (out_num_of_sectors != NULL) {
        *out_num_of_sectors = (size_t)count;
    }
    return ESP_OK;
}
```

The final implementation should also retain the card-capacity checks in the
SDMMC sector functions and test zero-length requests according to the BDL
contract. A 30 GiB card has only about 62 million 512-byte sectors, so its
sector indices fit easily in a 32-bit `size_t` even though its byte addresses
do not.

After fixing ESP-IDF, rebuild all affected components and reformat the card.
Do not attempt to repair or continue using a filesystem formatted or written
through the affected path; its allocation metadata cannot be trusted.

### Full-capacity alternative: direct lwext4-to-SDMMC backend

An application can bypass ESP-IDF's byte-addressed SDMMC `esp_blockdev` and
implement lwext4's `ext4_blockdev_iface` directly over
`sdmmc_read_sectors()` and `sdmmc_write_sectors()`. With lwext4's physical
block size set to the card's 512-byte sector size, the callbacks receive
sector numbers already and do not need to represent a greater-than-4-GiB byte
address in `size_t`.

Such a backend must still:

- keep capacity and range calculations in `uint64_t`;
- prove that the sector index and count fit `size_t` before narrowing;
- check the request against `card->csd.capacity` without overflowing;
- use an internal, DMA-capable, correctly aligned transfer buffer;
- bounce data between that buffer and lwext4 cache buffers when the latter may
  reside in PSRAM;
- split large requests into supported chunks;
- serialize requests; and
- preserve synchronous-write and shutdown semantics.

This approach can use the entire card and does not require changes to the
vendored `lwext4/` source. It does, however, couple the port directly to
ESP-IDF's SDMMC API rather than the generic BDL interface.

### Conservative containment

If neither the ESP-IDF fix nor a direct sector backend is available, fail
closed for devices larger than 4 GiB on affected 32-bit targets. A deliberately
limited view can be safe only when every translated lower byte address,
including the partition start offset, remains below the 4 GiB boundary.
Partition wrappers must be audited too: using 32-bit `size_t` or `ssize_t` for
partition offsets can introduce additional limits.

This containment sacrifices most of a large card and should be considered a
temporary measure. Silently accepting the full device with a smaller lwext4
cache is not safe.

### Validation after a fix

Use a freshly reformatted card and validate both the filesystem and the data
path:

1. Run format-only mode so no benchmark data can obscure formatter defects.
2. Power-cycle or remove the card after formatting.
3. Run `e2fsck -fn` on a host before mounting it on ESP.
4. Run a sequential test large enough to cross multiple block groups and
   verify every byte after an unmount/remount cycle.
5. Exercise data beyond the 4 GiB byte offset, not merely a large card whose
   test files remain near the beginning.
6. Remove the card and run `e2fsck -fn` again.
7. Repeat with different lwext4 cache sizes; all sizes must produce the same
   correct on-disk result.

The vendored lwext4 tree does not need modification for this issue.

## Experimental extent-tree port

### Scope and successful-path coverage

`port/lwext4_extent.c` is an out-of-tree implementation of lwext4's three
extent hooks. It supports initialized-extent lookup, allocation and merging,
root growth, non-root leaf/index splitting, suffix and finite-range removal,
metadata checksums, and bounded on-disk metadata validation. The vendored
lwext4 source is not modified.

The following successful-path checks passed on 2026-08-02:

- an ESP-IDF v6.0.2 `esp32p4` firmware build with ext4, journaling, the
  ext4-rs-based port, and a 256-entry block cache;
- the user's 30 GiB card completed format, sequential/random/stress tests and
  passed a host `e2fsck -f -n`;
- the 256 MiB `benchmark.bin` contained four extents, all represented by the
  inode's inline extent root;
- a host image deliberately interleaved two files to produce about 920 extents
  per file and a depth-2 tree, then verified data, truncated one file across
  leaf boundaries, truncated it to zero, rewrote it, and removed the other
  depth-2 file; and
- that host run passed ASan/UBSan and the unmounted image passed
  `e2fsck -f -n`.

These checks cover the previously failing full-root, external-leaf and
depth-2 transitions. They do not qualify the implementation for arbitrary
power loss, injected I/O failure, hostile-image fuzzing, or all Linux-created
extent layouts.

### Journaling is mandatory for mutation

The port refuses allocation, tree splitting and extent removal unless both
journaling support is compiled in and the mounted filesystem has an active
lwext4 journal transaction. It returns `ENOTSUP` otherwise. A filesystem with
the extents feature but no journal is therefore suitable only for extent reads
through this port.

This fail-closed rule is required because a tree split changes several
metadata blocks and allocation bitmaps. It is not enough merely to select
`CONFIG_LWEXT4_JOURNALING_ENABLE`: the on-disk filesystem must contain a
journal, journal recovery must run when required, and `ext4_journal_start()`
must succeed for the mount.

### Remaining format and API limits

- The port does not create or convert unwritten extents. Reads treat them as
  holes; a create/write lookup into an unwritten extent returns `ENOTSUP`.
- Normal lookup validates header bounds, node depth, checksum when present,
  the selected entry, its adjacent ordering boundaries, and physical block
  bounds. It intentionally does not linearly rescan every entry of every
  external node on each data-block lookup. A full node is scanned before a
  split redistributes all entries.
- A four-extent `filefrag` result exercises only the 60-byte inline root. At
  least five non-mergeable extents are needed to force the first external
  leaf; hundreds are needed to exercise a full 4 KiB leaf and depth-2 growth.
- The tree splitter does not rebalance under-filled siblings. `e2fsck` may
  offer an optional tree optimization after extreme synthetic fragmentation;
  that is not an on-disk consistency error.

### Lower-I/O failure caveat in the pinned lwext4 core

The pinned `lwext4` revision's `ext4_fwrite()` assigns the return value of
`ext4_fs_put_inode_ref()` to `r` at its `Finish` label. This can overwrite an
earlier data/allocation error before the function chooses between transaction
abort and commit. The extent port requires a transaction and keeps its normal
split ordering valid under a successful commit, but it cannot repair this
caller-level error propagation without changing the vendored core.

Consequently, passing successful-path and `e2fsck` tests is not evidence that
injected SD timeouts or cache/writeback failures are safely rolled back. Before
production use, either move to an lwext4 revision with verified error
propagation, carry a separately reviewed upstream fix outside the vendored
tree, or perform explicit fault-injection testing that proves the chosen
revision aborts every failed metadata operation.

### Performance impact

For a contiguous or four-extent file, the new validation and journal guard are
constant, small CPU work and the split machinery is never entered. External
node lookup remains a binary search plus validation of the selected entry and
its neighbours; it does not scan roughly 340 entries per level. Full-node
validation and copying occur only when fragmentation fills a node and forces a
split. The dominant cost for ordinary 4 KiB random writes remains lwext4's
transaction-per-write behavior and SD latency.

Repeat the target benchmark after changing the allocator, checksum feature,
PSRAM placement, or block size. The host test's elapsed time is useful for
regression detection but is not comparable to ESP32 SDMMC throughput.
