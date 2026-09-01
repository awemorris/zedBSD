# Queue: RTL8822BU minimum end-to-end connectivity

Last updated: 2026-09-01

QID: `q059`

Queue status: completed

Queue finished: **Yes**

Authorization: the user changed the implementation order on 2026-09-01.
Establish one simple successful communication path before perfecting detailed
error, recovery, race, and long-run behavior. Q058 already supplies the
automatic WPA2-Personal/CCMP kernel and RTL8822BU L2 foundation.

Timebox: none. Execute the normal path only. When a failure occurs, diagnose
and repair the first boundary blocking that path; do not broaden the Queue
into speculative abnormal-case hardening.

Parent: [master plan](master.md)

Previous Queue: [q058](queue-q058.md)

## Purpose

Provide the shortest useful vertical slice:

```text
RTL8822BU attach and firmware
  -> wlan0
  -> /sbin/ifconfig wlan0 up
  -> /sbin/wifi scan and list
  -> WPA2-Personal/CCMP connect
  -> carrier
  -> /sbin/dhcpc
  -> ping and bounded fetch
```

This Queue deliberately precedes rekey, automatic reconnect, firmware crash
recovery, repeated hotplug, exhaustive malformed input, exhaustive race/fault
injection, and long repeatability campaigns.

## Execution registry

| Priority | WS / Phase | Authoritative document | Status | Required result |
| --- | --- | --- | --- | --- |
| 1 | `ws005-p004` | [Primitive wifi command](ws005-networking/phase004-wifi-ioctl-command/phase.md) | completed (`q059`) | The direct root `/sbin/wifi` human command drives the existing scan/status/connect/disconnect WLAN ioctls on the normal path |
| 2 | `ws005-p009` | [Minimum connectivity](ws005-networking/phase009-wlan-minimum-connectivity/phase.md) | completed (`q059`) | One physical adapter run reaches scan, secure carrier, DHCP, ping, and bounded fetch using runtime-only credentials |

## Accepted decisions

- Implement only the direct root human `/sbin/wifi` forms needed for initial
  bring-up. `--machine`, secret fd 4, ZNV2, `networkd`, profile lookup, and
  `net wifi` orchestration remain p006/p007 work after basic connectivity.
- Use the existing p027-p029 kernel UAPI and WPA2/CCMP implementation. Do not
  add rekey/reconnect/recovery APIs merely to complete this Queue.
- Apply essential bounds, checked return values, finite waits, and secret
  buffer clearing to the normal path. Defer exhaustive syntax/error matrices,
  every cancellation race, and every injected failure to later hardening.
- The physical check is one developmental success check, not final
  acceptance or repeatability evidence. It does not require five runs and
  does not induce link loss, unplug/reinsert, or firmware failure.
- The supplied SSID and passphrase are runtime-only inputs. Never write them
  to a plan, source, fixture, image, retained command line, diagnostic, test
  log, or commit.
- Do not use `.internal/` or aggregate `make check`.

## Automatic gate before physical use

1. `/sbin/wifi` builds and is installed in the amd64 image.
2. A production-ioctl fake WLAN smoke test passes exactly one normal sequence:
   search start, completed list, status, connect through authorized carrier,
   disconnect, and search stop.
3. Basic empty/oversized interface, SSID, and passphrase inputs fail before an
   ioctl, output stays bounded, and every userspace secret buffer is erased.
4. Retain q058 as the accepted lower-layer baseline. Rerun only directly
   affected focused gates if a normal-path repair touches that layer, plus the
   driver-enabled amd64 image build, one bounded xHCI USB-root boot, and
   `git diff --check`.

## Physical development check

Use one candidate image and the already available passthrough environment.
Supply the controlled AP identity and passphrase only at runtime without
retaining them. Perform one bounded sequence:

1. Attach the exact p026 adapter, require one `wlan0` with the pinned
   firmware, and run `/sbin/ifconfig wlan0 up`.
2. Start a scan, wait for completion, and confirm the controlled WPA2/CCMP BSS
   appears.
3. Connect with `/sbin/wifi`; require authorized carrier.
4. Run `/sbin/dhcpc wlan0`; require an IPv4 address and route.
5. Ping the local gateway and one external address, then fetch one bounded
   object and validate nonzero/matching expected length or digest.
6. Disconnect once, confirm carrier clears, and bring the interface down.

If the sequence fails, retain only redacted first-boundary evidence and return
to that normal-path implementation. Do not add unrelated retry/recovery logic
in response.

## Completion definition

Q059 completes when p004's minimum direct command and p009's single physical
development check prove the complete vertical path through useful IP
communication. Completion then permits the normal `networkd`/`net wifi`
composition work and the separately planned p030/error-hardening work. It
does not claim rekey, automatic reconnect, hotplug recovery, exhaustive fault
behavior, or final five-run acceptance.

## Execution result

Completed on 2026-09-01. The final candidate was
`build/amd64/hdd-image.img`, 203423744 bytes, SHA-256
`6d0ec924f0d063b663c6da8ab1a7b8b39bcef8b2b2849e4fb0d8308340f9ed75`.

p004 added and focused-tested all six direct human `/sbin/wifi` forms. The
normal-path repair also accepts the authenticated RSNXE element (`id=244`) in
WPA2 message 3 and advertises all twelve implemented legacy rates so that the
fixed 6-Mbit/s EAPOL/data path is consistent with association capabilities.

One QEMU/KVM `-cpu host` run with xHCI USB passthrough then published `wlan0`,
completed a scan, reached WPA2-Personal/CCMP authorized carrier, obtained DHCP
configuration, passed both gateway and public ping at 3/3 replies with 0%
loss, fetched an 84255-byte HTTP object, and completed disconnect plus
administrative down without an observed hang. Runtime credentials and network
identity were not retained in the repository or evidence.

This is the single p009 development checkpoint only. It does not complete
p008's final five-run acceptance, p006/p007 `networkd`/`net wifi`
composition, p010 primitive abnormal/semi-normal hardening, or WS004 p030
reconnect, hotplug, recovery, and lifecycle hardening. Those remain eligible
next work rather than hidden q059 scope.
