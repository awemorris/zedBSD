# q136 verification result

2026-09-09. Current runtime evidence improves the phase, but the historical
invalid-free provenance is still unproven. No speculative lifetime change was
made to production code.

## Current-code tests

The maintained runner reads each ordinary PCAT/PC98 disk's own rootfs, validates
UFS allocation ownership, renames only the `/bin/login` directory entry to
`/bin/nogin` in a disposable copy, validates it again, and boots that copy.
It does not substitute a rootfs left by another architecture's shared build.
Each output records source hashes and actual QEMU argv. Sources remain unchanged.

| Evidence under WS002 temp | Scope | Result |
| --- | --- | --- |
| q136-pcat2 | Ordinary init/getty, missing login, three independent boots | Six exec failures each, no additional respawn in the subsequent observation window, no fatal |
| q136-pc98 | Same on PC98 QEMU, three independent boots | Same bounded six failures, no fatal |
| q136-lifecycle-pcat3 | Test-only PID 1 repeatedly forks/execs the real getty, waits for failure status 1 and checks owners | 100/100 cycles pass |
| q136-lifecycle-pc98 | Same real getty/exit/wait paths on PC98 | 100/100 cycles pass |
| q136-normal-pcat2 | Ordinary unchanged login image | Three login/pwd/logout/respawn cycles pass |
| q136-normal-pc98 | Same ordinary sessions on PC98 | Three login/pwd/logout/respawn cycles pass |

The lifecycle probe compares process, thread, filedesc, file, VM-space, HAL-task,
HAL-stack-byte and HAL-space counts after every wait, allowing at most one second
for deferred retirement. It verifies the child PID's LOGIN_PROCESS utmp record,
which getty writes only after terminal acquisition succeeds. It does not claim
an internal per-terminal reference/generation census that the public snapshot
does not provide. The real init crash-loop policy is tested separately above;
the probe does not replace that test.

## First-use cache distinction

The first lifecycle attempt reported one extra file. This was investigated,
not ignored by loosening an equality comparison. On PCAT, a parent-only read of
getty changes file 9 -> 11 and VM objects 2 -> 3, and initializes a read worker.
Parent-only creation/read of utmp changes file 11 -> 12 and VM objects 3 -> 4.
Neither preparation forks or creates a terminal session. These are retained
read-cache objects and handles (`vm_object_get_shared_internal` owns its cache
file), not a demonstrated exit leak. After this explicit input/cache preparation,
all 100 getty failures preserve exactly the same compared owner counts.
Cold observations are retained in q136-lifecycle-pcat, pcat2 and pcat3 logs.

This establishes steady-state ownership recovery, not universal equality of
all cold filesystem/page-cache counters. It does not prove the historical
page-backed allocator failure has been fixed.

## Fixture corrections and limitations

- The first preparation incorrectly assumed a platform-named architecture UFS.
  The runner now extracts rootfs from the selected disk, avoiding the shared
  i386 build artifact ambiguity. No guest ran in that failed preparation.
- The first normal PCAT probe attempted absent `/bin/true`. Its inherited
  status-only helper reported zero despite the visible `not found` diagnostic.
  That cell is not acceptance evidence. The maintained normal test now requires
  actual `pwd` output `/root`, as well as successful login/logout/respawn.
  The shell status observation is retained for separate WS001 investigation.
- Lifecycle fixtures use a separate test-only PID 1 ELF, built with the normal
  user ABI. They are not installed into ordinary images. Both focused builds
  pass (`/tmp/zedbsd-q136-lifecycle-build3.log` and
  `/tmp/zedbsd-q136-lifecycle-pc98-build.log`). Ordinary kernels remain q135's
  three-architecture build-verified sources; this queue changes only fixtures
  and planning documents.

## Uncleared boundary and resume condition

The original invalid allocation free did not recur. Exact allocation/free
provenance and forced fragmented-heap/page-backed fallback coverage are still
missing, as is an internal terminal-generation ownership census. Keep p021
uncleared rather than claiming that normal exit or 200 repetitions diagnosed
that old failure.

Resume with bounded test-only allocator call-site/ring provenance and controlled
small-heap fallback/fragmentation stress around the same getty failure path.
Preserve both ordinary six-restart tests and the current owner comparison probe.
Change production lifecycle only if a first bad owner is demonstrated. These
are software investigation residuals, not a hardware or user-permission gate.
Continue ready work in WS006 while retaining this phase and its evidence.

## q137 independent reproduction lead

WS006 paired USB input/root-I/O now reaches a heap walk stall in the amd64
process reaper: pointer_block <- heap_allocator_free <- kern_free <- detached
page-table freeing <- vmspace_reap_pending. Two QMP observations show the same
free target and traversal register while the CPU remains inside the walk with
IF clear. This is not yet proof that the old PC98 invalid free has the same
cause. [p024](../phase024-retirement-heap-integrity/phase.md) owns the bounded
heap capture and first-failure diagnosis; keep this phase's remaining ownership
and missing-login requirements intact.
