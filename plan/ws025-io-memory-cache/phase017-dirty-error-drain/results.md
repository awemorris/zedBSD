# WS025 p017 execution results

2026-09-07; q107 in progress. Not complete.

The standalone error ledger (`include/kern/io-error.h`, `src/kern/io-error.c`)
provides paired sequence/errno snapshots, per-observer cursor advancement only to
that captured sequence, independent cursors, and conservative sticky saturation.
A successful retry does not erase error history. It is not yet connected to the
normal kernel build or file/inode/mount/disk owners.

`temp/p017-error-core.log` passes ordinary and ASan/UBSan tests: older snapshot
acknowledgement cannot skip a newer event, independent observers see each failure,
old cursors cannot regress a newer acknowledgement, saturation preserves later
errors, and four readers verify paired snapshots against 100,000 concurrent
alternating-errno publications (72,315 / 106,674 observed intermediate events).

Remaining: owner/observer integration, dirty indices, explicit stacked drain,
through adapters and real subsystem/three-build/FS50-WiFi30-native gates.

## Owner/observer checkpoint

The ledger is now linked by platform builds and embedded on inode/mount/disk
owners. VM writeback errors preserve inode/mount history, and failing write/flush
BIO completions preserve the leaf disk's error. File descriptions carry independent
cursors; references/dup share the same cursor. Mount sync has its own cursor.

Shared-page drain is centralized in file_fsync before the backend flush, under
the description lock; syscall fsync keeps its descriptor validity checks and calls
this common path. A current drain failure always wins. A newer captured failure
with a different errno is not acknowledged by returning an older current errno.
Backend argument/capability errors are not recorded as storage writeback history.

`temp/p017-file-observer-2.log` passes 411,601 / 410,451 checks with real file,
VM, ledger and memory-budget code: independent opens observe the same failure,
a shared description acknowledges once, close/reopen sees retained owner history,
repeated dirty retries still fail, successful retry preserves bytes, another fd
still receives EIO, and file observations leave the mount cursor untouched. The
mount cursor portion currently exercises the ledger directly; production mount
sync, disk completion and explicit drain/dirty-index integration still need their
focused tests and native gates. An interim amd64 observer build is in progress.

## Dirty membership checkpoint

Block buffers now maintain global age and per-device intrusive dirty lists.
Enqueue/clear/discard are constant-time transitions under a dedicated rank-129
guard after the buffer lock. Sync selects a referenced dirty member and releases
the guard before cache/owner locks or I/O. Eviction closes membership and rechecks
references under the same guard, preventing retirement of a selected descriptor.
It no longer rescans clean hash buckets for every dirty candidate.
`temp/p017-dirty-buffer-2/` and its outer log pass both variants: held selection
blocks discard, separate device lists stay independent, failed writes remain
indexed, concurrent redirty needs a second write, and discard removes membership.

VM pages similarly maintain owner-local dirty membership under the object lock.
The PTE reverse-map discovery pass remains necessary and is preserved; the list
does not pretend to detect hardware dirt without revocation. Resize orphans are
excluded from writeback, restored on abort when dirty, or retired after committed
truncation and final unpin. `temp/p017-vm-dirty-orphan.log` passes 412,805 / 408,665
checks including real pinned orphan writes on abort and commit, alongside existing
cache/observer/concurrency cases. No delayed worker is running. An interim native
build is in progress; explicit stacked context and final phase gates remain.

## Native recursion found during intermediate integration

`temp/p017-native-dirty-512` reached the warm-cache check, then trapped in
mutex_lock_interruptible. A link-only mutex wrapper in `temp/p017-lock-probe.*`
confirmed recursive ownership without changing production locking. Its printf
used unsupported HAL %p formatting, so it did not resolve a useful lock address.
Static tracing identified the newly introduced cycle: VM sync owns the inode I/O
lock, calls overlay backend fsync, and overlay called generic file_fsync on the
same content inode, now re-entering VM sync. The host backend had no overlay layer
and therefore could not reproduce that cycle.

