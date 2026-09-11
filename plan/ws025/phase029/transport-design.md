# UAS transport design from q144 captures

Date: 2026-09-09
Status: descriptor evidence and implementation design; no UAS data path yet.

q145: stage 1 implemented in `src/drivers/usb/usb-uas.c` with
`include/drivers/usb-uas.h`. `tests/run-uas-descriptor-host.sh` compiles the
production decoder: ordinary and ASan/UBSan each pass 10,209 checks, including
both captured configurations, endpoint reorder/renumber, alternate isolation,
every truncation, invalid fields and deterministic mutation coverage. Failure
clears output. Both x86 USB-storage builds compile the decoder without a UAS
driver registration; PC98 remains unaffected. Explicit `make -j16` CI builds
for amd64, PCAT and PC98 pass (`/tmp/zedbsd-q145-{amd64,pcat,pc98}.log`).
Host evidence is `/tmp/zedbsd-q145-uas-host2.log`; the first sandbox run passed
ordinary checks but LeakSanitizer rejected the ptrace environment, so it was
rerun with approved execution permissions, not with checks disabled.

## Actual backend

Installed QEMU 10.0.11 enumerates UAS under the current amd64 kernel on both
EHCI and xHCI. Evidence: `../temp/q144/{high,super}/{result.json,uas.pcap,argv.json}`.
The kernel logs `class 08/06/62 no-driver` and reaches login. Source boot image
and blank 32 MiB SCSI LUN hashes are unchanged. This establishes an enumeration
backend, not successful zedBSD UAS read/write. UAS stays in Priority, not Future.

| Property | EHCI / high speed | xHCI / super speed |
| --- | --- | --- |
| Configuration bytes | 62 | 86 |
| Interface / alternate | 0 / 0 | 0 / 0 |
| Class/subclass/protocol | 08/06/62 | 08/06/62 |
| Command, status, data-in, data-out addresses | 01, 82, 83, 04 | 01, 82, 83, 04 |
| Bulk max packet | 512 | 1024 |
| Companion max burst | absent | 15 |
| Stream exponent: command / other pipes | absent | 0 / 4 (16 streams) |

Endpoint numbers are observations, never identification rules. Associate each
Pipe Usage descriptor with its preceding endpoint within the selected alternate.
Use `drv_usb_configuration_raw_descriptors` to retain that association; a flat
interface-extra list alone cannot establish the endpoint association.

## Stages and ownership

1. Pure bounded descriptor parser first. Select the requested interface and
   alternate; reject truncated/duplicate/missing/inconsistent pipe or companion
   descriptions. Return capabilities separately from transport support.
   Tests use captured bytes, malformed variants and reordered endpoints.
   No driver matches protocol 62 until transport is ready.
2. High-speed depth one is the first data transport. Send Command IU, receive
   status IU; on READ/WRITE READY issue data in the indicated direction, then
   collect Sense IU completion. No-data commands wait directly for completion.
   Validate IU length, tag, direction, status and sense length. Unknown or
   duplicate READY and stale completions are errors. Reuse transport-independent
   SCSI codec/flush helpers, without BOT residue or BOT reset semantics.
3. SuperSpeed requires explicit USB/HCD stream support. Even depth one uses a
   nonzero stream tag on this backend. Add endpoint stream allocation, stream
   ring DMA ownership, doorbell stream IDs and completion provenance; command
   remains stream zero. Checked stop/cancel must retire every stream's DMA.
4. Hold command/status/data buffers, URBs, disk BIO and generation until
   completion or checked quiescence. Stop admission before detach/reset. Do not
   reuse a timed-out tag until both host transfer and device task are retired.
   Task management needs its own IU tag and validated response. Failed
   quiescence quarantines retained resources; it never releases live DMA.
5. Publish a disk after capacity/block-size and flush policy are known. Preserve
   disk generation, prefix accounting and synchronous completion; flush waits
   for prior writes. Neither captured configuration has a BOT alternate, so
   fallback is unavailable here. Never switch transports with outstanding I/O.

Host tests cover malformed descriptors/IUs, missing/reversed READY, wrong tag,
timeout/quiescence, queue exhaustion and short data. Native tests must prove
read/write/fsync persistence, detach/replug, generation rejection and shutdown.
A parser-only result remains uncleared for full p029.
