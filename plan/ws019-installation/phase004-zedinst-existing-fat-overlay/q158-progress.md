# q158 progress

Date: 2026-09-09
Status: in-progress

Added confirmation.noct, using this process's Term.isTTY, exact destination
phrase, bounded input, editing and protected Term.close after the input body.
The command is not packaged; root checks, mount ownership and public flow
integration remain. Noninteractive execution correctly refuses.

The real-PTY test executes actual host Noct and compares complete terminal
attributes before/after each operation. Exact input, backspace correction,
empty/mismatched input and excessive input passed in JIT mode. The next case,
a lone Escape, timed out after 15 seconds; the harness killed/reaped only its
own child. Session 60157 ended with exit 1. This is retained as a failure,
not an accepted confirmation component.

Root cause and canonical-package repair scope are recorded in
[p024](../phase024-noct-terminal-input/phase.md), added to q158. No build/QEMU
process remains live at this checkpoint. Do not start public installer runtime
before repairing terminal progress and completing integration.

## Subsequent repair and target finding

Canonical patch 0003-terminal-partial-input-progress.patch is integrated as
zedbsd4 with the same upstream commit. Old manifest-verified extracts are
preserved under `../temp/q158-noct-old/{host,target}`. The fill routine now
waits for new bytes after a partial event; reads respect remaining capacity,
partial EOF terminates, and overlong partial events/numeric parameters are
bounded. Host build and maintained amd64/pcat/pc98 builds passed; logs are
`/tmp/zedbsd-q158-{noct-host,amd64,pcat,pc98}.log`.

Real host tests initially passed 16 confirmation cases plus fragmented
CSI/UTF-8/SS3, EOF and terminal restoration. First native run is retained at
`../temp/q158-confirmation/result.json`: exact confirmation cancelled because
the kernel console maps Enter to LF (`src/kern/tty.c`, INPUT_KEY_ENTER), and
Noct correctly exposes LF as Ctrl-J. The installer now accepts that event as
well as CR, still requiring exact text. Updated host tests pass 18 confirmation
cases, two fragmented-input PTYs, two pipe refusals and eight EOF cases; see
`/tmp/zedbsd-q158-terminal-host2.log`.

The first native runner session 36795 exited 1 and reaped its QEMU. Rebuild the
private fixture with the Enter correction, then rerun into a fresh directory.
p024 remains in-progress until native confirmation passes. Public mount and
publication integration still remains in p004.

Second fixture build passed `/tmp/zedbsd-q158-fixture2.log`. Native rerun uses
`../temp/q158-confirmation2`, live session 64708. The first exact-confirmation
case now reports CONFIRM ACCEPTED and storage-result-0; remaining cases are
running. Poll that session rather than restarting on an observation timeout.

Session 64708 subsequently exited 1: all eight native confirmation cases passed,
but the final harness command tried to exec shell builtin `echo` as a program.
The helper correctly reported ENOENT/127. Replace that harness check with the
existing `id -u` command and rerun; keep confirmation2's failure record intact.

Final p024 checkpoint: `../temp/q158-confirmation3/result.json` passes eight
native cases and final id execution; protected hashes match. Session 60939
exited 0. Canonical host/target source verification passed as well. p024 is
completed with [results](../phase024-noct-terminal-input/results.md). No process
remains live. q158/p004 stays in-progress for owned mount/public integration.

## Workspace integration component

Implemented workspace.noct: exclusive root-owned 0700 directories via the
existing mkdir command, covered-directory identity checks, explicit FAT mounts,
reverse-order unmount/rmdir and preservation of uncertain/replaced objects.
Each effect is recorded pending before execution; cleanup stops at an unresolved
resource and never recursively removes foreign contents. This observes inode
identity, not an atomic mount-generation lease.

installer-workspace.noct passes ten modeled lifecycle/recovery cases in host
JIT/interpreter modes. Initial test failure was use of nonexistent Dict.keys;
the fixture now uses native dictionary iteration and Dict.size. Cases include
pre-existing root, mkdir/mount/unmount effects with lost completion, busy
unmount, rmdir failure, replacement, nonempty root and repeated cleanup.

Private fixture build passed `/tmp/zedbsd-q158-workspace-fixture.log`. Native
source-only runner is live as session 24535 in `../temp/q158-workspace`; it
executes the modeled tests and actual owned source mount/unmount before existing
source inspection. Poll the same session and inspect result.json before claiming
native success. Full public installation remains incomplete.

Workspace native runner session 24535 completed with exit 0 and PASS source
inspection. `../temp/q158-workspace/result.json` retains equal before/after GPT,
FAT boot, sentinel and production-image hashes. The actual owned source mount,
unmount, populated tmpfs regression and subsequent source inspection all passed.

