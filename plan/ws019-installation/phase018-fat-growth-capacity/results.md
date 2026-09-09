# q133 FAT growth capacity — completed

2026-09-09. The q132 failure is fixed: truncate checks capacity under the
existing FAT mutation lock before allocating or zeroing a growing file.
Full-chain validation now optionally returns owned cluster count, including
preallocation beyond EOF; other callers keep their existing wrapper.
Capacity is read from the actual FAT, not FSInfo or an unlocked df estimate.
Insufficient capacity returns ENOSPC and read/corrupt-chain errors return EIO
before this growth mutates storage. Existing I/O-failure rollback remains.
No timeout limit was raised. Existing time now borrows a second correctly when
formatting a negative nanosecond difference.

## Verification

- `temp/q133-growth-host-final`: actual consolidated FAT12/16/32; 4800 checks
  each, ordinary and ASan/UBSan. Empty/existing files, full/no space, exact fit,
  partial cluster, preallocation, impossible volume size, malformed old size,
  cyclic chain, read error, uncertain write completion and barrier failures.
  Refusal preserves bytes/size/chain; recoverable I/O errors restore allocation
  ownership and old file contents.
- `temp/q133-staging`: PASS staging. Actual target Noct argv/status/timeout/output
  bound, exclusive creation and conflict, fresh 32MiB UFS data and 64MiB swap,
  flush/no-replace publication, both 256MiB volume-size and 192MiB free-space
  refusals with status 1 and unchanged zero-length staging object. Actual
  swapon validates 16383 slots, swapoff and unmount pass.
- `temp/q133-staging-reboot`: PASS staging reboot. Uses a copy of the exact
  generated images, boots overlay plus swap, writes a deterministic marker,
  reboots and checks the marker and active swap again.
- Both runtime cells preserve production inputs, partition tables, FAT boot
  sector and unmanaged sentinel hashes. The reboot cell also verifies its
  original staging medium is unchanged.
- Explicit make -j16 builds pass for amd64, PCAT and PC98:
  `/tmp/zedbsd-q133-amd64.log`, `-pcat.log`, `-pc98.log`; amd64 staging fixture
  build `/tmp/zedbsd-q133-fixture.log`. User config.mk was preserved.

Fresh data growth took 30.050s, its formatter 0.470s; swap growth 62.900s,
its formatter 0.060s. Entire staging scenario including both refusals took
99.112s. This proves bounded functional staging in this QEMU setup, not high
throughput or physical-device performance.

The reboot still prints USB class shutdown EBUSY and completes. This is
recorded under WS002-p023, not claimed repaired by this FAT change.

p017's command-staging acceptance is now complete. Source/destination selection,
transaction-wide ownership, interrupted installer recovery and idempotence
remain p004/p005 requirements; no installer acceptance is claimed here.
