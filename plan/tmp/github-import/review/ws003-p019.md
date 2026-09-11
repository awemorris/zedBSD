# WS003 Phase 019: Latitude NVMe native installation and boot

Last updated: 2026-08-29

Phase ID: `ws003-p019`

Status: future; not designed; not ready for a Queue

Parent: [WS003 real-hardware bring-up](../ws.md)

Tests: [WS003 test index](../tests/README.md)

## Objective

Accept the later native-root installer path on the Latitude without reopening
or weakening the completed existing-FAT overlay milestone.

## Deferred decisions

- select-existing versus create/format native filesystem;
- whole-disk GPT layout and destructive confirmation, if offered;
- UFS provisioning/growth and recovery;
- native `zedbsd.cfg` with `kernel=` and exact
  `rootpart=PARTUUID=...` selection;
- firmware Boot-entry policy after experience with p018 fallback boot.

## Entry condition

Do not detail or Queue this Phase until WS019 p006/p007 have separately fixed
and accepted the destructive/native product and safety contracts in QEMU.

## Completion direction

An explicitly selected native root boots through the installed UEFI loader to
init/login/root shell, with the final physical repeatability gate stated by the
later accepted WS019 contract.
