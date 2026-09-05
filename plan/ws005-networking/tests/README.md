# WS005 shared test cases

Parent: [WS005](../ws.md)

| Case ID | Environment | Required observation |
| --- | --- | --- |
| NET-T00 | Regression | WS002 `networkd` fd 3 readiness, synchronous `net`, `dhcpc`, and direct-ifconfig recovery remain passing |
| NET-T10 | Physical wired/USB | Static and DHCP configuration, route/DNS output, transfer, restart, and degraded failure pass |
| NET-T20 | Host ZNV2 protocol | Exact-length/split/limit/error framing passes at the 32-byte header and 4096/32768-byte request/response bounds for all wired and Wi-Fi operations; callers migrate together, the V1 parser/executor is deleted, and legacy magic can only receive a bounded unsupported-version error |
| NET-T21 | wifi.conf profile | `set-key` selects `/etc/wifi.conf` or its euid account's passwd-home `.wifi.conf` without trusting `HOME`; omission means manual and an explicit `manual` operand is rejected; permission, parsing, escaping, atomic rewrite, corruption recovery, maxima, wiping, and secret-redaction cases pass; the q071 daemon reader independently derives the active owner's same fixed path from `SO_PEERCRED` rather than accepting a caller path |
| NET-T22 | AF_UNIX admission/authentication | The p003 peer snapshot and lifecycle regressions pass; `root:network` GID 69 mode `0660`, readiness ordering, unauthorized/admitted peers, root/all versus nonroot/read-only-plus-WLAN authorization, fd passing, and peer close are covered |
| NET-T23 | Primitive wifi child contract | `WIFI1` scan/status/list/connect/disconnect records, secret fd 4, 32768-byte/64-record stdout, 512-byte stderr, blocked output, 15/30-second stage deadline, cancel, crash, one-second termination grace, kill/reap, USB removal, malformed records, and no-secret-echo cases pass |
| NET-T24 | Direct wifi command and bounded connect UX | Production-ioctl fixtures pass the original direct-root sequence plus prompt asynchronous scan/connect generations, one userspace-owned command-wide 30-second scan/select/connect retry deadline, ordered nonsecret progress, retryable/fatal classification, idempotent administrative up/down, global quiet output suppression, bounds, and secret erasure without DHCP, persistence, or hidden kernel reconnect |
| NET-T30 | WLAN orchestration fixture | Stateless `net` to networkd to production-contract `ifconfig`/`wifi`/`dhcpc` child doubles pass the six interface-free forms, local set-key plus empty profiles-changed, file-order then stable-radio auto/manual selection, exactly one connection, prospective-owner store/enumeration validation before mutation, valid zero-radio auto-searching/hotplug wait, partial multi-radio preparation with usable radios retaining stable order, 10-second DHCP/90-second total, cancellation, unchanged pre-mutation failure, manual-disconnected with radios still up/scanning, degraded retirement, and the bounded successful-session handoff for p011 without claiming hardware |
| NET-T31 | Archer identity subrecord | Within NET-T32, not as another physical gate, the Japan-market unit labelled `Archer T3U Nano` with no printed revision matches the authoritative p026 `2357:012e`/RTL8822BU `bcdDevice`/interface/endpoint profile and pinned firmware; RTL8828BU, another unit, or another descriptor profile is not inferred |
| NET-T32 | Single combined provisional checkpoint | Only after every automatic gate passes, one candidate-artifact action produces p028 identity/firmware/scan, p029 WPA2/CCMP L2, retained p030 lifecycle, p041 W52, p044 link-event/no-kernel-retry, and p008/p011 orchestration subrecords through attach, secure enable/DHCP, one successful same-SSID 30-second userspace recovery, one terminal recovery failure with cleanup to auto-searching, transfer, explicit disconnect with scans retained, disable, removal, and cleanup without a rerun or code/config change |
| NET-T33 | Frozen final batch | The unchanged W52 artifact and environment pass five consecutive complete runs, including exactly one event-triggered p011 `/sbin/wifi` recovery child per transient loss, after one batch request; no run/command rerun, artifact change, manual intervention, profile switch, or human gate occurs between runs |
| NET-T34 | Minimum physical WLAN development check | Before lifecycle hardening, one runtime-only-credential run reaches exact-adapter attach, scan, WPA2/CCMP carrier, DHCP, local/external ping, bounded fetch, and disconnect; it is not p008 acceptance evidence |
| NET-T35 | Primitive abnormal-path hardening | After NET-T34 and p044, exhaustive public CLI boundaries, asynchronous scan/connect generations, one 30-second userspace retry sequence, terminal errors, cancellation, races, detach, cleanup, output limits, and secret-redaction variants pass without restoring kernel high-level reconnect or taking ownership from p006/p007/p011 |
| NET-T36 | Managed WLAN link recovery | Startup publishes fd-3 readiness only after every detected WLAN completes disconnect/search-stop/down in disabled, and normal exit repeats the normalization or exits nonzero; a successful p007 connection installs one bounded nonsecret session; one matching kernel link-down event enters internal reconnecting and starts exactly one ordinary p010 `/sbin/wifi` child for the same selected SSID; success preserves coherent L3 ownership, while child failure performs disconnect/L3 cleanup, wipes the secret, and settles in auto-searching without a nested retry; explicit disconnect leaves radios up/scanning in manual-disconnected, and disable/replacement/shutdown/removal remain identity-safe |
| NET-T40 | USB CDC Ethernet | If selected, ECM/NCM interoperability, reconnect, DHCP/static, and transfer pass with device role proven |
| NET-T41 | RTL8156 first data path | After p020 and the safe automatic fixes, one final candidate gets one combined Latitude acceptance: first notification/bulk RX remain responsive, fixed-peer ARP/ping passes, and DHCP lease plus post-lease ping passes; failure retains the exact first stage and native-controller boundary |
| NET-T42 | QEMU CDC ECM control | In a separately approved Queue, QEMU `usb-net` selects ECM, publishes `ue0`, and passes carrier, static ARP/ping, DHCP, and post-lease ping in IDE-control and concurrent xHCI USB-storage topologies; physical NCM interoperability means this is no longer the automatic response to NET-T41 failure |
| NET-T43 | DHCP transition and diagnostics | Starting with no address or a static address, the old interface default is absent before DISCOVER, IPv4 source and BOOTP `ciaddr` are zero, every failed stage restores the exact prior interface/default state, success commits only the lease, and `ENETDOWN` is distinguishable from `ETIMEDOUT` |
| NET-T44 | Bound limited broadcast follow-up | With another interface's default route present, `SO_BINDTODEVICE` DHCP limited broadcast keeps destination/next-hop `255.255.255.255` on the bound interface; direct concurrent DHCP route mutation is serialized or ownership-safe before this case is claimed |

