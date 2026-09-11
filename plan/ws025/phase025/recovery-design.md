# p025 recovery implementation plan

2026-09-08 JST. Follows completed q120. This phase joins storage recovery with
common disk/cache admission; a driver-local media_error bit is insufficient.

## Observed implementation

usb-storage.c already bounds one transport reset and one 06/29/00 retry, with a
whole-command deadline. Unknown UA/absence sets media_error only in the storage
object. Cached accesses enter disk_cache_enter and currently see only disk live
state/reload ownership. Thus a driver failure alone cannot close old cache hits.
Disk async owners already capture d_media_epoch and reject stale completions.
disk_gone_if_idle performs backing mutation/cache flushing and cannot be invoked
inside the currently submitted BIO: it would encounter or wait on its own owner.
The existing physical disk object must never be reinterpreted as a new medium.

USB shutdown currently performs checked HCD quiesce even when mounted storage
class detach refuses with EBUSY. The old diagnostic says the controller is
retained, without distinguishing retained memory from stopped hardware. First
verify this actual boundary against the p018 observation; preserve mounted and
callback-visible ownership and never force-free it for a cleaner shutdown log.

## Implementation sequence

1. Add a common irreversible media-admission failure to the physical disk owner,
   separate from recoverable transport/persistence proof invalidation. Publish it
   without draining the current BIO or flushing old dirty bytes onto uncertain
   media. Close all ancestor-aware cache, BIO, open, claim, reload and mount
   admissions; invalidate the media epoch and keep old owners releasable.
   Verify admitted reads/old completion publication and loop-backed ancestry.
2. Replace the storage-only media bit with explicit ONLINE, RECONFIGURE,
   REVALIDATE, ABSENT and FAILED state, last decoded sense and bounded reset
   authorization. Keep one self-reset 06/29/00 retry. Classify 06/28/00 and
   capacity-change attention as identity uncertainty, mode-change attention as
   explicit bounded reconfiguration, and unknown UA as non-retryable failure.
   Confirm ASC/ASCQ classifications in maintained SCSI contracts before coding.
   Reconfiguration must refresh cache/write-protection/flush policy before a
   retried write; it does not clear an upper filesystem error state.
3. Keep same-medium command recovery within the existing serialized command
   owner and deadline; it must not wait for disk drain. Separate media retirement
   and reprobe into a control context which does not own a submitted BIO. Only
   an idle/unmounted/unclaimed old object can retire and permit a new physical
   object and partitions. Busy old mounts remain failed against the old object.
   Matching inquiry/capacity never proves identity or authorizes reconnection.
4. Check shutdown with real USB/HCD and storage ownership: stop new work, drain
   storage before transport shutdown, prove checked hardware quiesce, retain
   memory on incomplete class teardown, preserve bounded retries. Change code
   only where this inspection/runtime proves an inconsistency.
5. Add focused production-source fault scenarios before native tests: self-reset
   UA once/repeated, mode/capacity changes, absence, same-capacity replacement,
   unknown UA, queued BIO, cache hits, stale completion, partitions and loop,
   idle versus mounted/claimed retirement, failed detach/quiesce and retry.
   Run ordinary and sanitizer owner tests, relevant prior regressions, supported
   make -j16 builds, USB writeback/journal/readahead/shutdown and fresh images.

No generic error-bit clearing, mounted-root rebinding, unbounded reset, direct
caller DMA or old-dirty-buffer flush into a replacement medium is permitted.
Record exact ownership/admission decisions and evidence in results.md; any
unmet acceptance stays explicit. p026 integration and conditional p027–p030
adoption decisions remain subsequent bounded queues.

## Confirmed SCSI classification

Checked 2026-09-08 against the primary [T10 ASC/ASCQ assignment list](https://www.t10.org/lists/asc-num.txt):
2A/01 denotes changed mode parameters; 2A/09 denotes changed capacity data;
28/00 leaves medium identity uncertain; 29/00 alone cannot prove a locally
initiated reset. The implementation only classifies these values and requires
the command owner's separate reset/retry authorization. Fixed/descriptor current
sense can authorize classification, while deferred sense cannot authorize retry.
The new classifier's 4096 current/deferred/ASCQ combinations pass ordinary and
ASan/UBSan in `../temp/p025-sense-1/`. It is not yet connected to storage state
transitions, and this helper test is not full REC acceptance.

Native media exchange exposed another explicit event: QEMU removable disks report
current `06/3A/00` on ejection/loading, followed by `06/28/00` when loaded.
Confirmed in the primary [QEMU scsi-disk implementation](https://qemu.googlesource.com/qemu/+/refs/tags/v7.2.16/hw/scsi/scsi-disk.c)
and observed as key 6 / ASC 58 / ASCQ 0 in `p025-media-probe-native-2`.
Treat that exact event as ABSENT, with irreversible old-media revocation and idle
retirement before reprobe. It does not authorize a same-medium retry or mounted
rebinding. Unknown qualifiers and deferred sense still fail closed.
