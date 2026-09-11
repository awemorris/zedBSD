# q182 integration log

Status: completed q182

Implemented the actual zero-argument text wizard while retaining explicit
coexistence invocation. Source, mode, destination, default-NO destructive
review and backend callbacks are connected. Native backend owns source/census,
measures logical bytes without following links, admits formatter capacity,
generates non-secret clock/hash-derived UUIDv8 identifiers with visible GUID
collision checks, and drives reserved diskpart/mkfs through one-shot exact
identity/geometry prompts. It copies/verifies the native tree, prepares swap
and fstab, and publishes the three boot artifacts with configuration last.
Public QEMU install/boot acceptance passed in q182-native5; see the final ledger below.

Block inventory now exposes file-backed capability (public bit 8, internal
bit 16 translated explicitly), so virtual disks are excluded without names.
date supports the common %N nanosecond conversion used in identifier material.
Identifiers are not security tokens and no CSPRNG property is claimed.

Passed component gates:
- Native layout: 6 accepted / 30 refused cases after explicit virtual flag.
- Managed transactions: 153 scenarios, including native publication failures
  at every operation and incompatible/unknown profile refusal before I/O.
- Native flow: split PTY prompts, wrong geometry, missing/repeated prompt,
  nonzero child, output bounds, accurate destructive-start state; JIT and
  interpreter. Also menu default/EOF/empty selection, device filtering,
  raw-byte filename decoding and UUID/GPT encoding.
- Actual find/stat measurement with sparse file, hard link, external symlink,
  spaces/newlines and non-UTF-8 name; JIT and interpreter.
- Storage foundation: 41,993 checks each ordinary/sanitized, plus amd64/i386
  ABI. /tmp/zedbsd-q182-storage2.log. Initial run used the obsolete pre-flag
  identity in the file-backed reservation test; refreshed identity now proves
  EOPNOTSUPP, matching actual BLKGETINFO callers.
- amd64 builds /tmp/zedbsd-q182-amd64.log and
  /tmp/zedbsd-q182-amd64-2.log. Host date builds under C89 and emits %s:%N.

QEMU run 1: temp/q182-native1 reached the physical disk menu. Its screen showed
Noct did not interpret the \x1b string escape as intended. Interrupted the
owned runner before destructive approval, then replaced inline escapes with
Term.clear/moveTo and rebuilt. This was not an acceptance pass.

QEMU run 2: temp/q182-native2, /tmp/zedbsd-q182-native2.log passed actual
default-NO cancellation and full target SHA-256 equality. Captured the complete
review screen and showed it to the user. Dedicated execution then stalled at
diskpart input with zero allocated target blocks; interrupted the owned runner.
First disk-menu screenshot raced framebuffer output; later captures wait for
rendering. Production image hash remained unchanged.

Run 3 added a finite prompt-wait diagnostic, but the same zero-write stall
persisted. Code inspection found zedBSD timeout puts its child into a separate
process group without giving it the controlling PTY foreground. The child
stops on terminal input (SIGTTIN). GNU timeout used by the first host protocol
fixture does not have that exact grouping behavior. An actual zedBSD timeout
host build and controlling-PTY test now reproduces default-mode timeout and
proves both -f and the new --foreground alias allow input. Native commands now
request foreground explicitly. These C formatter utilities do not spawn their
own process trees; timeout still bounds and reaps the direct child.

Run 4: temp/q182-native4, /tmp/zedbsd-q182-native4.log passed initialization,
both formats and all 253 files/38 directories of copy. The fresh read-only
comparison returned ENOMEM on /bin/sleep, and the installer correctly reported
partial installation without publishing boot configuration. Cancellation was
explicitly omitted here; run 2 owns that evidence. Source-free boot was not
reached. Its build: /tmp/zedbsd-q182-amd64-4.log, passed.

BUG-019 investigation: fresh VM with copied-disk clone and read-only mounts
reproduced ENOMEM on a different file (q182-verify1), with ~462 MiB free RAM,
no shared-budget refusals and buffer usage near its component cap. Added
capacity_failures/physical_failures to the existing buffer statistics. Native
q182-verify2 confirmed 3 capacity failures and 0 physical failures. These are
diagnostic reads, not final source-tree acceptance (new kernel/userland builds
can differ from the earlier copied root).

The clean-reclaim/retry admission let competing allocators take the reclaimed
space before its requester reserved it. A 64-KiB clean-cache host test with
four readers reproduced 516 ENOMEM results in 20,000 requests. An admission
mutex now covers only component reservation and clean reclaim; shared reclaim,
physical allocation and backend I/O remain outside. It is separate from the
resize mutex, whose dirty-writeback path can recurse through a loop backend.
After correction, ordinary and sanitized runs both had 0/20,000 failures and
passed existing transfer faults, pressure, reentry and concurrent writes:
/tmp/zedbsd-q182-admission-after.log. Shared accounting, dirty/pin refusal and
no-I/O shrink also pass in ws025 temp/q182-cache-accounting. The old host fixture
needed its non-atomic disk-buffer pin stub corrected for actual thread use;
runner adapts obsolete split io-stats/cache-memory references to current
io.c/cache.c and tests current production buf.c.

Current native confirmation: q182-verify3, /tmp/zedbsd-q182-verify3.log, built
with /tmp/zedbsd-q182-amd64-6.log. Record terminal outcome before restarting
the complete installer. Full native installation remains unaccepted.

