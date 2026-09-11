# q169 / ws019-p036 results

Completed 2026-09-09. The native codec initializes only the root directory
and reserves inodes 0/1/2; it does not create overlay journals or a marker.
Existing journal/snapshot locators remain outside filesystem allocation.
Native geometry uses 64-bit total fragments and free-space summaries, derives
groups with a bounded search, checks final-group metadata room and respects
off_t and on-disk inode limits. Each transfer is checked against medium bounds.
Native invalidation and publication have explicit flush boundaries.

| Acceptance | Evidence |
| --- | --- |
| Actual native images | 4 MiB and 4 GiB sparse files generated and read back in ordinary and ASan/UBSan/leak modes |
| Independent decoder | Python checks root dot entries only, inode 2 mode/link/size/allocation, all inode/free bitmaps, group and filesystem summaries, backup superblocks and persistence locator fields |
| Wide geometry | Production geometry and superblock generation for 5 TiB retain more than UINT32_MAX fragments; maintained decoder accepts header and counters. This is a synthetic header test, not a full 5-TiB format or native mount |
| Boundary cases | 161 final-group edge geometries retain room after metadata; bad alignment/small/overflow and inode-count overflow refuse |
| Failure paths | All 171 write/flush and 168 read operations of the 4-MiB case fail in turn; short I/O refuses; root, inode, bitmap and locator corruption detected |
| Free data | First unallocated data-sector sentinel retains its preimage; all transfers remain within the declared medium |
| Legacy output | Maintained backend byte equality and 12521 fault checks per mode pass; ordinary overlay and feature profile remain unchanged |
| Pristine regression | 3071 checks in each mode and independent reference hashes unchanged |
| Target builds | amd64, pcat and pc98 disk-image exit 0 |

Logs: `/tmp/zedbsd-q169-native.log`, `/tmp/zedbsd-q169-legacy.log`,
`/tmp/zedbsd-q169-pristine.log`, `/tmp/zedbsd-q169-{amd64,pcat,pc98}.log`.
Reusable tests are native-ufs-codec-test.c, native-ufs-inspect.py and
run-native-ufs-codec-test.sh under this WS's tests directory.

The legacy overlay image profile retains its historical size bound; the new
native profile does not inherit it. Host off_t restrictions still apply on
architectures using narrower file offsets. No full-disk zeroing, secure erase
or crash-atomic replacement is claimed. The public reserved native mkfs
command and QEMU mount/tree-copy acceptance remain the next prerequisite.
Automatic installation and BeUI remain unfinished.
