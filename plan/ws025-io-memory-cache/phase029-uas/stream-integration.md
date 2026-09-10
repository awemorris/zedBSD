# SuperSpeed UAS integration against the current xHCI owner

Date: 2026-09-10
Status: q199 native normal/lifecycle accepted; q200 recovery contract below

The q144 QEMU descriptors require nonzero streams for status/data even with one
command. The current xHCI driver has one active request per endpoint. Preserve
that bounded admission initially; adding stream rings does not authorize queue
multiplexing. Stream zero remains the command/control path.

q198 implemented the configuration gate, primary arrays, selected request ring,
stream doorbell/dequeue/restart and checked cleanup mapped below. Focused helpers
and default-ring QEMU regression pass. SuperSpeed UAS coordination and actual
stream configuration/cancel acceptance remain; the following map retains the
full intended contract.

## USB/class boundary

q197 adds drv_usb_urb_setup_stream and drv_usb_urb_stream_id. Nonzero setup needs
SuperSpeed bulk/companion and explicit HCD capability; ordinary setup clears the
identity. No HCD sets the capability yet. Setup cannot establish that a hardware
stream context exists: enqueue must validate the configured endpoint/stream too.

Next add an explicit class-requested endpoint stream configuration transaction,
before UAS allocates/submits its URBs. Serialize it with interface alternate and
device teardown using the existing core selection/control admission mechanisms;
check existing ownership helpers before choosing that gate. Do not automatically
turn on streams for every endpoint that merely advertises them. The transaction
must reject active URBs, retain existing configuration on failure, and preserve
failed command/DMA ownership. HCD configuration is checked, not a void callback.

Start with a primary stream context array covering IDs 1 and 2 (four context
slots, index zero invalid). Status needs normal-command and management IDs; data
uses the normal command's ID. Use each request's explicit tag/stream rather than
an implicit endpoint global. All capability/allocated-count bounds must be
validated against both endpoint companion and HCCPARAMS1.

## Actual xHCI change sites

- `struct xhci_endpoint`: retain default ring and add owned primary-context DMA
  plus stream rings/configured count. Context/rings live through slot quiescence.
- `xhci_endpoint_enable`, `fill_endpoint`: explicit configuration builds a linear
  primary stream array, MaxPStreams and LSA. Do not replace a context pointer while
  hardware can still access it. Rollback requires completed configure/stop proof.
- `struct xhci_request`: store selected ring and stream ID for its whole lifetime.
  `xhci_urb_enqueue` currently pushes into ep->ring and rings only dci; select the
  validated ring and encode doorbell stream ID in bits 31:16. Keep current
  submission/recovery admission barriers and allocation-free reserved transfers.
- `xhci_event_request_locked`: match event TRB addresses against the active
  request's selected ring, not endpoint->ring. Preserve slot/DCI/range and
  reservation-generation checks. Wrong-stream/old-ring events cannot complete it.
- `xhci_cancel_request`: Stop Endpoint applies to every stream. Set TR Dequeue
  must carry the cancelled stream ID in the command status word and use that
  ring's producer/cycle. The active owner stays published through the barrier.
- `xhci_endpoint_restart_empty`, `xhci_endpoint_recover`, halt/reset paths: ring
  the selected stream, and update every relevant stream dequeue when recovering
  the whole endpoint. Stream zero cannot restart a streams-enabled endpoint.
- `xhci_endpoint_disable`, device release (`ring_free` loop near device context
  cleanup): free all stream rings/context DMA only after the same checked
  endpoint/slot/HCD barriers used for the default ring. Failed barriers retain
  the complete graph. Do not free just the active stream on failure.

Local QEMU `build/qemu-pc98/hw/usb/hcd-xhci.c` confirms primary array sizing
`2 << max_pstreams`, stream ID zero rejection, LSA primary SCT=1, stream ID in
Set TR Dequeue status bits 31:16 and doorbell bits 31:16. `xhci_find_stream`,
`xhci_ep_set_dequeue` and `xhci_kick_epctx` are concrete interoperability checks.
These are implementation observations; they do not replace xHCI ownership gates.

## UAS and acceptance

SuperSpeed does not send READY. The command owner must submit data/status on
nonzero streams according to its direction, accommodate CHECK CONDITION without
full data, and cancel any remaining data URB before releasing the command. Do
not reuse the high-speed sequential wait-for-READY loop unchanged: waiting for
status before data can deadlock. Abort uses its own management stream/tag and
must retire every outstanding host URB before device-task recovery.

Add focused actual ring/provenance/cancel tests, then a superspeed QEMU disk cell:
normal data and filesystem fsync, timeout/abort, detach/replug, and halt. Preserve
high-speed q194/q196 behavior and BOT regression. Until these paths are connected
and exercised, p029 stays uncleared; the new API alone is not stream support.


## q200 recovery decision: reset rather than infer an empty old stream

Local QEMU `hw/usb/dev-uas.c` provides concrete counterevidence to reusing the
high-speed recovery algorithm: `usb_uas_task` aborts the request and queues the
management response, but does not remove already queued Sense entries from
`uas->results`. `usb_uas_handle_data` selects results by stream; reading the
management stream cannot consume a failed command's stream. `usb_uas_cancel_io`
removes a pending USB packet, not queued device status. An empty-read timeout is
not a positive retirement barrier. QEMU's device reset explicitly cancels all
requests and frees queued results in `usb_uas_handle_reset`.

Implement SuperSpeed recovery at the disk-class boundary through a checked USB
reset. Keep high-speed ABORT TASK. This changes the recovery mechanism, not the
requirement to resume new I/O and preserve failed-write uncertainty.

1. Retain device and four endpoint associations under the existing class lifetime.
   Serialize against BIO and quiesce; drain/free the old transport before reset.
   Failed drain keeps all owned resources and admission closed.
2. Call `drv_usb_device_reset`. Its current root-port-only implementation takes
   topology/selection/control gates without waiting for topology ownership,
   checks physical generation before and after destructive steps, stops endpoints
   and HCD DMA, resets the port and restores retained configuration/bindings.
   ENOTSUP for downstream hubs and failed identity proof remain explicit failures.
3. Reconfigure stream arrays and allocate a new reserved transport only after
   successful reset. Old HCD slot/rings cannot be reused: reset has disabled and
   reconstructed them. Never continue with the old reserve after reset failure.
4. Reprobe LUN readiness (bounded reset UNIT ATTENTION), capacity, block size and
   cache/write-protect policy before admitting the waiting *new* BIO. Refactor
   probe output into a candidate description so failed validation cannot mutate
   published disk geometry. Preserve the sticky flush/write-uncertainty field.
5. USB physical identity proof alone does not prove removable media identity.
   Treat changed geometry, media-change sense or unprovable identity as media
   retirement, not permission to send an old descriptor to a replacement disk.
   Connect to the existing disk-generation contract; never replay the failed BIO.
6. Verify actual-source host failure ordering (drain, reset, stream configuration,
   reprobe, media mismatch, sticky write) plus native throttled SS read failure,
   reset and explicit new read success. Update the native harness to distinguish
   reset evidence from the high-speed ABORT IU assertion. Keep HS abort coverage.

The existing `uas_probe` mutates owner geometry and uses SYNCHRONIZE CACHE during
probe. It cannot simply be called after reset and declared safe: preserve prior
policy/geometry and write uncertainty until validation succeeds. This is the
next implementation boundary; q200 does not claim it implemented.
