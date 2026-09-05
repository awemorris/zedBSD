# Q075 confirmed-commit publication evidence

Last updated: 2026-09-05

Owner: [ws011-p009](../phase009-confirmed-commit-overlay-publication/phase.md)

Queue: [q075](../../queue-q075.md)

Predecessor: [q074 evidence](q074-confirmed-commit-qemu-evidence.md)

## Result boundary

Normal-build case 06 reproduced the guest-side ordinary-commit timeout. Its
saved CPU/process state localizes the expensive operation to startup-file
writing through the FAT backing file of the UFS upper layer. This is distinct
from case 01's host-only route/DNS predicate failure, which was repaired and
revalidated without another guest run.

The one instrumented run and normal cases 01--05 completed confirmation,
70-second stability, and reboot persistence. They did not establish a causal
repair. Case 06 remains a recorded pre-fix failure. The first FAT traversal
correction and subsequent authorized optimization passed deterministic cost,
safety, sanitizer, and maintained-build gates. All four remaining post-fix
cases 07--10 passed, including the actual failing `show candidate` sequence.
This is a localized correction with four post-fix target passes, not ten
successful non-reproductions. The original case 06 failure remains evidence.

## Environment and execution record

All cells used QEMU 10.0.11 (Debian), amd64 PC/AT `pc`, 512 MiB, four virtual
CPUs, ISA NE2000 at I/O `0x300` / IRQ 10, and synthetic MAC
`52:54:00:11:07:01`. QEMU user networking used `restrict=on`. Old intent was
`10.0.2.15/24`, confirmed intent `10.0.2.17/24`, gateway `10.0.2.2`, and DNS
`10.0.2.3`. Every cell used a fresh disposable image. Bounds remained 120
seconds for boot, 30 seconds per command, a public one-minute confirmed timer,
and a real 70-second post-confirmation wait.

The instrumented run enabled `ZEDBSD_NCOM_TRACE`, debug information, and frame
pointers. Normal cases explicitly disabled those build options;
`NCOM_CAPTURE_FAILURE=1` only enabled failure snapshots and a bounded stopped
guest inspection window. It did not add tracing to the guest or increase the
30-second command bound. T020 was not rerun.

| Run | Changed procedure | Result |
| --- | --- | --- |
| Diagnostic | Trace/debug build, otherwise baseline | Observations passed; not acceptance |
| 01 | Baseline, 15 ms host key spacing | Guest passed; host predicate failed; retained evidence revalidated |
| 02 | Baseline, 30 ms key spacing | Passed |
| 03 | Baseline, 5 ms key spacing | Passed |
| 04 | Wait 1 second immediately before ordinary confirmation | Passed |
| 05 | Wait 5 seconds immediately before ordinary confirmation | Passed |
| 06 | `show candidate` before the normal startup display/confirmation | Guest ordinary commit timed out; snapshot retained |
| 07 | Replay case 06's actual failing `show candidate` sequence | Passed post-fix |
| 08 | Edit candidate to .18 and back to .17 before arming | Passed post-fix |
| 09 | Ordinary-commit unchanged intent first, making startup upper-backed | Passed post-fix |
| 10 | Ordinary-commit again after confirmation | Passed post-fix |

Successful observations showed old checksum `645868049 462 /etc/net.conf`
followed by `1760403986 419 /etc/net.conf` after confirmation, after 70 seconds,
and after reboot. All four observations retained the expected address, route,
resolver, and successful one-packet gateway ping. The canonical writer removes
formatting present in the initial 462-byte fixture; 419 bytes is expected.

Local detailed records remain under ignored directories
`plan/ws011-net-config/temp/q075-diagnostic-01/` and
`plan/ws011-net-config/temp/q075-normal-matrix/`. The matrix's original failure
rows are retained rather than overwritten.

After recurrence, case 07 was reassigned from the redundant extra startup
display to the actual failing case 06 sequence. This consumes one of the four
remaining normal cells; it does not repeat or relabel the consumed case 06.

## Case 01: host predicate defect, not a guest recurrence

The guest returned `Commit complete.`, waited 70 seconds, rebooted, and produced
all final observations. Both controller and QEMU exited zero. The host then
failed `require_state()` because the single-quoted route and resolver regular
expressions used two backslashes before each dot. The regex engine therefore
looked for a literal backslash instead of matching the dotted address.

