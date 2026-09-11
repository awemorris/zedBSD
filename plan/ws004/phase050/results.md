# WS004-p050 results — completed / cleared q187

## Implemented behavior

The driver no longer rejects a second controller or overwrites the first one's
registry entry. Each allocated controller receives a monotonic boot-local index
under the registry lock. Namespace names use that index (`nvme0n1`, `nvme1n1`,
etc.); boot identity remains PARTUUID. Failed attaches may consume an index.
The linked registry has no new fixed controller-count cap. Actual admission is
bounded by PCI, memory/IRQ/DMA resources and the existing 80 disk descriptors
(shared with other disks and partitions). The existing single-active-namespace
profile per controller is unchanged; this is multiple-controller support.

Namespace discovery claims one eligible controller under registry/command locks,
then searches the registry afresh after completing its probe. No unlocked sibling
pointer survives teardown. Failed probes retain only their existing quarantine
binding and do not stop discovery of later controllers. Detach/shutdown claims
match the exact PCI device under the registry lock and unpublish only that node.
Command queues, DMA, recovery and shutdown remain owned by each controller.

## Evidence

- Current amd64 kernel `8b456ad31492c4800e2c53c32a124217f9a10db639809d276771c8a99240c51b`
  copied into clones of the accepted q184 coexistence installation. mtype/hash
  verifies the installed kernel bytes; no installer source disk is attached.
- `temp/q187-multiple3/result.json`: PASS in both PCI enumeration orders.
  Both namespaces appear, the installed PARTUUID boots/root login/swap succeed,
  raw auxiliary bytes change from nonzero to zero and match SHA-256 after read,
  root writes remain separate, protected auxiliary bytes remain unchanged, and
  both normal halts complete on all four CPUs.
- `temp/q187-followup1/result.json`: PASS for both persisted installations and
  an empty auxiliary controller first. The latter has no namespace; the driver
  logs `namespace probe failed (21)` and `failed probe resources released;
  quarantined`, then publishes/boots `nvme1n1`. Root writes and halt still pass.
- `temp/q187-concurrent1/result.json`: PASS. Two forked guest processes write,
  fsync and compare all bytes of 64 separate 64 KiB patterns on different
  controllers (raw auxiliary at 8 MiB; root overlay file on installed NVMe).
  Different per-process patterns expose cross-controller data mixups. Halt passes.
- `run-nvme-registry-host.sh`: ordinary and ASan/UBSan PASS, current complete
  driver source. Exact-device claim, busy-probe timeout, independent shutdown,
  middle/head removal, request failure, queue-memory reset and quarantine leave
  a sibling controller and its queues untouched. Reset refuses a still-owned BIO.
- `run-current-nvme-lifecycle-host.py`: existing initialization-cleanup, I/O
  lifecycle and terminal-shutdown assertions all PASS, ordinary and ASan/UBSan.
  Tests include the current full driver translation unit; no stale generated
  fragment or alternative implementation is substituted.

Timeout/reset and failed initialization ownership are checked by host models of
current production functions; no physical controller lost-completion or live
hardware-reset experiment is claimed. Actual QEMU tests cover both devices,
concurrent I/O, probe-failure isolation, persistence and normal shutdown.
No production hardware reset algorithm was changed by this registry refactor.

## Trial failures and limits

q187-multiple1/2 failed in the test's checksum command, not in controller boot.
The correct option is `cksum -a sha256`, not `--sha256`; the earlier assumption
about a stale shell builtin was incorrect. The first fixture also treated short
/dev/zero records as full 4096-byte records; using 512-byte records verified the
intended 65536 bytes. Only the corrected multiple3 run is acceptance.

ASan global registration keeps unused MMIO code alive in the host TU, so the
host fixture uses `--param=asan-globals=0` and section garbage collection;
executed ownership logic remains instrumented. LeakSanitizer is disabled in
this ptrace environment; no LeakSanitizer claim is made.

No hot-plug feature or multiple-active-namespace support is added. Physical
controller compatibility remains subject to the driver's existing capability
profile. The former one-controller limit and its boot-order failure are fixed.


Builds: amd64 (`/tmp/zedbsd-q187-amd64-build2.log`), PC/AT
(`/tmp/zedbsd-q187-pcat-build.log`) and PC98 (`/tmp/zedbsd-q187-pc98-build.log`)
all pass. `git diff --check` passes. No commits or aggregate make check.
The user-required multi-controller task is cleared; other WS004 work remains
independent. Queue q187 is finished.
