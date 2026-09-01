# WS004 Phase 027: generic WLAN UAPI, common core, and fake device

Last updated: 2026-09-01

Phase ID: `ws004-p027`

Status: complete (`q055`)

Parent: [WS004 hardware expansion](../ws.md)

Tests: [WS004 test index](../tests/README.md)

## Objective

Add one device-independent station-mode WLAN control and lifetime layer before
writing the RTL8822BU transport. Define the ioctl ABI used by the later
low-level `wifi` command, retain scan and connection state after that command
exits, and prove the state/error/lifetime contract with a deterministic fake
radio. The fake validates common behavior only; it is never evidence that QEMU
emulates an 802.11 radio.

This Phase does not add the `wifi` or `net wifi` user commands, a configuration
database, DHCP, Realtek register access, firmware upload, WPA cryptography, or
an encrypted data path. Those consumers and later layers must use this common
contract instead of introducing a second device-private control path.

## Dependencies

- `ws004-p012`: removable `net_device` references, carrier, poll, shutdown,
  and detach lifetime.
- `ws004-p015`: interface-scoped USB ownership is a later hardware consumer;
  the generic core must not assume a bus.
- `ws004-p026`: target capability and firmware boundary, including the rule
  that hardware identity never enters the generic ABI.
- Existing AF_INET socket ioctl routing and fixed-width zedBSD UAPI rules.

`p027` is independently host-testable and is the implementation Phase selected
by `q055` after p026's identity decision closed.

## Frozen public control ABI

WLAN control uses the existing network-interface ioctl path on an AF_INET
datagram socket. Each request starts with `ifr_name`, ABI version, structure
size, and zeroed reserved fields. The network layer resolves and references the
`net_device`, verifies that it advertises the WLAN class, and dispatches the
request to the common WLAN core. No `/dev/wlan*` character device and no
driver-specific Realtek ioctl is added.

The first ABI defines these symbolic operations; numeric values are allocated
once from a zedBSD WLAN-private ioctl range and are never reused:

| Operation | Semantics |
| --- | --- |
| `SIOCSWLANSCAN` | `START` creates a new scan generation or idempotently joins the current one when already scanning; it never replaces an active generation. `STOP` retires current production/callbacks before return. Stopping an idle scan is idempotent. |
| `SIOCGWLANSCAN` | Return current generation, running/complete/cancelled/failed state, terminal error, result count, and cache-change sequence. |
| `SIOCGWLANBSS` | Return exactly one BSS record by `{generation,index}`. A stale generation returns `ESTALE`; end of a valid snapshot returns `ENOENT`. |
| `SIOCSWLANCONNECT` | Copy one SSID and WPA2 passphrase into the persistent common-core request, choose a compatible BSS deterministically, and return a connection generation. The asynchronous WPA2 operation is implemented in `p029`. |
| `SIOCSWLANDISCONNECT` | Cancel pending scan/connect/reconnect work or disconnect the current BSS, clear authorization and carrier first, and scrub key material. It leaves generic `IFF_UP` unchanged. |
| `SIOCGWLANSTATUS` | Return administrative, scan, authentication, association, key, controlled-port, retry, BSSID/channel, and terminal-error state without returning a credential or key. |

Each symbol is defined with `_IOW`, `_IOR`, or `_IOWR` and its exact public
request type; no WLAN command uses a legacy size-zero literal. In particular,
start/connect/disconnect return a generation or terminal admission state and
therefore use an `_IOWR` record, while query records also encode their complete
input/output size. This is required because the current libc `ioctl()` wrapper
consumes the variadic argument for a new command only when its encoded size is
nonzero; an unknown size-zero WLAN number would otherwise silently pass a null
argument to the kernel. Direction, group, number, and encoded size are part of
the frozen ABI and wrong-size variants never alias a valid command.

The ABI is pointer-free. Scan listing is paged one fixed record per ioctl so
its layout is identical on i386 and amd64 and no nested user pointer remains
pinned across a state transition. Every structure uses fixed-width integers,
an explicit `version`/`size`, and reserved-zero input validation; output is
fully initialized before `copyout`.

SSID is an octet string, not a C string: `length` is `0..32` and the storage is
always 32 bytes. Display escaping belongs to the command. A BSS record contains
SSID, BSSID, 20-MHz channel and center frequency, signed RSSI in dBm, beacon
interval, age, capability bits, and normalized security flags. Raw information
elements and unbounded vendor data are not exposed in version 1. The cache is
bounded at 64 BSS records, deduplicated by BSSID, with a deterministic weakest/
oldest eviction rule and stable BSSID tie-break.

One scan generation has a 15-second total monotonic deadline. The common core,
not a chip driver, owns that deadline; every dwell, callback, cancellation, and
terminal publication uses the smaller of its local bound and the remaining
generation time. The fake connection state also enforces the frozen 30-second
total direct-L2-connect deadline even though p029 supplies the real WPA2 work.

