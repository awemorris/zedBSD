# Q074 confirmed-commit QEMU evidence

Last updated: 2026-09-05

Owner: [`ws011-p007`](../phase007-confirmed-commit-acceptance/phase.md)

Queue: [`q074`](../../queue-q074.md)

## Fixed environment

- QEMU: `QEMU emulator version 10.0.11 (Debian 1:10.0.11+ds-0+deb13u1)`
- machine: amd64 PC/AT `pc`, 512 MiB, four virtual CPUs
- NIC: ISA NE2000, I/O `0x300`, IRQ 10, synthetic MAC
  `52:54:00:11:07:01`
- network: QEMU user network with `restrict=on`; guest `10.0.2.15/24`,
  gateway `10.0.2.2`, and resolver `10.0.2.3`
- candidate addresses: timeout cell `10.0.2.16/24`; confirmation cell
  `10.0.2.17/24`
- real rollback wait: 70 seconds for the public one-minute minimum
- controller bounds: 120-second boot, 30-second command, and 420-second cell
  deadlines

The effective QEMU command was:

```sh
qemu-system-x86_64 -machine pc -m 512 -smp 4 \
  -drive file=DISPOSABLE.img,format=raw,if=ide \
  -netdev user,id=net0,restrict=on \
  -device ne2k_isa,netdev=net0,iobase=0x300,irq=10,mac=52:54:00:11:07:01 \
  -display none -serial none -debugcon file:GUEST.log -monitor stdio
```

The test-only hybrid source image was
`build/amd64/tests/ws011-p007-confirmed.img`, SHA-256
`3564e43244389656ae6a33da693cace95f2ebd3bd938e2c54f2190626082e649`.
The production `config.mk` remained
`6e30fa8c0b14d40bebc7d8c6f4bc6a0ebd547af36a426ffeb2972dc171063262`,
and the production `build/amd64/hdd-image.img` remained
`5e12781eb52127f5f56744298adfe16d79c27b34726916d1953c23617cc61ab9`.
The test config and installed synthetic `net.conf` were respectively
`57b259056c98bc309587120f942c89fb4ba6063f6af7b7f913694202e515ba4b`
and `c2ca62425df52d20b98b19db671d41c648749baf540e21a79837246d9400ff26`.

## NCOM-T020 result: pass

One fresh disposable cell booted with `10.0.2.15/24`, applied
`10.0.2.16/24` through `commit confirmed 1`, lost the originating `net`
client, and waited 70 real seconds. Networkd reported that the confirmed
rollback expired and ran. The final running address returned to
`10.0.2.15/24`.

All three observations reported the byte-identical startup checksum
`645868049 462 /etc/net.conf`. Before, during, and after the transaction the
default route was `10.0.2.2`, the resolver was `10.0.2.3`, and a one-packet
gateway ping passed. No guest or QEMU fatal diagnostic appeared.

The first version of the host post-validator stopped on an unset local shell
variable after the guest cell had completed. The retained guest evidence was
validated with the same address, route, resolver, ping, checksum, expiry, and
fatal-pattern predicates; the local-variable declaration was then corrected.
The product cell was not repeated.

## NCOM-T021 result: uncleared

A second fresh disposable cell booted with the same old checksum and usable
networking. In one interactive `net` session it applied `10.0.2.17/24` through
`commit confirmed 1`; `show startup-config` still showed the old
`10.0.2.15/24` intent. The subsequent ordinary `commit` caused ten admitted
networkd connections, matching the expected confirmed-check and nine
reconcile requests, but did not print `Commit complete.` or return the
configuration prompt within the fixed 30-second command deadline.

No confirmed-disarm request appeared, no second boot began, and no guest or
QEMU fatal diagnostic appeared before the controller terminated the cell
cleanly through the monitor. Authentication is logged before request dispatch,
so the tenth connection does not prove final DNS completion. The unobserved
interval includes final DNS handling/response/client close, subsequent
`netconf_save_atomic_locked()` publication, and DISARM connection setup.
Existing evidence cannot distinguish these stages without an instrumented
observation. The entire cell lasted less than one minute; the missing rollback
expiry is therefore not a timer failure. NCOM-T021 does not establish
publication, late-timer absence, or reboot persistence.

The p007 two-cell boundary is exhausted. No third QEMU cell was run. The
correction and a T021-only rerun are extracted to
[`ws011-p009`](../phase009-confirmed-commit-overlay-publication/phase.md).

## Non-QEMU final gates

After the two cells, all NCOM-T001--T012 parser, console, persistence, boot,
reconcile, confirmed-model, and interactive integration tests passed. The ZNV2
protocol runner and the Wi-Fi command, managed-WLAN, credential-store, and
Wi-Fi-child runners passed. Maintained amd64 and i386 PC/AT `net` and
`networkd` builds passed. Aggregate `make check` was not run.

Verbose logs remain locally below ignored directories
`plan/ws011-net-config/temp/q074.rNswdS/` and
`plan/ws011-net-config/temp/q074.nCiNxa/`; they are not planning artifacts.
