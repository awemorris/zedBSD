# Q084: Refactored kernel integration

Date: 2026-09-06
Phase: [ws018-p016](../phase016-kern-refactor-integration/phase.md)
Status: completed

## Provenance and integrated result

- Refactor input: `/home/awe/claude/zedBSD`, commit `03436b8`, based on `a143d5a`.
- Destination snapshot: `/home/awe/zedBSD`, commit `d99865c`.
- Retained kernel fixes from `d97e21c`, `ac4fb7c`, and `d99865c`.
- Both inputs and the ancestor/recent patch are retained in the ignored
  `plan/ws018-kernel-architecture/temp/q084-integration/` directory.
- Kernel delta: 90 C files, five private network headers, and deletion of
  `devfs-block-range.h`. The new destination `swap-format.c` is retained unchanged.
  Deleted synthetic `rootfs.c` and its public interface remain deleted.
- Current public headers, driver/userland implementations and all platform
  manifests remain unchanged. Unrelated source-repository softfloat and tool
  changes are outside this import. No commits were made.

The refactored function order, declarations, helper extraction and comments are
the integration base. Overlapping fixes were reapplied by function rather than
accepting whole-file conflict markers or restoring the old formatting wholesale.

## Preserved behavior

| Area | Retained correction |
| --- | --- |
| Disk/devfs/partition | 512/4096-byte geometry, block info/reload ioctls, cached-I/O and open/mount/backing exclusion, validated partition replacement and rollback |
| Mount/inode/syscall | Shared namespace transactions, attachment reservation and rollback, covered-name/inode/ancestor protection, prepare-mutation before inode I/O lock |
| Namecache/overlay/tmpfs | Observed lookup generation, hidden-lower whiteouts, invalidation after committed mutation even if sync fails, partial-write EOF and unchanged zero-length EOF |
| File/backing/VM | Format lease lifetime, fixed EOF, physical aliases and dirty/shared mapping exclusion, shared-object preparation before VM metadata locks |
| Swap | Separate format parser ownership and current source/control/drain behavior |
| WLAN | Tuned-channel OFDM probe rates on W52 and capacity for the maximum SSID; current retirement/authorization behavior |
| Startup | Explicit auxiliary mounts, current mount-query ABI, parser/VFS/commit/init order and exec/ELF cleanup |

## Import defects corrected

1. The source deleted `devfs-block-range.h` but retained its include. Remove the
   stale include and use the refactor's inlined production helpers; point the
   existing range fixture at real `devfs.c` with section GC.
2. Restore the missing colon in the initial process environment:
   `PATH=/bin:/sbin:/usr/bin`.
3. Restore the PTY slave writer's status-flag read after a positive short output.
   A blocking output may observe concurrent `F_SETFL`; using the pre-wait cached
   `O_NONBLOCK` value changed the original retry behavior.
4. Match the host-only overlay helper prototype's existing definition guard.
   The first PCAT build exposed its unused unconditional declaration under
   production `-Werror`; the corrected three-config build set passes.
5. Update the UNIX socket publication fixture's selectors to function definitions
   so new forward declarations cannot satisfy the source-order checks.

## Review and host gates

History inventory, per-function C comparisons, joined string-literal comparisons
and optimized amd64/i386 LLVM IR comparisons were used together. IR equality is
not claimed for every function: guard expansion, declaration lifetime and helper
inlining alter generated IR. Differing functions were reviewed for evaluation
order, lifetime, ownership, error results and state transitions. This is bounded
integration evidence, not a proof that all kernel behavior is bug-free.

| Gate | Result |
| --- | --- |
| Legacy namespace/mount | PASS, 1303 checks per ordinary/sanitized run, 21 concurrent admission/rollback schedules, stale lookup and metadata lock ordering |
| Overlay fault matrix | PASS, 3204 checks ordinary/sanitized/analyzer; final guarded prototype also rechecked ordinary/sanitized |
| tmpfs partial write | PASS, 79 checks ordinary/sanitized |
| Storage foundations | PASS, 20376 checks ordinary/sanitized, amd64/i386 ABI |
| devfs range and dynamic cdev | PASS, ordinary/sanitized/analyzer |
| Filesystem identity | PASS, 110 checks and ownership audit |
| Format reservations | PASS, 583 checks ordinary/sanitized |
| Swap format | PASS, 49333 checks ordinary/sanitized and maintained generator byte equality |
| Backing/swap/VM | PASS, backing claim, manager, drain, runtime control, system ioctl and commitment-resize gates |
| PTY output flag changes | PASS, 103 checks ordinary/sanitized; uncorrected refactor fails the targeted negative control |
| Exec preparation | PASS, 96 checks with the imported current exec.c |
| Boot | PASS, parameter parser, source contract, runtime source reference and aggregate header (32/64-bit kernel, x86 HAL, PC-98, X68k) |
| WLAN | PASS, common core, L2, WPA2 codec/engine, crypto and complete CCMP fixture; supplied sanitizer/analyzer/32/64 ABI modes included |
| Generic networking | PASS, device/ARP/inet hotplug, authorization, route events, UNIX publication ordering and supplied sanitizer/analyzer/ABI modes |
| Retired-source audit | PASS, retained APIs/flags and six manifests |

The aggregate boot-header runner initially lacked a default m68k compiler.
It passes with the existing WS018 extracted compiler and its matching library
path; no toolchain installation or source change was needed.

## Build and runtime gates

Serialized `make -j16 ZEDBSD_CONFIG=config/ci/config-{pcat,pc98,amd64}.mk`
builds all pass, followed by the existing storage and formatter fixture targets.
No aggregate `make check` was run. No production build runs during QEMU.

Both maintained `qemu-system-x86_64` combined cells pass using fresh disposable
media under WS019's runner-enforced temp directory:

- `run-storage-qemu.py combined plan/ws019-installation/temp/q084-storage-integration --mount-protection`:
  mounted namespace protection, idle/busy partition reload behavior, current
  mount inventory and post-reboot persistence pass.
- `run-formatter-qemu.py combined plan/ws019-installation/temp/q084-formatter-integration`:
  guest UFS1/swap formatting, ordinary checks, transition to overlay root with
  file-backed swap and repeated reboot/persistent-file verification pass.
  GPT tables, FAT boot sector and sentinel hashes are identical before/after.

Production image SHA-256 (unchanged throughout both cells):
`c36f5aab3b34365d188b942262468828fa728f819c3c5b7a4d1db39c96c4cc0a`.
Formatter rootfs SHA-256:
`e546a7ec3426a892f1698ba41c6e9c15a35256689734cf59bfe12119c20c6069`.
Runtime command/guest logs and result metadata are retained in those disposable
output directories; host/build logs and review/snapshot artifacts are retained
under this WS's `temp/q084-integration/`. Final kernel file hashes are recorded
there and match the source used for the accepted builds/runtime.

## Acceptance limits and final state

Three configured x86 production builds, focused functional gates, the PTY
negative control and both amd64 runtime cells pass. No physical WLAN retest was
needed or performed for this import; the current driver and prior exact-device
acceptance are retained. Non-x86 runtime and exhaustive whole-kernel concurrency
coverage are not claimed. The X68k check covers the aggregate boot header only.
Final source retains the requested refactor and the newer fixes, with no
unresolved integration failure. Queue/P/W/M are synchronized and `git diff
--check` passes. No private material, SSH action, unrelated source import or
commit was performed.

