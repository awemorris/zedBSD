# WS005 shared test cases

Parent: [WS005](../ws.md)

| Case ID | Environment | Required observation |
| --- | --- | --- |
| NET-T00 | Regression | WS002 `networkd` fd 3 readiness, synchronous `net`, `dhcpc`, and direct-ifconfig recovery remain passing |
| NET-T10 | Physical wired/USB | Static and DHCP configuration, route/DNS output, transfer, restart, and degraded failure pass |
| NET-T20 | Host ZNV2 protocol | Exact-length/split/limit/error framing passes at the 32-byte header and 4096/32768-byte request/response bounds for all wired and Wi-Fi operations; callers migrate together, the V1 parser/executor is deleted, and legacy magic can only receive a bounded unsupported-version error |
| NET-T21 | wifi.conf profile | `net` selects `/etc/wifi.conf` or its euid account's `~/.wifi.conf` without trusting `HOME`; permission, parsing, escaping, atomic rewrite, corruption recovery, maxima, wiping, and secret-redaction cases pass, and networkd never opens a profile |
| NET-T22 | AF_UNIX admission/authentication | The p003 peer snapshot and lifecycle regressions pass; `root:network` GID 69 mode `0660`, readiness ordering, unauthorized/admitted peers, root/all versus nonroot/read-only-plus-WLAN authorization, fd passing, and peer close are covered |
| NET-T23 | Primitive wifi child contract | `WIFI1` scan/status/list/connect/disconnect records, secret fd 4, 32768-byte/64-record stdout, 512-byte stderr, blocked output, 15/30-second stage deadline, cancel, crash, one-second termination grace, kill/reap, USB removal, malformed records, and no-secret-echo cases pass |
| NET-T30 | WLAN orchestration fixture | `net` to networkd to production-contract `ifconfig`/`wifi`/`dhcpc` child doubles passes search start/stop, list, local set-key, up/down/connect, file-order auto selection with four attempts, 10-second DHCP/90-second total, cancellation, unchanged pre-mutation failure, fixed admin-up/disconnected/search-stopped/no-owned-L3 fail-clean, degraded retirement, and same-network-only bounded reconnect without claiming hardware |
| NET-T31 | Archer identity subrecord | Within NET-T32, not as another physical gate, the Japan-market unit labelled `Archer T3U Nano` with no printed revision matches the authoritative p026 `2357:012e`/RTL8822BU `bcdDevice`/interface/endpoint profile and pinned firmware; RTL8828BU, another unit, or another descriptor profile is not inferred |
| NET-T32 | Single combined provisional checkpoint | Only after every automatic gate passes, one candidate-artifact action produces p028 identity/firmware/scan, p029 WPA2/CCMP L2, p030 reconnect/lifecycle, and p008 DHCP/orchestration subrecords through attach, search/list/stop, secure up, DHCP, bounded reconnect/transfer, down, removal, and recovery without retry or code/config change |
| NET-T33 | Frozen final batch | The unchanged artifact and environment pass five consecutive complete runs, including the frozen reconnect oracle, after one batch request; no retry, artifact change, manual intervention, profile switch, or human gate occurs between runs |
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