The first connect form accepts an SSID plus an 8--63-octet WPA2 passphrase. It
does not accept a shell-formatted string, raw pointer, implicit NUL terminator,
or 64-hex PSK in version 1. `p029` rejects an empty/open, WPA/TKIP, WPA3/SAE,
802.1X, or PMF-required BSS honestly instead of silently downgrading it.

State-changing scan/connect/disconnect ioctls require a superuser credential;
snapshot and status reads do not expose secrets and may be read by an ordinary
caller. The WS005 `networkd` path can provide separately reviewed delegation
for `net wifi`; it must not weaken the direct ioctl check.

### INET dispatch and hot-unplug barrier

The existing `net_device_ops.ioctl` member is not currently reached by the
INET socket dispatcher. This Phase adds one central WLAN dispatcher rather
than letting a user pointer enter a chip driver:

1. Classify the exact size-encoded command and select its public structure
   size/direction. Reject an unknown group/number/direction/size before copy or
   device lookup.
2. Copy the complete input into an initialized kernel-local request of that
   command's exact type, validate version/size/reserved fields, then resolve
   `ifr_name` and acquire a live `net_device` reference.
3. Check WLAN capability and read-versus-mutate privilege centrally. Admit the
   operation through a generic active-ioctl counter/gate before calling the
   device/common-core hook with only the kernel buffer.
4. `net_device_gone()`, final close, and terminal shutdown first block new
   ioctls and join every admitted ioctl along with open/poll callbacks. A
   removal racing lookup/admission returns `ENODEV`; it cannot call freed
   `driver_data` or publish success from a removed object.
5. Retire the active-ioctl admission, fully initialize the output, copy it out
   only for a successful query/admission result, and release the device
   reference. The chip driver never calls `copyin`/`copyout` and never retains
   an ioctl stack/request pointer.

This gate is a common `net_device` lifetime extension and must retain the
existing wired behavior byte-for-byte. It is not acceptable to depend on the
ordinary registry reference alone: the current p012 barrier explicitly joins
open/close/poll, while the new blocking connect/status path also needs an
admitted-ioctl join.

## Common station state and ownership

The common core owns one persistent station object per WLAN `net_device` and a
monotonically increasing operation generation. Its externally visible states
are:

```text
DOWN -> IDLE -> SCANNING -> IDLE
                  |          |
                  +------> AUTHENTICATING -> ASSOCIATING -> FOUR_WAY
                                                        -> CONNECTED
any active state -> DISCONNECTING -> IDLE
any active state -> FAILED or REMOVED
```

`FOUR_WAY` and successful `CONNECTED` are completed by `p029`; `p027` builds
the transitions and fake events without claiming cryptographic association.
Administrative `IFF_UP`, scan activity, 802.11 association, and controlled-port
authorization remain separate facts. `IFF_RUNNING`/carrier becomes true only
after the controlled port is authorized, never after firmware load, scan,
authentication, or association alone.

The core owns:

- serialization of scan, connect, disconnect, close, and removal;
- scan generations, cache snapshots, strict beacon/probe-response and
  information-element bounds, and normalized security descriptions;
- bounded timers, retries, cancellation, and rejection of stale driver events;
- selection of one BSS by exact SSID, supported security, strongest RSSI, then
  lexicographically lowest BSSID;
- the authentication/association/WPA state slots used in later Phases;
- controlled-port and carrier ordering, Ethernet queue admission, status and
  redacted diagnostics; and
- final credential/key zeroization after disconnect, interface close, failed
  connect, detach, and terminal shutdown.

The hardware driver supplies bus and radio operations only: start/stop scan or
channel dwell, transmit a management/EAPOL/data frame, report received frames
and TX status, install/delete a key, and quiesce/reset. Every callback carries
the station generation that admitted it. The common core ignores a late event
from an old generation and releases its ownership exactly once.

## Concurrency and failure contract

- All control operations are callable from thread context. IRQ/USB completion
  callbacks enqueue bounded records and never run authentication or ioctl
  copyout while holding a bus lock.
- A scan cache update is committed before its sequence is published. A list
  either sees one complete generation or receives `ESTALE`; it never combines
  generations.
- Interface close first blocks new commands, lowers carrier, cancels timers,
  stops driver producers, drains queued callbacks, scrubs credentials/keys,
  and only then returns. A failed quiesce retains the complete object for a
  checked retry.
- Removal makes the interface unobservable through the existing
  `net_device_gone()` barrier. Blocked or polling users receive `ENODEV`, not a
  success result from freed state.
- Timeout, cancellation, radio error, unsupported security, authentication
  rejection, association rejection, and key failure remain distinct status
  reasons. Diagnostics contain SSID only in escaped/redacted form and never
  passphrase, PMK, PTK, GTK, nonce, MIC key, or packet-number material.
- Scan and direct-connect generations terminate within their respective
  15-second and 30-second total monotonic deadlines; a retry or child
  transition consumes the existing budget and never restarts it.
- No operation reports success from a stub. A real device before `p029` returns
  `EOPNOTSUPP` for secure connect while scan can be complete independently.

## Deterministic fake device

