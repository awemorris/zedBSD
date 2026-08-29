# WS004 Phase 021: xHCI SuperSpeed interrupt endpoint context

Last updated: 2026-08-29

Phase ID: `ws004-p021`

Status: planned; ready for a Queue proposal; not queued

Parent: [WS004 hardware expansion](../ws.md)

Tests: [WS004 test index](../tests/README.md)

## Objective

Make the native xHCI endpoint context truthful for SuperSpeed interrupt
endpoints. Decode the endpoint companion's `wBytesPerInterval`, validate it
against the endpoint it accompanies, and encode Max ESIT Payload plus Average
TRB Length before issuing Configure Endpoint.

The immediate physical target is the RTL8156 NCM notification endpoint:

```text
address=0x83  type=interrupt-in  wMaxPacketSize=16
bMaxBurst=0  bmAttributes=0  wBytesPerInterval=16
```

Its expected xHCI values are Max ESIT Payload 16 and Endpoint Context word 4
`0x00100010`. The current builder leaves Max ESIT Payload zero. QEMU tolerates
that invalid context, while a native controller may decline to schedule the
notification endpoint. This is independent of the q029 notification-`wIndex`
parser defect and must remain a separate change and Phase.

## Dependencies

- `ws004-p010`: retained SuperSpeed endpoint companion descriptors.
- `ws004-p011`: concurrent xHCI endpoint/request ownership and retirement.
- `ws004-p014` and `ws004-p018`: the NCM interrupt endpoint consumer and the
  exact physical RTL8156 descriptor evidence.
- q029 remote evidence: the same adapter passes connection notification,
  DHCP, NTB transfer, and ping through QEMU xHCI after the parser repair.

## Frozen implementation boundary

1. Add one typed, read-only USB endpoint accessor returning the retained
   SuperSpeed companion or `NULL`. Decode `wBytesPerInterval` to host endian
   when retaining the descriptor. Keep the existing maximum-burst accessor for
   compatibility; do not expose `companion_valid`, add one scalar accessor per
   field, or change the endpoint ownership model.
2. Correlate the companion with a SuperSpeed interrupt endpoint before xHCI
   enable. Require `bMaxBurst <= 15`, reserved interrupt companion attributes
   to be zero, a nonzero interval payload, and a payload no larger than
   `min(wMaxPacketSize * (bMaxBurst + 1), 16384)`. Also require packet size
   `1..1024` with reserved bits clear and `bInterval` `1..16`. Accept every
   payload inside that legal closed range exactly as reported; do not round a
   legal under-report upward.
3. Encode the accepted Max ESIT Payload in Endpoint Context word 4 and set
   Average TRB Length to the same value. SuperSpeed interrupt payload is at
   most 16 KiB, so word 0 Max ESIT High remains zero; LEC/SSP and wider periodic
   payloads are explicitly outside this Phase.
4. Keep FS/LS/HS interrupt contexts and control, bulk, and isochronous contexts
   byte-for-byte unchanged. Isochronous Mult and periodic high-bandwidth
   redesign are outside this Phase.
5. Validate before allocating the transfer ring. Reject a malformed or
   impossible companion before Configure Endpoint without acquiring new DMA
   ownership. Do not silently produce another zero-payload context.
6. Do not add a VID:PID quirk, vendor request, NCM-specific HCD branch, or new
   public UAPI.

## Descriptor compatibility policy

USB 3.x defines `wBytesPerInterval` as the total bytes per service interval and
does not require it to equal or exceed `wMaxPacketSize`. The Phase therefore
accepts any nonzero value no larger than
`min(wMaxPacketSize * (bMaxBurst + 1), 16384)`. Zero, an oversized value, reserved
interrupt attributes, or a missing SuperSpeed companion is rejected before
Configure Endpoint. Linux-style rounding and a missing-companion fallback are
separate compatibility quirks and are not needed for the conforming RTL8156
descriptor.

## Verification plan

1. Extend the production USB descriptor fixture with host-endian
   `wBytesPerInterval` checks and malformed companion cases.
2. Add pure xHCI context encoders or equivalent focused inspection for:
   - RTL8156 values: packet 16, burst 0, payload 16, word 4
     `0x00100010`;
   - zero payload rejection;
   - payload 15 acceptance and payload 17 rejection for packet 16/burst 0;
   - `bMaxBurst > 15` and nonzero reserved interrupt attributes rejection;
   - the 16384-byte ceiling and 16385 rejection; and
   - unchanged FS/HS interrupt, bulk, and control context words.
3. Run the USB function-model, xHCI model/control, concurrent-URB, NCM driver,
   USB binding, and USB-storage regressions in ordinary and sanitizer modes
   where their Phase fixtures provide them.
4. Run `make -j16` and boot a disposable amd64 xHCI USB-storage image to
   `login:` with `qemu-system-x86_64`. Do not use `make check` or `.internal/`.
5. Build one candidate image and combine its first Latitude check with the
   pending q029 NCM acceptance. One successful `net up ue0`, DHCP lease, and
   peer ping is sufficient for the development checkpoint; repeated cold-boot
   acceptance remains the later final reliability gate.

## Completion conditions

- Valid SuperSpeed interrupt companion data produces the exact Max ESIT and
  Average TRB Length fields, including RTL8156 word 0 `0x000a0000`, word 1
  `0x0010003e`, and word 4 `0x00100010`.
- Invalid zero/oversized/reserved-field cases fail before Configure Endpoint
  without leaking DMA/ring ownership.
- Non-SuperSpeed and non-interrupt endpoint contexts remain unchanged.
- Focused, sanitizer, configured-build, and disposable QEMU USB-root gates
  pass.
- One Latitude boot with the combined candidate reaches truthful `ue0`
  carrier, DHCP, and peer ping without a freeze; any remaining failure retains
  the exact endpoint/transfer boundary.

## Reconsideration boundary

Stop and return to planning if the physical controller additionally requires
isochronous Mult, the device reports a payload above the descriptor-derived
maximum, Configure Endpoint succeeds but interrupt completions still do not
arrive, or satisfying the endpoint requires a vendor-specific request. Those
findings are not permission to broaden this Phase.