Existing executable Phase 20 and DHCP tests remain under repository `/tests`
and are cross-owned as regression inputs rather than duplicated.

NET-T21 has one host-side parser/store gate:

```sh
sh plan/ws005-networking/tests/run-wifi-conf-store-test.sh
```

It exercises the strict v1 model, canonical serialization, checked
same-directory store operations, injected publication failures, lock timing,
concurrency, and redaction in ordinary, ASan+UBSan, and compiler-analyzer
builds.  Native root/non-root ownership and directory-durability acceptance
uses the actual `/sbin/net wifi set-key` command through:

```sh
make -j16 toolchain
build/NoctLang/build-static/noct --path=tools/build \
  plan/ws005-networking/tests/wifi-credential-native-qemu.noct \
  "$PWD" /tmp/ws005-p005-native-evidence
```

That private amd64/UEFI fixture runs root, sudo-like, effective-user, and
ordinary-user command invocations without a shell. It checks exact owner,
group, mode, persistent-lock inode, rename replacement, temporary cleanup,
ignored `HOME`, unchanged `net.conf`, and absent `networkd`, then issues QMP
`quit` immediately after the first PASS and validates both stores after a
second boot of the same overlay writable medium. Synthetic passphrases are
captured inside the guest, rejected from every retained text stream, and the
mode-0600 writable medium is deleted on success. The source image remains
immutable. Its kernel VFS prerequisites `ws001-p022` and `ws001-p023`
completed in q050.

NET-T22 has three reproducible executable entry points:

```sh
plan/ws005-networking/tests/run-networkd-auth-test.sh
plan/ws005-networking/tests/run-peercred-native-qemu.sh
plan/ws005-networking/tests/run-inet-ioctl-authorization-test.sh
```

`run-networkd-auth-test.sh` builds and executes the production-source
`networkd` publication and authorization fixture in ordinary, ASan+UBSan, and
compiler-analyzer variants.  Its temporary build directory defaults to the
project-local `plan/ws005-networking/temp` directory and honors `TMPDIR` when
the caller supplies one.

`run-peercred-native-qemu.sh` builds the test-only PC-98 image and proves the
native AF_UNIX peer snapshot together with `networkd` readiness, root/all and
non-root/SHOW-only decisions, and the final `init: system running` gate.

`run-inet-ioctl-authorization-test.sh` exercises NET-T22's direct-kernel
authorization boundary.  It proves that every current route and interface
mutation is rejected before argument access for a non-root caller, that root
reaches the ordinary validation path, that the explicit query set remains
available, and that future unknown/private commands default to the privileged
side of the boundary.

NET-T24 has one focused command-level entry point:

```sh
plan/ws005-networking/tests/run-wifi-command-test.sh
```

It includes the production `wifi` command with deterministic socket, ioctl,
clock, sleep, and output doubles.  The fixture proves the exact six-command
normal sequence and public WLAN request records, basic empty/oversize rejection
before ioctl, and mutable argv/request secret clearing without relinking the
lower WLAN, WPA, or driver stack.  A dedicated terminal-timeout scenario
proves that a failed connect prints the redacted public stage, retry, and
error summary before its terminal message and still cancels the admitted
attempt without leaking the credential.  A terminal/pipe pair of
scenarios proves that the scan countdown refreshes one line in place with
`\r` only on an interactive terminal and emits one line per refresh
otherwise.
