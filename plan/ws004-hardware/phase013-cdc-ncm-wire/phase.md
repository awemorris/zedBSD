# WS004 Phase 013: CDC NCM wire codec and negotiation

Last updated: 2026-08-29

Phase ID: `ws004-p013`

Status: completed (`q027`)

Parent: [WS004 hardware expansion](../ws.md)

Tests: [WS004 test index](../tests/README.md)

## Objective

Implement a strict, allocation-bounded CDC NCM 1.0-compatible NTH16/NDP16
codec and negotiation model that can be tested independently of USB hardware.

## Frozen profile

- Little-endian NTH16/NDP16 only, Ethernet datagrams, no CRC, MTU 1500.
- RX accepts multiple datagrams and a bounded NDP chain.
- Initial TX emits one datagram per NTB; layout still obeys the device's
  divisor, remainder, and alignment constraints.
- GET_NTB_PARAMETERS is required. Negotiated sizes are clamped to explicit
  driver/HCD limits. SET_NTB_INPUT_SIZE and other required profile requests are
  modeled; optional unsupported formats fail rather than being guessed.
- Packed wire structures are never directly dereferenced. All integer and
  range operations use explicit checked little-endian loads/stores.

## Planned work

1. Define internal NCM constants and the compact negotiation/result objects.
2. Validate parameter sizes, formats, max NTB/datagram values, divisor,
   remainder, alignment, and arithmetic overflow.
3. Parse NTH16, NDP16 chains, datagram entries, terminators, signatures,
   sequence numbers, offsets, overlaps, loops, and block bounds.
4. Build a legal one-datagram NTB with padding chosen from negotiated values
   and without relying on an unimplemented HCD zero-packet flag.
5. Add valid, malformed, boundary, multi-datagram, sequence-wrap, alignment,
   and fuzz-style deterministic fixtures compiled against production source.

## Completion conditions

- Valid single/multi-datagram NTBs round-trip through production code.
- Every malformed length, offset, chain, signature, overlap, alignment, zero
  divisor, and overflow case fails without out-of-bounds access or allocation.
- Negotiated values never exceed explicit resource limits.
- Ordinary and sanitizer-focused host fixtures and configured source builds
  pass.
- `git diff --check` passes without `make check` or `.internal/`.

## Reconsideration boundary

Stop if an actual device requires NTH32, CRC mode, or NCM 1.1 behavior for the
basic bind path. Record that descriptor/negotiation evidence as a later profile
extension rather than widening this codec silently.

## Result

Completed on 2026-08-29.

- The production codec negotiates an explicitly resource-bounded NTH16/NDP16
  profile, parses bounded multi-datagram/NDP-chain input, and builds one-frame
  no-CRC NTBs without packed-structure dereferences.
- TX observes divisor, payload-remainder, alignment, max-packet, and effective
  NTB limits. It distinguishes the function-advertised `dwNtbOutMaxSize` from
  a smaller local clamp, so the NCM no-ZLP maximum-size exception is used only
  at the true device boundary.
- Malformed lengths, offsets, signatures, overlap, chains, sequence errors,
  zero-block short-transfer handling, aliasing, and callback failure were
  exercised against production source.

Evidence:

- `run-usb-cdc-ncm-wire-test.sh`: PASS;
- ASan/UBSan production-source fixture: PASS;
- GCC `-fanalyzer`: PASS;
- `make -j16`: PASS;
- independent specification/code review found no remaining completion blocker
  after the advertised-maximum correction.

Physical NCM interoperability remains the declared WS005 acceptance boundary;
this Phase proves only the independently testable wire contract.
