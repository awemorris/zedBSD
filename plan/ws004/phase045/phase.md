# WS004 Phase 045: Archer T3U Plus feasibility

Last updated: 2026-09-06

Phase ID: `ws004-p045`

Status: Complete (user-requested feasibility investigation; no implementation Queue)

Parent: [WS004](../ws.md)

Evidence: [HW-T43 intake and Linux probe](../tests/archer-t3u-plus-intake.md)

## Objective and authorization

Determine whether the TP-Link Archer T3U Plus attached to `awe@10.0.10.25`
can use the existing RTL8822BU implementation and the optional
`userland/firmware/rtl8822b/` GitHub firmware package. The user explicitly
authorized operations on that host and requested feasibility first.

This investigation covers USB inventory, upstream comparison, existing-code
review, fresh acquisition through the current package recipe, and a reversible
Linux firmware/probe/passive-scan diagnostic. It changes no production driver,
firmware pin, WLAN API, image, or execution Queue. The old Nano p026/p028/p036
identity and accepted evidence remain historical contracts for `2357:012e`.

## Result

The extension is feasible in principle. The exact device is `2357:0138`.
Linux's standard `rtw88_8822bu` accepts the same pinned 161240-byte firmware
already used by zedBSD, initializes the interface, and passively detects eight
BSSs including 2.4 GHz and channel 44. The observed original Debian failure
was missing `rtw88/rtw8822b_fw.bin` (`ENOENT`, -2), with no installed
`firmware-realtek` package; it was not an observed firmware incompatibility.

The Linux probe also revealed a material transport difference: the device
starts at High Speed with `bcdUSB/bcdDevice=2.10`, and Linux's default USB-mode
switch re-enumerates it at SuperSpeed with both revisions `3.00`. The existing
zedBSD driver accepts only Nano `012e`, High Speed and 512-byte bulk endpoints.
It also programs a fixed High-Speed RXDMA profile. A PID addition alone cannot
support the currently enumerated SuperSpeed adapter.

## Verification and limits

Completed evidence:

1. Both observed USB descriptor profiles, including SuperSpeed companion bytes.
2. Exact host/kernel and original missing-file diagnostics.
3. Fresh GitHub download with the production package's size/hash checks.
4. Standard Linux probe with firmware `30.20.0`, H2C `14`, and one successful
   passive scan, with the new interface unmanaged and unassociated.
5. Restoration of temporary firmware search path, udev policy and files;
   adapter unbound and management routes unchanged.

The adapter remains powered in its observed SuperSpeed mode after the Linux
switch. This is explicitly different from its initial High-Speed state.
Linux was not rebooted and no permanent firmware/package/configuration change
was made. The development host's wired management link remained available.

The installed kernel disables `CONFIG_RTW88_DEBUG` and `CONFIG_RTW88_DEBUGFS`.
Exact silicon cut, RFE option, EFUSE country/channel plan and calibration bytes
were not read. Successful Linux operation supports reuse of the chip family,
but does not establish zedBSD's stricter W52 eligibility. No zedBSD boot,
authentication, IP data transfer, throughput or long-duration result is claimed.

## Follow-up

[P046](../phase046/phase.md) records the bounded driver
extension and verification path. Preserve the existing firmware pin first;
support the two measured USB profiles, propagate speed privately to RXDMA,
check applicable USB PHY setup and packet-boundary behavior, then observe the
board through zedBSD's existing EFUSE parser before radio acceptance.
