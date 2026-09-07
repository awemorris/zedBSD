# Unified UFS acceptance fixtures

The [acceptance matrix](acceptance.md) defines the final WS gates. A passed
focused fixture does not claim all matrix rows.

- `run-driver-host.py OUT`: production unified metadata, allocation/truncate,
  run, view and journal/snapshot consistency in ordinary + sanitizer variants.
- `run-width-host.py OUT`: real freestanding LP64 and ILP32 mapping and size
  boundary executables, independent of missing host i386 libc headers.
- `ufs-super-host.c`: both-endian canonical and malformed geometry decoder gate.
- `run-formatter-images.py OUT`: target/C/Python producer agreement, ordinary
  and explicit persistence profiles, dynamic group count and sparse maximum.
- `run-formatter-fault-host.sh`: maintained formatter failure, corruption,
  prefilled backing, bounded I/O and Noct agreement gates.
- `features.mk` / `run-features-qemu.py OUT`: public syscall namespace, extattr,
  user/group quota, mounted snapshot, remount and reboot acceptance. Build the
  normal amd64 image and `ws024-features-fixture` sequentially first. Runtime
  copies the protected source image and creates a disposable secondary medium.

OUT must be a fresh directory below this WS's `temp/`, except the WS025 native
baseline which retains its owner and writes below WS025 `temp/`. Never run tests
against external user media. Historical completed Queue results stay unchanged.

- `platform-images.py OUT`: ARM64/RPi4/SPARC packaging with explicit synthetic inputs; no boot claim.
- `retirement-inventory.py OUT.json`: active source/path and three supported kernel symbol audit.
- `run-features-qemu.py OUT LEGACY_IMAGE [reject]`: preserved legacy UFS2 read-only mount, or UFS1 rejection, with whole-media hashes.