Add a phase-owned fixture that links the production WLAN common core to a fake
driver and explicit test clock. It provides scripted beacons, probe responses,
authentication/association responses, key outcomes, RX/TX completions, and
detach. It must cover at least:

- binary/hidden/maximum-length SSIDs, duplicate BSS updates, 64-entry eviction,
  malformed/truncated information elements, and unsupported security suites;
- start/idempotent-join/stop/idempotent-stop, completion versus stop, cache
  snapshot generation, stale list, and scan timeout;
- deterministic BSS selection and no match;
- every state transition, driver rejection, timeout, cancellation, retry, and
  late event from a retired generation;
- list/status concurrent with scan updates and connect/disconnect;
- close and detach during every active state, with a deliberately blocked
  callback and checked retry; and
- credential buffers filled with a test pattern and proven scrubbed on every
  terminal path. Test logs use synthetic credentials only.

An optional kernel-test configuration may expose the fake in QEMU for ioctl
round trips, but it is absent from ordinary images and cannot satisfy any
physical gate.

## Expected areas

- `include/uapi/zedbsd/wlan.h` for the versioned, pointer-free ABI;
- `include/kern/net/wlan.h` and `src/kern/net/wlan*.c` for common state, frame/
  IE codec, cache, timer, and driver callbacks;
- the existing network socket-ioctl router and `net_device` class/capability
  boundary; and
- `plan/ws004-hardware/tests/` for production-source host fixtures and any
  test-only fake-device glue.

These are planning paths, not implementation authorization.

## Verification gates

1. `HW-T30` passes ordinary, ASan/UBSan, and compiler-analyzer host variants
   against production common-core sources.
2. ABI layout/offset/size assertions pass on configured amd64 and i386 builds;
   all reserved, size, version, SSID-length, index, and generation errors are
   covered. Each symbol has nonzero encoded size, libc forwards its pointer,
   and wrong direction/size variants reject without reaching a device.
3. A test-only QEMU ioctl client, if added, reaches the fake device through the
   production socket/ioctl path and proves admitted-ioctl join plus `ENODEV` on
   lookup/admission/removal races. This remains a model gate.
4. `make -j16` and ordinary amd64 IDE and xHCI USB-root boots reach `login:`.
   `make check` and `.internal/` material are not used.

## Completion conditions

- The frozen WLAN ioctl ABI is pointer-free, versioned, bounds checked, and
  identical on i386/amd64.
- One common station object owns scan cache, state, generations, cancellation,
  carrier ordering, and sensitive-buffer retirement independently of a chip.
- The fake-device matrix passes without a leak, stale transition, partial
  snapshot, double completion, credential disclosure, or false hardware claim.
- Non-WLAN interfaces reject WLAN ioctls with `EOPNOTSUPP`, and existing wired
  interface, DHCP, route, hotplug, and shutdown regressions remain unchanged.

## Execution result

Q055 completed the frozen common boundary without a physical-radio claim. The
kernel now has one pointer-free WLAN UAPI with six exact size-encoded ioctls,
strict INET copy/privilege dispatch, an active-ioctl teardown barrier, bounded
scan snapshots and generations, normalized beacon/RSN parsing, persistent
station/connect state, total scan/connect deadlines, credential erasure, and
checked detach/shutdown ownership. The network worker retains a generation
counter plus a level-triggered WLAN work predicate so an edge wakeup cannot be
lost immediately before sleep.

The station owns a live net-device reference through successful detach or
shutdown finalization. Its attach contract explicitly requires the hardware
caller to serialize the full live-publication/attach interval against removal;
failed duplicate, stopping, and capacity admissions do not mutate carrier.
Driver radio callbacks run outside station locks, must return within a bounded
driver contract, and may not wait for network-worker progress. Production uses
the single kernel `clock_ticks()` domain; the explicit clock exists only in the
test attachment API.

The production-source `HW-T30` runner passes ordinary, ASan/UBSan, GCC analyzer,
and configured amd64/i386 ABI variants. The INET WLAN authorization dispatcher
passes twice, and the extended net-device, ARP, and INET hotplug fixtures pass.
Ordinary PC-98 `make -j16` plus forced amd64 and i386 builds pass; the i386
integration gate caught and removed one accidental direct compiler-atomic
runtime dependency. The final amd64 image has SHA-256
`b0409dad5d4dd3574cb4b4e9381ade59a7308e72cc6641b21cf7924fbad8f43f`.
Disposable four-CPU, 4-GiB OVMF q35 launches reached exact `login:` through
both explicit IDE and xHCI USB-only storage, with no fatal or storage-error
marker. `git diff --check` passes. P028 has been synchronized to the required
LIVE-publication then serialized-station-attach order.

## Reconsideration boundary

Stop and revise this Phase if a correct command requires a nested pointer ABI,
an event stream that cannot be represented by generation/status polling, an
unprivileged direct state-changing ioctl, or a hardware-specific field in the
public record. Do not add a Realtek-only ioctl, make the short-lived `wifi`
process own reconnect state, or expose raw firmware/802.11 structures merely to
avoid a common-core design.