The [runner](run-confirmed-commit-qemu.sh) now uses a single regex escape inside
those single-quoted strings. Its double-quoted netmask expression correctly
retains two shell-level backslashes. A focused offline check demonstrated that
the old predicates reject case 01's actual route/DNS lines and the corrected
predicates accept them. Q074's retained T020 old/temporary/restored route and
DNS observations also passed the corrected predicates; no T020 was repeated.

[Offline revalidation](validate-confirmed-commit-evidence.sh) imports the same
observation functions as the live runner, verifies the recorded normal-build,
controller/QEMU, and input-integrity prerequisites, and checks startup view,
four states, checksum transitions, two boots, completion, and fatal patterns.
Case 01's original `results.tsv` and `run-metadata.txt` still record the host
failure (`runner_exit_status=1`). Separate `01/revalidation.tsv` records
`NCOM-T021` / `pass`; `01/revalidation.Cwrwnd/provenance.sha256` identifies its
raw inputs and validator versions. Matrix resume started at case 02.

This predicate correction does not explain q074's missing guest completion or
case 06's later guest-side timeout.

## Case 06: startup write dominated by FAT-chain traversal

The raw guest log again ends with ordinary `commit` and ten admitted networkd
connections, without completion or another prompt. Authentication messages
alone still cannot prove final DNS completion. The stronger saved state does:

- `/sbin/net`, PID 18 / thread 22, is running on CPU 1 in syscall 5 (`write`),
  descriptor 5, user buffer `0x418518`, length `0x1a3` (419 bytes).
- Descriptor 5 names an overlay regular file. The recorded write extent holds
  the canonical new startup configuration, not a DNS request. Only the recorded
  419-byte extent is relevant; a debugger's unbounded C-string display may show
  additional stale bytes past that extent.
- `networkd` is sleeping in its poll event channel. The client has progressed
  beyond reconcile and is still inside the startup write, before subsequent
  stream close, rename, and confirmed DISARM.
- CPU 1's stack/caller addresses identify buffer-cache access inside
  `fat_raw_next_cluster()` / `fat_raw_validate_chain()` called from the FAT
  backing-file write. Normal optimized binaries have limited unwind data;
  generic stack-address scans are candidates, not independently reliable full
  backtraces. The typed syscall/file/mount state and matching caller sites
  provide the stronger localization.

The visible root is overlay with read-only UFS lower and writable UFS1 upper.
The upper's 32 MiB `DATA.IMG` is itself a FAT32 file on the outer boot partition;
FAT is therefore on the write path even though it is not the visible upper
namespace. The saved mount has 512-byte sectors, four sectors per cluster,
and two FAT copies. Independent read-only inspection of the preserved disk
found exactly 16,384 contiguous, acyclic 2 KiB clusters, from 16,795 through
33,178, ending at `0x0fffffff`. Both FAT copies are identical.

At the sampled old Floyd validator, the slow pointer was at chain index 5,685
and the fast pointer at index 11,370. A complete validation of this valid chain
requires 24,576 next-entry reads. The write path repeatedly validates/seeks the
large backing chain while handling small upper-filesystem writes. Repeated
slow/fast accesses also defeat the FAT driver's single-sector working cache.

The global block-cache counters at capture were 6,981,626 hits, 562 misses,
546 read BIOs, and 3,817 write BIOs. These are cumulative counters, not a
measurement of only the one 419-byte write. The cache held 2,428,928 bytes with
a 4 MiB limit: `CONFIG_BUF_CACHE_KIB=0` selects automatic sizing here, not a
disabled cache. Net had accumulated 2,499 system ticks and four user ticks.

Both the live stopped-guest inspection and corrected core show the buffer
cache lock as unheld (`held=0`, `owner_valid=0`). Net is running, not waiting on
a lock queue; the active buffer has no I/O in flight. Together with the valid
long chain and millions of cache hits, this supports excessive CPU traversal
cost, not the previously suspected lock deadlock, missing DNS response, or
corrupted cyclic chain. The 30-second timeout was not relaxed.

