# WS009 Phase 006: completed-producer documentation follow-up

Last updated: 2026-09-05

WSID: `ws009`

Phase ID: `p006`

Combined ID: `ws009-p006`

Status: Complete (`q072`, 2026-09-05)

Parent: [WS009](../ws.md)

Tests: [WS009 test index](../tests/README.md)

Queue: [q072](../../queue-q072.md)

## Objective

Bring public documentation forward to the already accepted USB HID, WLAN,
Intel Mac image-variant, and Noct/toolchain state before the next implementation
wave. This Phase documents current behavior only; it does not implement or
promise the later WS006, WS011, WS017, WS019, or WS022 changes.

## Required updates

1. Add a networking reference covering the completed `/sbin/net wifi`,
   `networkd`, `/sbin/wifi`, `/etc/wifi.conf`, one-managed-connection policy,
   30-second userspace retry, credential ownership, link-event recovery,
   permissions, and observable failure states. Keep raw test credentials and
   `.internal/` paths out of public documentation.
2. Reconcile the evdev reference with physically confirmed USB HID Report
   Protocol and hotplug. State truthfully that Xzed and the selected Noct BeUI
   backend consume `/dev/input/eventN`, while the old `/dev/console` event and
   key-state UAPI remains deprecated until `ws006-p009` actually removes it.
3. Extend the boot/build guidance with the three amd64 image Variants:
   `UEFI + BIOS (for PC/AT)`, `UEFI (for Apple)`, and `BIOS (for PC/AT)`.
   Describe the physically accepted Intel Mac UEFI-only boundary and do not
   imply that the Apple internal USB-storage quirk is solved.
4. Reconcile the source/build guide with the completed project LLVM/cache,
   x86 sysroots, host-Noct versus target-noct ownership, and selected Noct
   2.0.1 acquisition path.
5. Update documentation navigation and status banners so current, deprecated,
   planned, and manually blocked behavior are not conflated.

## Completion conditions

- The networking examples agree with the installed command grammars and
  completed WS004/WS005 evidence, including no public WLAN interface operand
  in `net wifi` and no long-lived password inside networkd.
- The input reference claims physical USB HID support but does not prematurely
  claim `ws006-p009` removal.
- The build/boot guide reproduces each configured amd64 Variant and correctly
  separates host bootstrap tools from zedBSD target artifacts.
- Relative-link validation, documentation command/example checks, and
  `git diff --check` pass without exposing credentials or `.internal/` data.

## Follow-up ownership

WS011 p006/p007, WS019 p002--p005/p008/p009, WS022, WS017, and WS006 p009 each
update the corresponding public reference when their behavior exists. This
Phase must label those contracts as planned rather than documenting them as
installed commands or UAPIs.

## Result

- Added [`docs/reference/managed-wlan.md`](../../../docs/reference/managed-wlan.md)
  from current `net`, `networkd`, `wifi`, profile-store, route-event, and q071
  evidence. It records the six interface-free commands, effective-UID store
  selection, `root:network` mode-0660 socket, four persistent policy states,
  bounded transients, deterministic selection, 30-second child retry, link-loss
  recovery, permissions, limits, and observable failures without a real
  credential or retained internal path.
- Reconciled [`docs/reference/evdev.md`](../../../docs/reference/evdev.md) with
  production Report-Protocol USB keyboard/mouse/tablet binding, immediate
  hot-unplug unpublication, stale-generation fd isolation, final-close event
  number reuse, the accepted physical HID result, and the current Xzed/Noct
  BeUI evdev consumers. The still-present console event/input-mode/key-state
  ioctls remain explicitly deprecated rather than reported removed.
- Extended the [source-build guide](../../../docs/howto/build-from-source.md)
  with all three amd64 Variant labels and saved values, their positive and
  negative firmware behavior, the maintained six-cell command, the Intel Mac
  physical boundary and separate no-media `05ac:8406` reader limitation, the
  exact Noct 2.0.1 identity, LLVM 23.1.0 `rev-0` cache, both x86 sysroots, and
  host-Noct versus target-Noct ownership.
- Normalized every product-document banner to the documented `current`,
  `experimental`, `deprecated`, or `planned` vocabulary and added navigation
  for the WLAN reference. The manually blocked μITRON design remains `planned`
  and states its hold instead of appearing current.
- Added the repeatable DOC-T70--T72 Noct reconciliation check. The relative-link
  validator now excludes only ignored disposable `temp/` and prohibited
  `.internal/` trees; while validating the permanent tree it found three q014
  links to a no-longer-present generated image, which were converted to truthful
  historical artifact names rather than live links.

## Evidence

The following commands passed from the repository root:

```sh
build/NoctLang/build-static/noct \
  plan/ws009-documentation/tests/check-completed-producer-docs.noct .
# DOC-T70--T72 completed-producer documentation: PASS (11 status banners)

build/NoctLang/build-static/noct \
  plan/ws009-documentation/tests/check-relative-links.noct docs plan
# Markdown relative-link check: PASS (1803 links)

git diff --check
```

No producer source, credential, image, runtime behavior, or existing
`config.mk` was changed. The already accepted q048, q063, q064, q071, and WS020
runtime/hardware evidence was documented rather than rerun.
