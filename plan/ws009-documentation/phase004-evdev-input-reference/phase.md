# WS009 Phase 004: evdev and input reference reconciliation

Last updated: 2026-08-31

Phase ID: `ws009-p004`

Status: complete (`q046`)

Parent: [WS009 documentation](../ws.md)

Product reference: [evdev compatibility profile](../../../docs/reference/evdev.md)

Tests: [WS009 test index](../tests/README.md)

## Objective

Reconcile the public evdev and `/dev/input/eventN` reference with the
production UAPI and the completed WS006 p001--p007 behavior. Remove stale
statements that still describe character-only state and aggregate pointer
ownership as unresolved after q044, while keeping live USB HID and final
legacy-console removal explicitly future work.

## Frozen boundary

- Document only observable ABI, event, queue, capability, state, resync,
  source-ownership, console-subscriber, and detach behavior already present in
  production headers/source and executable WS006 fixtures.
- Preserve the experimental-UAPI label. Do not claim Linux or FreeBSD binary
  compatibility, stable event numbers, USB HID runtime, output reports, or
  removal of the legacy console UAPI.
- Explain the distinction between physical make/break/repeat sources and
  character-only momentary sources, plus per-source pointer/key state and
  transactional `SYN_DROPPED` resynchronization.
- Link current public headers, implementation owners, Phase evidence, and
  focused tests. Do not change source, UAPI, or Noct.

## Verification

1. Compare every constant/layout/ioctl statement with
   `include/uapi/zedbsd/input.h` and its compatibility wrappers.
2. Compare queue, capability, state, subscriber, source, overflow, and detach
   statements with production input/console/HAL implementations.
3. Cross-check the published claims against WS006 IN-T00, IN-T10, IN-T20,
   IN-T30, and IN-T40 evidence without presenting parser-only work as a live
   device.
4. Run DOC-T00 relative-link validation and a source-anchor audit. Do not use
   aggregate `make check` or `.internal/` material.

## Completion conditions

- the stale q044-resolved residual paragraph is replaced by current truthful
  multi-source and character-only semantics;
- every public ABI and behavior claim has a header/source/test anchor;
- live USB HID and legacy-console removal remain visibly unimplemented;
- DOC-T00, DOC-T10, and DOC-T50 pass; and
- WS009 and master status point to the reconciled reference.

## Reconsideration boundary

Stop if reconciliation requires changing public UAPI, defining USB HID policy,
or inferring behavior not covered by production source and the retained WS006
fixtures. Return such a finding to its producer WS instead of documenting a
planned behavior as current.

## q046 result

The public reference now replaces the two stale residuals from p005 with the
q044 production boundary:

- physical make/break/repeat sources and character-only momentary sources are
  described separately, with truthful capability and held-state behavior;
- key/button/axis state, readers, grab, and detach are documented as
  per-device ownership rather than an aggregate pointer model;
- the bounded internal console subscriber, per-source translation state, and
  the fact that `EVIOCGRAB` does not suppress console delivery are explicit;
- reader-local overflow and physical-HAL transactional resynchronization are
  distinguished, including atomic `EVIOCGKEY` replacement and the public
  `SYN_DROPPED`/`SYN_REPORT` result; and
- terminal held-key/button releases, queue drain, EOF, and `POLLHUP` are tied
  to the production unregister path.

Every behavior section links its public header, production owner, and focused
fixture. The evidence table maps IN-T00, IN-T10/11/12, IN-T20/q044, IN-T40,
and the Xzed IN-T30 consumer boundary without presenting parser-only HID work
as a live USB device.

Current limitations remain visible: the UAPI is experimental; Linux/FreeBSD
names are not binary ioctl compatibility; successful `eventN` generations are
not yet reused; live USB HID binding/hotplug is p008; and the legacy console
event/key-state UAPI remains until p009. No source or UAPI change was required,
so the reconsideration boundary was not crossed.

Verification completed from the repository root:

```text
DOC-T00 reference/WS009 scope                       PASS (150 links)
DOC-T00 producer-linked documentation scopes        PASS (314 links)
DOC-T10 public header/source/test anchors           PASS
DOC-T50 current/experimental/future distinctions    PASS
IN-T00 LP64/ILP32 syntax/layout                     PASS / PASS
q044 ownership runner ordinary + ASan/UBSan         PASS
git diff --check                                    PASS
```

Neither aggregate `make check` nor `.internal/` material was used.

The first live run covered `docs/reference` plus `plan/ws009-documentation`.
The second added the WS003, WS013, and WS016 producer evidence reached by the
boot reference. An unfiltered whole-live-`plan` run also descends into an
ignored WS008 `temp/` source-copy whose upstream-only document is deliberately
absent; disposable `temp/` content is outside DOC-T00.