## Source preflight integration

Added preflight.noct, connecting discovery to the workspace owner. Every
recognized FAT partition on the retained source disk is mounted read-only once;
firmware and configuration origins may share a mount. Require exactly one
current zedbsd.cfg on the retained configuration partition, read it within a
4096-byte bound, validate its boot profile, and capture only loader/kernel/rootfs.
Revalidation compares retained metadata, mount identities, candidate uniqueness,
configuration inode/text and immutable input digests. Live data/swap are not
copy inputs.

Private build passed `/tmp/zedbsd-q158-preflight-fixture.log`; CLI missing-operand
refusal confirms host module loading, not functional preflight acceptance.
Native validation is running in `../temp/q158-preflight`, session 58426, using
the --discovery runner and its two-partition NVMe fixture. Poll that handle.
Do not mark this integration accepted until its actual result is available.

Remaining p004 integration: destination reload before owned mounts, other
destination configuration candidates, existing-file/capacity preflight without
creating directories, confirmation summary, post-confirmation directories and
seed, full transaction publication/revalidation, cleanup and public packaging.
p005 installed-NVMe-only boot follows full p004 acceptance.

Source preflight runner 58426 completed with exit 0 and PASS source and
destination discovery. `../temp/q158-preflight/result.json` has equal protected
GPT/FAT/sentinel, second-payload and production hashes. The native source
preparation/recheck/cleanup marker is present in its guest log.

## Destination preflight integration

Added destination.noct. Existing ancestors are checked without creating missing
EFI directories; all six existing managed files must verify before any staging.
Existing stage paths are refused as unowned recovery state. The returned plan
contains only missing directories and per-partition required/free capacity.
Every recognized FAT target partition is inspected for a competing zedbsd.cfg;
only the selected payload may contain the managed marker. Selected ESP/payload
mounts are writable, others read-only, all owned by the workspace stack.

instDiscoverReloaded resolves source/target identities before issuing the existing
diskpart reload command, refuses the source disk, and performs full discovery
again afterward. Child registration may change; the whole disk and source
provenance must remain stable. This occurs before acquiring any target mounts.
The reload is an admission check, not an ongoing device reservation.

Host JIT/interpreter destination tests passed 64 existing-file subsets, 384
individual file conflicts, four path conflicts and absent EFI ancestor planning.
Private build passed `/tmp/zedbsd-q158-destination-fixture.log`. Native runner
session 16610 is live in `../temp/q158-destination` with --discovery; it now
executes full source/destination preparation, capacity and owned cleanup.
Poll that session and inspect result.json before claiming native acceptance.

Remaining: confirmation display and bounded wait, final post-pause revalidation,
post-confirmation directory/seed creation, transaction/recovery integration,
public packaging and complete p004/p005 acceptance. p024 remains complete.

Destination runner 16610 completed with exit 0, PASS source and destination
discovery. `../temp/q158-destination/result.json` and guest.log retain actual
reload/source/destination preparation, capacity, cleanup and unchanged protected
hash evidence. This is preflight acceptance, not a completed installation.

## Integrated interactive installation

Added install.noct and main.noct (private fixture only; no packaged launcher yet).
The entry point requires actual caller terminals and root before device work.
One protected body owns preparation, summary/confirmation, post-pause metadata
and capacity checks, missing directory creation, exclusive attributes-only seed,
existing managed transaction and cleanup. Confirmation is bounded to 1200 input
polls. Temporary stage directories are removed only with retained ownership;
published final files/permanent EFI directories are preserved on later failure.
Recovery paths for transaction leftovers/publication use device-relative names
after temporary mounts disappear. Mount cleanup is attempted independently of
reported stage leftovers.

`/tmp/zedbsd-q158-confirmation-bounded.log` passes the existing 18 real-PTY
confirmation cases, fragmented input, EOF and restoration after the polling
bound change. Host main rejects noninteractive invocation before discovery.
Private fixture build passed `/tmp/zedbsd-q158-install-fixture.log`.

Native integrated runner `run-install-qemu.py` is live as session 12156 in
`../temp/q158-install`. It performs actual console cancel, install and exact
rerun through main.noct, compares the entire destination after cancellation,
and retains GPT/FAT/sentinel/payload, firmware variables and production hashes.
Its deadline is finite (3600 seconds); poll the same live handle. Full p004 still
requires successful integration, conflict/recovery acceptance and public packaging;
p005 installed boot and other Priority residual phases remain outstanding.