Case 06 started at `2026-09-05T07:23:40Z`; the failed controller stopped the VM
for capture at `07:24:36Z`. It exited after the bounded inspection interval,
with controller status 1 and QEMU status 0. The paused interval is not evidence
of additional guest progress or of a rollback timer failure.

## Core-artifact correction and provenance

Only one memory dump was taken from case 06. Its raw QEMU paging-aware ELF had
incorrect mappings: a kernel direct-map segment advertised only 640 KiB of file
data but 512 MiB of memory, causing GDB to synthesize zeros over real RAM;
low-half user mappings were also incorrectly sign-extended. Early offline
zero-valued counters after the failed target connection in `gdb-cache.log`
are invalid and are not used above.

The original dump was retained unchanged. A separate
`guest-memory-mapped.elf` corrects only ELF metadata: restrict false zero-fill,
add the verified direct-map segment for physical RAM `[1 MiB, 512 MiB)`, and
normalize the affected user virtual addresses. The file/physical relation was
checked against three existing user mappings and CPU 1's raw stack bytes.
Guest memory bytes were not changed. The corrected core's cache-lock state
matches the live snapshot, and its restored user mapping exposes the recorded
net write buffer. No additional QEMU run or second memory capture was used.

Host-only header-derived type objects were loaded into GDB; they were never
linked into or written to the guest. Authoritative local artifacts are case
06's `gdb-snapshot.log`, `gdb-lock.log`, `gdb-core-derived.log`,
`gdb-cache-mapped.log`, and `data-chain-inspection.json`. Mapping and disk
inspection helpers remain in ignored `temp/ncom-postmortem-types.ZTXL96/`.
Raw memory, disk images, and verbose debugger records are not committed.

## Final FAT correction and deterministic host gates

The final responsible-path correction in
[`fat.c`](../../../src/drivers/fs/fat.c) uses a single forward Brent validator,
fuses full validation with locating the operation's start cluster and old tail,
and advances a local cursor through data writes, sparse zero-fill, and growth.
It avoids alternating distant FAT sectors and repeatedly seeking from the chain
head. Full bounded validation still precedes mutation, including corruption
beyond the requested write range. Cycle/range checks, mirrored FAT updates,
growth rollback, write results, and synchronization order are retained. The
cursor exists only for the operation under the mount lock; there is no
cross-operation validation cache to become stale.

The [cost fixture](fat-write-cost-host-test.c), invoked by
[its runner](run-fat-write-cost-host-test.sh), links production FAT code against
the maintained in-memory VFS/disk mocks. It matches the recurrence's 32 MiB
file, first cluster 16,795, 16,384-link chain, 2 KiB cluster size, FAT32 format,
and two FAT copies; its disposable whole-volume geometry is smaller than the
guest's outer partition. It counts mock backend sector reads, not guest BIOs,
wall-clock latency, or scheduler progress.

| Operation on the 32 MiB backing file | Before correction | Final correction | Enforced maximum |
| --- | --- | --- | --- |
| 4 KiB at offset 8,192, matching the sampled write | 16,399 reads | 138 reads | 160 reads |
| 4 KiB at offset 33,550,336, near EOF | 17,423 reads | 138 reads | 160 reads |
| 8,193 bytes at unaligned offset 511 | 16,413 reads | 150 reads | 170 reads |
| Append 4 KiB | 17,573 reads | 155 reads | 180 reads |
| Zero a 4,099-byte gap, then write 8,193 bytes | 19,971 reads | 217 reads | 240 reads |

The exact final fixture was run against pre-correction FAT source blob
`43ecdcbca988028e4b0353e558101e6d617a02d9` and the frozen corrected source.
The old-code report-only run passed 1,660,782 safety checks with five cost
bounds disabled; enabling those bounds fails the first 16,399-read operation.
Corrected normal and ASan/UBSan runs enforced all five bounds and passed
1,660,787 checks each. Thus the improvement has an executable old-fails/new-passes
regression, not only a wall-clock comparison.

The cost fixture rejects self/prefixed/full-chain cycles, free/reserved/bad and
out-of-volume links, a corrupt tail beyond the write target, and early/late/tail
read errors. Rejection preserves prior size and bytes and performs no data
write. Successful cases check payload, FAT mirrors, accounting, and explicit
synchronization. Final before/expected-failure logs are in
`temp/fat-write-cost-final-before.Z6aMGs/{report-only,enforced}.log`; final normal
and sanitizer logs are `temp/fat-write-cost.LhmYEd/result.log` and
`temp/fat-write-cost.5YELMY/result.log` under WS011. Earlier 139/267-read results
were intermediate and are superseded by the final table.

