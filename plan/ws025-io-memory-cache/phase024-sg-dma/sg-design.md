# SG design checkpoint

Implementation in progress in q120, 2026-09-08 JST. p023 is completed.

Current USB storage does not hand arbitrary caller memory directly to hardware.
USB core copies the synchronous client into an URB-owned sync_buffer, retained
across a failed cancel; xHCI then copies that staging into a second coherent
reservation. wait_reusable may detach the caller after bounded failed retirement,
while the HCD-owned URB and its isolated staging remain alive. Preserve this.
Mapping a pool/worker/client pointer directly would lose isolation after timeout:
a mere page pin prevents free but does not prevent the caller reusing the bytes.

Use one shared, HCD-reserved staging owner for eligible normal bulk URBs instead.
Extend the optional reservation contract to expose its CPU staging pointer only
when the controller explicitly supports this ownership. Core reserve_transfer must
prepare the new owner fully before replacing old staging/reservation, and record
that this pointer is borrowed from the HCD reservation (never hal_free it). URB
setup remains excluded while HCD-owned, so output bytes cannot change under DMA;
input bytes reach the client only after successful checked retirement. Normal
completion releases request ownership before terminal publication as today.
Failed cancellation retains the entire URB/reservation/vector. Async arbitrary
buffers, controls and unsupported HCDs retain the existing bounce protocol.

The DMA layer owns bounded scatter backing (up to current 64 KiB transfers), using
independent RAM pages with vmap where suitable, or constrained coherent contiguous
storage when the DMA mask/capability requires it. CPU VA, CPU PA and device address
remain distinct; no generic pointer subtraction proves DMA reachability. Validate
segment count, byte totals, alignment, mask, device boundary and overflow before
publication. Record exactly one DMA resident charge and retain the device owner.
The staging owner lives until reservation retirement, not merely until wait returns.
Do not introduce direct user-page DMA in this phase.

xHCI normal TD construction and short-transfer accounting need the same bounded
segment/TRB plan, respecting each 64 KiB boundary and ring capacity. A single BOT
command still covers contiguous LBAs; physical scatter does not imply scatter LBA.
The existing reclaim reserve and stop/dequeue/controller-quiesce fences remain.
One shared core/HCD staging removes a full extra HCD copy without exposing mutable
caller memory, even when a particular allocation happens to be physically contiguous.
Measure bytes copied and actual TRB/PFN shapes under forced fragmented/high memory.

Expose a bounded synchronous disk vector adapter with full validation and existing
BIO sequencing/error/claim/media admission. Unsupported drivers may assemble into
one bounded scratch request or split only as required; do not present split backend
calls as a single command. Native USB DMA scatter can work with the existing b_data
CPU-contiguous run because its backing is a page vector. Keep arbitrary scatter-VA
adapter behavior explicit and test partial confirmed prefixes/errors.

Tests must cover ordinary and short completion, early/late event, failed cancel
with caller reuse, reservation growth failure, device mask fallback, bad vector,
ring/TD boundary and exact owner accounting. Reuse actual USB lifecycle and HCD
fixtures plus native USB writeback/readahead; native zero-copy claims require
measured copy counters and physical vector evidence, not host helper tests alone.