First integrated run: cancellation passed the whole-destination hash comparison.
Installation then exited 1 with `invalid private staging seed` before publication.
The runner was still waiting for a success marker; after observing the explicit
guest terminal failure, its verified PID 95130 received SIGINT so its finally
block saved results and stopped/reaped QEMU. Session 12156 exited 130, and the
failure record remains in `../temp/q158-install/result.json`. This was not a
restart caused by an observation timeout.

The seed inherited the source executable mode. The next native fixture records
`ls -l /bin/sh` as `-rwxrwxr-x` (0775), not the required staging mode 0755.
After exclusive creation and regular/empty-file ownership checks, the installer
now explicitly chmods its own seed to 0755 and verifies identity/permissions.
No source executable permissions are changed. The runner also recognizes a
nonzero guest exit immediately rather than waiting for success until deadline.

Updated private build passed `/tmp/zedbsd-q158-install-fixture2.log`. Current
native rerun is `../temp/q158-install2`, live session 41645. Poll it; cancel,
install and rerun acceptance is not complete yet. Source and destination
preflight are already accepted separately. p005's plan was reconciled with
single 64-bit UFS and the generated explicit PARTUUID boot0 binding.

Current observation: session 41645 is live. The corrected run completed cancel
and entered the approved install body without the previous seed-mode error.
No installation success is claimed yet.

Read-only integration review while runtime proceeds:

- Public package can register an amd64-only package requiring base/noct, with
  a custom copied shell launcher and data modules. Package-class custom targets
  avoid the basic-command C link rules; image checks require an executable
  launcher but only enforce the ELF ABI on /bin/sh. Final configuration and
  standalone-install verification must accompany registration.
- Complete rerun currently plans stage directories and a seed even when every
  final file is already verified. Fix this before full acceptance: require a
  stage directory only on a partition with missing managed files, and no seed
  when all files exist. Otherwise a byte-identical full destination can be
  spuriously rejected for staging capacity. Still refuse existing unowned stage
  paths; do not interpret a previous stage as owned recovery state.
- Add native single-byte conflict refusal using existing dd conv=notrunc on a
  disposable managed kernel, preserve/restore its prior bytes with guest cp,
  and rerun the unchanged public orchestration. The existing dd supports that
  operation; no helper command is required.

While session 41645 remains live, prepared the complete-rerun fix exclusively in
`/tmp/zedbsd-q158-rerun` (copies of production Noct modules plus the destination
fixture). Production files are unchanged by this preparation. The candidate
plans stage directories only for partitions with missing files, refreshes the
presence array after confirmation, and skips seed creation when nothing is
missing. Updated host JIT/interpreter tests pass all prior subsets/conflicts
plus zero capacity requirement and no seed operations for a complete destination.
After runtime terminates, review/apply the differences in destination.noct,
install.noct and installer-destination.noct, preserving intervening edits.

Latest runtime evidence: read-only observation of the disposable target shows
the full 33554432-byte rootfs staging file after the kernel, and creation of the
data.img staging entry. This is intermediate progress, not final on-disk
acceptance. Session 41645 is still live; no restart or production mutation.

Further intermediate observation: data.img staging size reached 33554432 bytes
and the swapfile staging entry appeared. Prepared (not executed) a follow-on
runner in `/tmp/zedbsd-q158-rerun/run-install-qemu.py`. After install/rerun it
mounts only the disposable target, saves the kernel to tmpfs, writes one zero
byte over the ELF magic via existing dd conv=notrunc, unmounts, invokes the same
installer and expects refusal, verifies the corrupted digest stayed unchanged,
then restores and compares the original kernel. Python compilation passed.
This draft must be applied to the owning tests directory before execution;
its HERE-relative fixture imports intentionally assume that final location.

Initial installation in q158-install2 now reports all six Published paths,
Installation complete and storage-result-0. Session 41645 continues the exact
rerun; final protected hashes are still pending. Independent read-only mtools
comparison found loader/kernel/rootfs byte-identical to the actual source and
the generated configuration names the selected payload PARTUUID. Evidence is
`../temp/q158-install2/published-input-comparison.json`, explicitly an intermediate
rerun observation. The first ad hoc comparison incorrectly classified every
non-ESP partition as payload; the source also has a BIOS boot partition. The
corrected comparison selects the unique fixture MS basic-data partition.

Prepared public package drafts `/tmp/zedbsd-q158-rerun/{Makefile,zedinst.sh}`.
Shell syntax passes; the draft package is amd64-only, optional, requires base/noct
and registers all runtime modules under /lib/zedinst. It is not applied/built or
accepted yet; production configuration, standalone installation and runtime
launcher acceptance remain to be verified after current QEMU terminates.
