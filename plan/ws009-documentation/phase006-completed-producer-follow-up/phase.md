# WS009 Phase 006: completed-producer documentation follow-up

Last updated: 2026-09-05

WSID: `ws009`

Phase ID: `p006`

Combined ID: `ws009-p006`

Status: Queue-ready; highest current priority

Parent: [WS009](../ws.md)

Tests: [WS009 test index](../tests/README.md)

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
