# WS005 Phase 009: minimum physical WLAN connectivity

Last updated: 2026-09-01

WSID: `ws005`

Phase ID: `p009`

Combined ID: `ws005-p009`

Status: completed (`q059`)

Parent: [WS005 networking and WLAN](../ws.md)

Tests: [WS005 test index](../tests/README.md)

## Objective

Prove one simple physical end-to-end communication path before expanding WLAN
error handling. Use the direct root primitives to reach:

```text
RTL8822BU attach -> wlan0 -> ifconfig up -> scan -> WPA2/CCMP carrier
                 -> dhcpc -> ping -> bounded fetch
```

This is a development checkpoint, not final acceptance or repeatability
evidence.

## Dependencies

- WS004 p026-p029: exact adapter/firmware, common WLAN UAPI, scan, and secure
  Ethernet L2 automatic milestones.
- WS005 p004: the minimum direct `/sbin/wifi` command.
- Existing `/sbin/dhcpc`, `ping`, and `fetch` paths.
- The approved USB-passthrough test environment and a controlled
  WPA2-Personal/CCMP access point.

WS004 p030, WS005 p006/p007, and the final p008 campaign are not prerequisites.
That ordering is the purpose of this Phase.

## Credential handling

The controlled SSID and passphrase are supplied only at execution time. They
must not be written to a plan, source, fixture, disk image, retained command,
diagnostic, test log, or commit. Retained evidence names only redacted network
identity and the first public failing boundary.

## Procedure

Use one freshly built amd64 image containing `/sbin/wifi` and the separately
selected pinned firmware package:

1. Attach the exact p026 adapter, require exactly one usable `wlan0`, and run
   `/sbin/ifconfig wlan0 up` so the radio and firmware are started.
2. Start a scan, wait within the existing finite scan budget, list the
   completed snapshot, and find the controlled WPA2/CCMP network.
3. Connect once with `/sbin/wifi` and require authorized carrier.
4. Run `/sbin/dhcpc wlan0` and require a usable IPv4 address and route.
5. Ping the local gateway and one external address.
6. Fetch one bounded object and verify a nonzero expected size or digest.
7. Disconnect once, confirm carrier is down, and run
   `/sbin/ifconfig wlan0 down`.

The run is bounded and performed once. It does not deliberately cause link
loss, rekey, firmware failure, device removal, concurrent stress, or a second
boot.

## Failure handling

On failure, record the first boundary only: attach, firmware, scan, BSS
selection, association, four-way handshake, carrier, DHCP, ping, or fetch.
Repair the simplest cause blocking that same normal path and rerun this single
checkpoint when a new candidate is ready. Do not respond by adding unrelated
retry, recovery, or comprehensive fault machinery.

## Completion conditions

- One exact candidate reaches scan, WPA2/CCMP authorized carrier, DHCP, local
  and external ping, and one bounded fetch.
- Disconnect lowers carrier without an obvious hang or secret-bearing output.
- The candidate image and nonsecret build/firmware identity are retained.
- The result is explicitly labelled a one-run development checkpoint and is
  not reused as p008's five-run final acceptance ledger.

Completion unlocks p006/p007 functional composition and provides the working
baseline required before WS004 p030 and WS005 p010 hardening.

## Execution result

Completed in q059 on 2026-09-01 with
`build/amd64/hdd-image.img` (203423744 bytes, SHA-256
`6d0ec924f0d063b663c6da8ab1a7b8b39bcef8b2b2849e4fb0d8308340f9ed75`).
One QEMU/KVM `-cpu host` run with xHCI USB passthrough produced the following
bounded normal-path evidence:

- the RTL8822BU path attached and published `wlan0`;
- scanning completed and the controlled WPA2-Personal/CCMP network was found;
- association completed with authorized carrier;
- DHCP completed;
- gateway and public ping each returned 3/3 replies with 0% loss;
- one HTTP fetch produced 84255 bytes; and
- disconnect and administrative interface down completed without an observed
  hang.

The successful candidate includes authenticated message-3 RSNXE (`id=244`)
handling and advertises all twelve implemented legacy rates, including the
rate used by the fixed EAPOL/data transmit path. No credential, BSSID, MAC
address, assigned address, or other network identity is retained here.

This is exactly one development checkpoint. It is not p008's five consecutive
final runs and does not claim `networkd`/`net wifi` composition, rekey,
automatic reconnect, hotplug/recovery, repeated detach, or comprehensive
abnormal/semi-normal behavior. p006 and p010 are now dependency-ready; p007
follows p006, and all three remain incomplete.