Still required: successful complete public native install, default-NO unchanged
target, reboot/swap/halt, explicit coexistence regression, failure/cleanup
coverage and final supported builds. p006/p007 remain open, including final
paging stress/fault matrix; p029 graphic frontend remains unimplemented.

q182-verify3 completed: diff returned 1 only for nativecommand.noct and sysctl
changed by the diagnostic rebuild, with no read error; capacity_failures=0,
physical_failures=0. This is a complete read-through, not byte-identical source
acceptance. q182-native5 now repeats default-NO cancellation and a full blank-
disk installation using amd64-7. Its fixture additionally compares the retained
rootfs.img hash after shutdown. Physical destination selection now preserves
4Kn coexistence while restricting native formatting to 512-byte sectors; both
paths reject file-backed destinations. Focused JIT/interpreter native flow and
selection/discovery tests pass after this correction.

q182-native5 PASS, terminal status 0 (/tmp/zedbsd-q182-native5.log). Actual
public wizard default-NO left the blank 1-GiB destination byte-identical. YES
created GPT/64-MiB ESP/958-MiB UFS, copied 253 files and 38 directories, passed
fresh read-only content/metadata comparison, initialized and exercised 32-MiB
UFS swap, published loader/kernel/config last, and released the owned workspace.
Two subsequent boots attached only that installer-created NVMe disk: native
UFS root, automatic swap (truncate refused EBUSY), persisted /etc/installer-check
and checked halt all passed. No host-side target provisioning was involved.
Production SHA256 remained 270fb1e4c27f5cda50ae5784bf78ae525003cfeda081cb6d34a8d31566a560ed;
retained source rootfs SHA256 remained a6cf5078046ee8128d51fb1e442ea7890e8d7eac720603e795a118a65ef8b916.
Actual review/copy/completion screenshots are in temp/q182-native5; the copy
screenshot was displayed to the user. BUG-019 is fixed with host and QEMU evidence.

Additional common-finish scenarios pass in both Noct modes: copy/publication
errors retain the primary cause and device-relative recovery paths; an uncertain
mount cleanup prevents success and preserves the unresolved resource; cancellation
is not success. Existing transaction fault injection and workspace lifecycle gates
remain applicable. run-installer-navigation-qemu.py now tests shell return into
a new source/mode admission plus disk-selection Escape and unchanged target.
Its first launch was not executed because automatic approval review timed out;
the explicitly permitted single retry was approved and is running.

q182-navigation PASS (terminal 0): actual shell opens and runs a command;
exit returns to source selection, then mode/disk selection, whose Escape
cancels without touching the blank target. Owned /run workspace is absent
after return. Coexistence regression is running in temp/q182-coexist.

## Integration acceptance ledger

| Contract | Evidence | State |
| --- | --- | --- |
| Public dedicated mode on blank disk | q182-native5, actual installed command | Pass |
| Default NO, full-target hash unchanged | q182-native5 cancel | Pass |
| Source image unchanged | q182-native5 retained-image SHA256 | Pass |
| Physical/source/readonly filtering and 4Kn coexistence | installer-nativeflow, selection, discovery; JIT/interpreter | Pass |
| Exact child authorization and failure/partial-write evidence | native-command-child.py PTY cases; timeout-foreground-host.py | Pass |
| Layout/capacity and no-I/O refusals | q181 plus nativeplan/nativeadmit regressions | Pass |
| Census progress and metadata oracle | 253 files, 38 directories; fresh RO diff in q182-native5 | Pass |
| File-copy/metadata/protocol failures | p028 results; installer-treeprogress | Pass |
| Publication failure at each operation and config-last | installer-transaction, 153 scenarios | Pass |
| Cleanup ambiguity and stable recovery paths | installer-workspace and nfFinish in installer-nativeflow | Pass |
| Shell return invalidates observations | q182-navigation, public shell/source/mode/disk sequence | Pass |
| Native root, active swap, persistence, halt twice | q182-native5 native0/native1 | Pass |
| Existing-FAT cancel/install/rerun/conflict | q182-coexist | Pass |
| Supported build gates | amd64-7, pcat, pc98 build logs | Pass |

All integration gates pass. Actual paging
stress remains a distinct p007 final acceptance item, and p029 remains planned.

q182-coexist PASS, terminal 0 (/tmp/zedbsd-q182-coexist.log): default cancellation,
actual install, idempotent rerun and single-byte kernel conflict refusal all pass.
GPT/FAT boot records, unrelated sentinels/payload, UEFI variables and production
image hashes match before/after. The injected byte was retained on refusal and
the original kernel was restored afterward. Read-only HMP CPU snapshots during
long verification did not stop the VM or add application input; commands advanced
and completed normally. The silence is a frontend progress issue tracked in p029.
Git diff --check passes. PCAT/PC98 build gates follow sequentially.

Final supported builds: amd64-7, pcat and pc98 all terminal status 0, using
make -j16 with their explicit CI configurations. Logs: /tmp/zedbsd-q182-amd64-7.log,
/tmp/zedbsd-q182-pcat.log, /tmp/zedbsd-q182-pc98.log. No aggregate make check,
no production-image test mutation and no commit were performed. p049 is complete.
The mode-selection/provisioning requirements of p006 are accepted by the same
integration plus its linked prerequisite fault gates. p007 retains final native
paging/fragmentation acceptance; p029 remains unimplemented.