The separate [cursor fixture](fat-write-cursor-host-test.c), through
[its runner](run-fat-write-cursor-host-test.sh), passed 237,441 checks in both
normal and ASan/UBSan modes. Its 12 geometry cases cover fragmented FAT12/16/32,
unaligned offset 511, FAT12 logical 1,024-byte sectors, and eight sparse
zero-fill/payload handoffs at exact or interior boundaries. It injects every
in-loop growth write-failure ordinal: 19 FAT16/512-byte-sector and 36
FAT12/1,024-byte-sector cases. These verify restored FAT bytes, no leaked
allocation, old directory/size, successful retry, and remount. Logs are
`temp/fat-write-cursor.A2gzBK/test.log` and
`temp/fat-write-cursor.FWDbQ0/test.log`. These are single-threaded mock gates,
not independent proof of target scheduling or overlay atomicity.

Maintained native FAT gates passed twice with 441,782 checks per run, now
retained in `temp/q075-final-gates/fat-native.log`. The final gates also passed:

- WS011 parser, persistence, reconcile, confirmed model, console, boot, and
  interactive confirmed integration;
- atomic writer success/error/order checks in normal and sanitizer builds;
- production overlay publication with synthetic backend/locks, 199 checks in
  normal and sanitizer builds;
- ZNV2 protocol, Wi-Fi command, managed WLAN, credential store, and Wi-Fi child
  runners;
- maintained amd64 and i386 kernel, `net`, and `networkd` builds, including the
  PC/AT ELF contract. Aggregate `make check` was not run.

The first i386 build command incorrectly inherited an amd64 configuration with
AX211 enabled and failed to link `drv_pci_intel_ax211_driver_register` and
`drv_pci_intel_ax211_devices_ready`. This command/configuration error is retained
in `temp/q075-final-gates/i386-build.log`; it is not a successful gate or the
FAT regression. Selecting maintained `ZEDBSD_CONFIG=config/ci/config-pcat.mk`
passed kernel/net/networkd and the PC/AT ELF contract, recorded in
`i386-ci-build.log`. Other retained logs are alongside these in
`temp/q075-final-gates/`.

## Post-fix target acceptance and reproduction keys

Cases 07--10 all completed normal-build confirmation, the unchanged 70-second
wait, reboot, four-state address/route/DNS/ping checks, and the expected
checksum transition. Their controller, QEMU, and runner statuses were zero;
all input-integrity checks passed. Logs contain no diagnostic trace, fatal
diagnostic, expired rollback, or degraded rollback. No failure capture pause
was needed. The actual failing `show candidate` sequence is case 07.

| Case | Ordinary commit host elapsed time |
| --- | --- |
| 07 | Confirmation: 1,072 ms |
| 08 | Confirmation: 1,072 ms |
| 09 | Initial unchanged commit: 1,065 ms; confirmation: 1,173 ms |
| 10 | Confirmation: 1,073 ms; repeated commit: 1,173 ms |

These are host wall-clock observations including keyboard delivery and prompt
polling, not isolated guest syscall timings. The command deadline stayed at
30 seconds. Case 06 and post-fix case 07 have byte-identical `net.elf` and
`networkd.elf`; the normal userland was not replaced with a traced/debug build
to obtain the successful result.

Equivalent single-cell commands, from the repository root, are below. They
describe the four completed variants; another execution requires a new
authorized budget and a new output directory.

```sh
evidence_root=plan/ws011-net-config/temp/q075-postfix-NEW
for item in 07:show-candidate 08:edit-roundtrip 09:upper-startup 10:repeat-commit; do
    NCOM_DIAGNOSTIC=0 NCOM_CAPTURE_FAILURE=1 NCOM_CELL_SELECTION=t021 \
        NCOM_DIAGNOSTIC_HOLD_SECONDS=120 KEY_DELAY_SECONDS=0.015 \
        NCOM_VARIANT="${item#*:}" \
        bash plan/ws011-net-config/tests/run-confirmed-commit-qemu.sh \
        "$evidence_root/${item%%:*}" || break
done
```

