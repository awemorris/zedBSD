# WS004 Phase 021: xHCI SuperSpeed interrupt endpoint context

Last updated: 2026-08-31

Phase ID: `ws004-p021`

Status: uncleared (`q045` automatic/source milestone passed; fresh-image QEMU
and Latitude checkpoints await the existing Noct verifier correction)

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
`0x00100010`. The current builder leaves Max ESIT Payload zero. QEMU and the
final q029 Latitude run both tolerate that invalid context, so this is now a
nonblocking specification correction rather than the explanation for the
physical carrier failure. It is independent of the repaired q029
notification-`wIndex` parser defect and remains a separate change and Phase.

## Dependencies

- `ws004-p010`: retained SuperSpeed endpoint companion descriptors.
- `ws004-p011`: concurrent xHCI endpoint/request ownership and retirement.
- `ws004-p014` and `ws004-p018`: the NCM interrupt endpoint consumer and the
  exact physical RTL8156 descriptor evidence.
- q029 evidence: the same adapter passes connection notification, DHCP, NTB
  transfer, and ping through QEMU xHCI, then DHCP/DNS/external fetch on the
  Latitude after the parser repair.

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
5. Build one candidate image and run one Latitude check when this independent
   Phase is eventually queued. One successful `net up ue0`, DHCP lease, and
   external fetch is sufficient for its development checkpoint; repeated
   cold-boot acceptance remains the later final reliability gate.

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

## q045 implementation result (2026-08-31)

The automatic/source milestone passed. The USB core now decodes retained
`wBytesPerInterval` explicitly from little endian and exposes the retained
companion through one typed, read-only endpoint accessor. The xHCI path uses a
pure transactional context encoder before `ring_alloc()`: USB SuperSpeed
interrupt endpoints require the frozen packet, interval, burst, attribute, and
payload bounds, while legal under-reports are encoded without rounding.
Endpoint context word 4 receives the same accepted payload in Average TRB
Length and Max ESIT Payload Low. SuperSpeedPlus LEC/SSP remains outside this
Phase and retains the preceding context encoding, as do FS/HS interrupt,
control, bulk, and isochronous endpoints.

The Phase-owned runner is:

```sh
TMPDIR="$PWD/build/q045-tmp" \
  plan/ws004-hardware/tests/run-xhci-superspeed-interrupt-context-test.sh
```

It passed ordinary and ASan/UBSan execution plus compiler-analyzer compilation:

- the strict endpoint-context corpus passed 82 checks in each runtime mode;
- the RTL8156 case produced word 0 `0x000a0000`, word 1 `0x0010003e`, and
  word 4 `0x00100010` exactly;
- the USB production-source function model passed 1,415 checks in each runtime
  mode, including host-endian `0x1234` decoding, malformed descriptor headers,
  retained typed access, and a missing-companion `NULL` result;
- the pre-existing xHCI model passed in both runtime modes;
- the source-order gate proved context validation precedes transfer-ring
  allocation; and
- production `pci-xhci.c` and `usb.c` objects compiled with `-Werror` under the
  dedicated amd64/UEFI and i386/PC-AT xHCI configurations.

The independently owned regressions also passed through their available
ordinary, sanitizer, and analyzer gates:

```sh
TMPDIR="$PWD/build/q045-tmp" \
  plan/ws004-hardware/tests/run-xhci-concurrent-urbs-test.sh
TMPDIR="$PWD/build/q045-tmp" \
  plan/ws004-hardware/tests/run-usb-binding-transactions-test.sh
TMPDIR="$PWD/build/q045-tmp" \
  plan/ws004-hardware/tests/run-usb-cdc-ncm-wire-test.sh
TMPDIR="$PWD/build/q045-tmp" \
  plan/ws004-hardware/tests/run-usb-cdc-ncm-driver-test.sh
```

The concurrent-URB fixture passed twice, the binding fixture passed 971 checks
twice, the NCM wire fixture passed twice, and the NCM driver fixture passed
1,540 checks twice. Direct ordinary and ASan/UBSan runs of
`usb-storage-scsi-test.c` and `usb-urb-publication-test.c` also passed, and both
compiled under the analyzer.

The fresh-image boundary is not cleared. This exact configured build was
attempted without `make check`, `.internal/`, or any Noct source change:

```sh
TMPDIR="$PWD/build/q045-tmp" make -j16 \
  ZEDBSD_CONFIG=plan/ws004-hardware/tests/config-amd64-xhci.mk
```

Compilation reached the existing host-Noct verification step, where
`build/NoctLang/build-static/noct --path=tools/build ...` failed with
`Unknown option --path=tools/build`. A separate configured `vmunix` target
linked the kernel containing this change, then failed at the same checker and
deleted the target. Therefore no fresh candidate image existed for the
disposable xHCI USB-root QEMU boot, and reusing the older image would not be
valid evidence. The Latitude candidate/check was consequently not requested.

Resume after the separately owned Noct command-line/build integration accepts
the repository's verifier invocation: rerun the configured image build, one
disposable q35/xHCI USB-root boot to `login:`, then one Latitude `ue0` carrier,
DHCP, and external-fetch checkpoint. No source/model defect or additional
implementation decision is presently known in this Phase.

## Reconsideration boundary

Stop and return to planning if the physical controller additionally requires
isochronous Mult, the device reports a payload above the descriptor-derived
maximum, Configure Endpoint succeeds but interrupt completions still do not
arrive, or satisfying the endpoint requires a vendor-specific request. Those
findings are not permission to broaden this Phase.
