# q181 result

Status: completed 2026-09-10

Implemented pure native capacity API on the production geometry, public mkfs
--check-size for native UFS/FAT32, and nativeadmit.noct shared preflight.
No media open/read/write/ioctl/fsync occurs in the query; stdout errors fail.
Arithmetic planner remains separate from formatter admission and device leases.

Passed host gates:
- Native codec capacity agrees with actual formatted superblock counts at
  4 MiB/4 GiB and generated 5-TiB geometry; independent CG/bitmap inspection and
  existing fault/corruption tests retained. /tmp/zedbsd-q181-native-codec.log.
- Actual public CLI linked with abort-on-media-I/O wrappers: valid geometries,
  syntax/overflow/alignment/profile/argument refusal and /dev/full output error,
  normal and ASan/UBSan. /tmp/zedbsd-q181-query.log.
- FAT formatter, all four sector sizes, independent inspection/mtools and
  faults: /tmp/zedbsd-q181-fat-codec.log. Existing reserved block formatter
  command regressions: /tmp/zedbsd-q181-block-command.log.
- Noct JIT and interpreter: installer-nativeadmit.noct, 2 accepted and 25
  refused cases, including exact fit, byte/inode shortages and malformed records.
- amd64 build: /tmp/zedbsd-q181-amd64.log.

PCAT /tmp/zedbsd-q181-pcat.log and PC98 /tmp/zedbsd-q181-pc98.log final builds
passed; targeted diff check clean. Full destructive installer transaction, UI,
source-free installed-system acceptance and paging stress are p006/p007;
this component does not itself complete those phases.