The authoritative existing outputs are `q075-normal-matrix/07/` through `10/`.
For each cell, `results.tsv` records `NCOM-T021` and `input-integrity` passes;
`run-metadata.txt` records `ordinary_commit_N_status`,
`ordinary_commit_N_host_elapsed_ms`, `ncom-t021_controller_status`,
`ncom-t021_qemu_status`, `runner_exit_status`, and before/after hash keys.
`ncom-t021-guest.log` is raw; `ncom-t021-guest-logical.log` removes CR only.
The `ncom-t021-{old,confirmed,after-deadline,rebooted}-begin/end` markers bound
the four observations. `Commit complete.` and two `init: system running`
records establish completion and reboot. `matrix.tsv` retains case 06's fail
and marks cases 07--10 `pass-post-fix`, rather than claiming ten passes.

## Hash identities and completion boundary

| Artifact | SHA-256 |
| --- | --- |
| Production `config.mk` | `6e30fa8c0b14d40bebc7d8c6f4bc6a0ebd547af36a426ffeb2972dc171063262` |
| Production `build/amd64/hdd-image.img` | `5e12781eb52127f5f56744298adfe16d79c27b34726916d1953c23617cc61ab9` |
| Test configuration | `57b259056c98bc309587120f942c89fb4ba6063f6af7b7f913694202e515ba4b` |
| Instrumented source image | `8d2f9a97add765d1e4f30e896f9ed689ad4966cfef99ce701835f32f6d2d9edd` |
| Normal source image, cases 01--06 | `2dd0a2a18a7bb7dfe61b41f5e8f3fb9e2694ab924898a6319e2a42d5da7861e8` |
| Normal post-fix source image, cases 07--10 | `55ca25349a0aaed3b34136b9bda0c0fe8c0ecf943a25df371564290b6de3b457` |
| Case 06 kernel ELF | `7a43cd25c7308614c192011893ca7965c6a8b0393525de9cd77ef3ca306bd474` |
| Post-fix kernel ELF, cases 07--10 | `ab43eae5e913aebdc88e42a182ddb066015a8bf6ba36ad1c03dcfe6a961b049c` |
| Unchanged normal `net` ELF, cases 06/07 | `55d5d534ff018c7ce8e0284c14e439f37fc89734cdb16f991e86ae00c78e6fed` |
| Unchanged normal `networkd` ELF, cases 06/07 | `e7f7d632f14d8af8eb005c5249e09c000f4852d314d2a8fff7e0a563ed9d1a61` |
| Final production `src/drivers/fs/fat.c` | `12e82f7006ad9a186846ab2d75f7b604a5161a9f68ea8e73f28fa8d75109cfa5` |
| Case 06 raw guest log | `c55608ecefa13d66589c709b9f66e4f2c3c40b594d531d25e801a52fae5bda46` |
| Case 06 failed disk image | `b2646f47e64a5e7e7378e774f06509c05801ed8a177b57b56581f4ca8676862f` |
| Case 06 original memory ELF | `5348ea3ca2d34b42f7ca5dcd83f2ba7b2ed86f935a0ad9b5cb9a496674efea88` |
| Case 06 corrected-mapping ELF | `9dbab5826083ebb2e07608f5e30488cc008cad9c3a8e521c56818f0237d80d8e` |

All ten normal cells recorded unchanged within-cell production/test/source
input hashes. Case 06's tracked-tree digest was
`5698090a0a95287ebb17e979933db0c48e64d75760b13ff22096067f8a0a5ba5`
both before and after execution. Every post-fix cell recorded
`346069ba0c166052b8cc839090cf86ea7c8afa28c4b6bd4beda583747d937e39`
as both tracked-tree digests. Planning/evidence synchronization occurs after
these runtime snapshots, not during the frozen target runs.

The bounded correction and acceptance gates are complete: the old code fails
the deterministic cost gate, the final code passes cost/safety/build gates, and
four post-fix target cells pass with unchanged normal userland and durability
ordering. This establishes the responsible-path correction without erasing
case 06 or claiming an exhaustive proof against every scheduling/storage
failure. Physical acceptance p008 is outside this evidence and remains separate.
