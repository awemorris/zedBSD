# UAS requirement/evidence audit — q230 (q218 findings retained below)

Date: 2026-09-10
Status: completed / cleared; applicable QEMU depth-one acceptance verified in q230

The selected implementation is the planned synchronous depth-one LUN-0 disk class,
with HS READY transfers and SS status/data concurrency on three stream IDs. It
uses reserved USB staging, not direct user-page DMA. Do not claim multi-command
queueing, zero-copy SG performance, arbitrary downstream-hub reset, or real hardware
acceptance. Hardware is not a required gate under the user's QEMU-only decision.

| Requirement | Current-state evidence inspected | Judgment |
| --- | --- | --- |
| Descriptor/pipe/stream validation | Production decoder and q145 actual captured descriptors; actual-source malformed descriptor and command fixtures | Implemented; prior ordinary/sanitized results retained in results.md |
| HS READY and SS coordinated data/status | usb-uas-transport.c, uas-transport-host.c, uas-super-host.c; native HS/SS result records | Implemented, depth one; no multiplexing claim |
| ASYNC02–07 applicable transfer ownership | Reserved URBs; cancelled/drained before free, failed drain retains all four; host late/wrong-tag and reserve failure checks; q205-super3/q206-high2 pending disconnect records | Positive evidence for UAS ownership; asynchronous disk-submit latency and multi-queue fairness are not provided by this synchronous class |
| SG02–04 compatibility path | UAS uses setup/wait_reusable and reserved transport buffers; no direct-SG admission advertised | Staged compatibility path; SG01/05 direct-DMA/performance claims are not established by these cells |
| Read/write/file fsync | q203-ufs-high1/q203-ufs2 result.json plus q218-super current UFS fsync/remount portion | Positive persistence/remount evidence; no power-loss claim |
| FLUSH04 uncertain write/error/reset | q210-high1/q210-super2, q211-high1/q211-super1; disk host sticky-error checks | Fixed-media failure/timeout and non-replay evidence; preserve limitations about noncommitting injections |
| REC01/02 bounded sense/reset and failure refusal | uas_probe, uas_command, reset helpers and current disk host failure cases | Bounded retries, mode/unowned attention revoke rather than silently reuse |
| REC03 same-capacity replacement/absence | q214-high1/q214-super1 protocol audits; q215 empty boot/insertion | Positive HS/SS native evidence |
| REC04 retire before removable reset | q217-high1/q217-super3 native results, reset/read IU assertions and disk host live-owner refusal | Positive HS/SS read-timeout recovery evidence |
| REC05/06 old raw descriptor/cache | q208-held-high1/q209-held1 old read/write/fsync rejection and new backing unchanged | Positive physical replacement evidence |
| REC05/06 mounted old filesystem | q218 ordinary ENXIO retained; q228 SS explicit force removes old attachment and permits replacement readback; live/cwd refusal and retry pass | q230 HS/SS retain and discard 4096 dirty bytes, refuse held FD/cwd and republish unchanged replacement; HS post-disposal accounting zero. q229 fixes the observed HS wait stall. |
| Replacement partition discovery | q216-high1/q216-super3 sdX1 command/result and host administrative-open checks | Positive HS/SS MBR replacement evidence |
| Detach/halt | q205-super3/q206-high2 and q213-removable-lifecycle, checked four-CPU samples | Positive documented topologies; not arbitrary topology proof |
| BOT fallback | Both captured configurations have no BOT alternate | Not available; do not switch a live transport |

## Resolution of the q218 findings

q218 reproduced ordinary ENXIO retention; that result remains valid. q220–q228
implemented explicit revoked-media teardown, culminating in public umount -f with
closed admission, preflight/rollback and local dirty disposal. q230 proves the dirty
mounted path at both speeds. Ordinary unmount still preserves synchronization errors.
The separately found BOT administrative-open omission was fixed in q219, with focused
checks/builds and BOT boot evidence; no broad BOT replacement claim is inferred.

The q230 results section records the exact native records/protocol audits re-read
and the final implementation scope. Applicable p029 requirements are complete under
the user's QEMU-only acceptance decision. Prior failures and unclaimed performance,
hardware and multi-command capabilities remain explicitly distinguished.