`file_fsync_backend` now serializes only backend/open-file state, without a new VM
drain or observer acknowledgement. overlay_regular_fsync uses this explicit lower
entry. Generic file_fsync retains the VM+backend+observer pipeline. The diagnostic
kernel was removed and an ordinary kernel rebuilt; the failed native/probe logs
are retained. A fresh native run must confirm this fix before phase completion.

`temp/p017-native-drain-fix-512` passes the ordinary-kernel file-cache, actual
MAP_SHARED/msync, truncate/copy-up, budget pressure and sysctl controls after the
backend split. `nm` confirms no __wrap_mutex symbol remains. This validates the
recursive-drain fix; it does not complete the remaining context/observer gates.

## Explicit synchronous context integration (in progress)

Added borrowed operation provenance to file_io, overlay, loop, FAT, UFS,
buffer writes and direct BIO splitting. Backend fsync bypasses upper VM drain.
FAT scopes its borrowed context under the existing mount mutation lock; UFS
passes data context as an argument and scopes journal/snapshot callback context
under their respective locks. Metadata strengthens ordered/drain; no delayed
mode is enabled. Context does not authorize backing claims. BIO submission
captures leaf identity and persistence epoch, and short successful writes become
EIO before completion publication and ledger recording. Retained dirty buffers
do not retain borrowed context pointers.

`temp/p017-disk-context-buffer-2` passes real buffer/index/budget regressions in
ordinary and sanitizer builds. `temp/p017-stack-build-2.log` passes amd64 after
FS integration. Earlier build diagnostics (header forward declaration, an
accidentally extended disk_view_matches signature, and an obsolete unused UFS
helper) were corrected; their logs remain. The first storage gate stopped on a
host-only direct pwrite_inode call after its signature extension; a legacy
through wrapper now retains that internal fixture interface. The subsequent
storage gate is in progress. These are interim results, not p017 completion.


## Final p017 verdict — complete (2026-09-07)

- WB01–WB05: `temp/p017-file-final.log` passes 412942 / 409319 checks
  (ordinary / sanitizer), including independent descriptions, dup shared cursor,
  retry failures, retained dirty pages and close/reopen. Primitive snapshot races
  and saturation pass in `temp/p017-error-context-final.log` (88184 / 215562
  paired concurrent observations).
- Production BIO/mount owner tests `temp/p017-owner-frontier-final-2` pass 71
  checks per variant: independent short-write error observers, real mount sync
  historical and current errors, unknown operation/context rejection before
  admission, out-of-order holes, captured flush target, late completion and
  callback retirement. Existing p014 FLUSH01–FLUSH06 evidence remains applicable.
- `temp/p017-context-propagation-2` passes both variants with nested buffers,
  scalar and 64 KiB run origin/claim/generation propagation, invalid flags before
  mutation, dirty index selection lifetime, retry/redirty and clean reclamation.
- `../ws018-kernel-architecture/temp/ws025-p017-stack-2/results.json` records
  FS50/50, Wi-Fi30 ordinary/sanitizer and two native USB boots passing. Real FAT,
  loop, UFS journal/snapshot and overlay remain synchronous through adapters.
- `temp/p017-final-{pcat,pc98,amd64}.log` supported builds pass.
  `temp/p017-native-stack-final` passes current ordinary-kernel cache, MAP_SHARED,
  msync, truncate/copy-up, cache budget and sysctl controls at 512 MiB.
- Physical runtime remains user-accepted, not agent-measured. No delayed mode,
  owning async request or stale-media recovery is claimed here; these remain
  p018/p019/p025. `git diff --check` passes. No commit was created.

The first context propagation fixture failed because its old global assertion
forbade any busy upper buffer during intentionally nested lower allocation.
Restricting that preparation assertion to the non-nested cases retains its
original purpose; the nested test now verifies inherited context independently.
The first ledger-linked frontier fixture needed explicit host IRQ adapters;
production ledger and locking were not weakened to satisfy isolated linking.
