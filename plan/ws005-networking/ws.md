# WS005: networking and WLAN

Last updated: 2026-08-31

WSID: `ws005`

Status: active; q029 p001 completed the first physical USB-Ethernet path,
q040 completed p003's AF_UNIX/network authorization foundation, and q041
retained p005's host implementation while assigning two native VFS blockers

Parent: [master plan](../master.md)

Last verified result: `ws005-p003` publishes one authenticated
`root:network 0660` socket, returns immutable 12-byte AF_UNIX peer snapshots,
permits admitted non-root `SHOW` while denying mutation, and rejects direct
non-root mutating network ioctls.  Its focused, analyzer, sanitizer, full
build, and PC-98 native runtime gates pass.  The earlier RTL8156 carrier,
DHCP, ping, and external-fetch path remains passing.

Resume point: complete `ws001-p015` effective-credential object creation and
`ws001-p016` truthful directory `fsync`, then requeue `ws005-p005` for its
root/non-root native ownership and remount-durability cell.  It does not wait
for physical radio identity or the later generic WLAN UAPI.

Shared tests: [WS005 test index](tests/README.md)

## Phase registry

| Phase | Status | Result / resume point |
| --- | --- | --- |
| [`ws005-p001`](phase001-usb-ncm-physical-datapath/phase.md) | Complete (`q029`) | RTL8156 NCM carrier/static/DHCP/ping and final Latitude external fetch pass |
| [`ws005-p002`](phase002-wlan-v1-contract/phase.md) | Planned; not queued | Freeze the v1 control, security, scan, association, DHCP, cancellation, and ownership contracts; record every intentional exclusion |
| [`ws005-p003`](phase003-unix-peer-credentials/phase.md) | Complete (`q040`) | Fixed 12-byte connection-time AF_UNIX identity, checked `root:network 0660` publication, root/non-root operation policy, and kernel ioctl privilege boundary pass focused and native PC-98 gates |
| [`ws005-p004`](phase004-wifi-ioctl-command/phase.md) | Planned; depends on p002, p003, and `ws004-p027` | Add the primitive, L2-only `/sbin/wifi` ioctl command with bounded machine and human output |
| [`ws005-p005`](phase005-wifi-credential-store/phase.md) | Uncleared (`q041`); host implementation passes; depends on p002 plus `ws001-p015`/p016 | Requeue after native creation ownership and directory-sync semantics are repaired, then run root/non-root guest acceptance |
| [`ws005-p006`](phase006-networkd-wifi-protocol/phase.md) | Planned; depends on p003-p005 | Replace whitespace `ZNV1` limitations with bounded length-framed WLAN requests, peer authorization, and a secret-FD child path |
| [`ws005-p007`](phase007-net-wifi-orchestration/phase.md) | Planned; depends on p005-p006 and WS004 WLAN fixture | Implement the requested `net wifi` search/list/up/down/connect flow through `networkd` to `ifconfig`, `wifi`, and `dhcpc` |
| [`ws005-p008`](phase008-archer-physical-acceptance/phase.md) | Planned; depends on p007 and `ws004-p030` | Prove one complete physical scan/WPA2/DHCP/transfer/down path, then run the final frozen-artifact repeatability campaign |

`ws002-p020` remains historical ownership of the current wired
`networkd`/`net` baseline; it is not renumbered into this WS. Native device and
common kernel WLAN work is owned by `ws004-p026` through `ws004-p030`.

## Goals

- Preserve the completed wired and USB-Ethernet behavior while adding one
  coherent WLAN control path.
- Make `/sbin/net` the interface used by people and desktop software.
- Keep `networkd` a privileged, lightweight command orchestrator whose fixed
  primitive backends are `/sbin/ifconfig`, `/sbin/wifi`, and `/sbin/dhcpc`.
- Keep `/sbin/wifi` a thin WLAN-ioctl command: no persistence, DHCP, routing,
  DNS, interactive database, or resident daemon role.
- Use one authenticated Unix-domain control socket for root and authorized
  ordinary users; never trust an identity carried in the request payload.
- Bring up TP-Link Archer T3U Nano V1.0 as the first WLAN target. Public
  primary evidence identifies it as RTL8822BU, USB `2357:012e`; the exact
  physical unit remains authoritative and must be recorded before driver
  implementation.

## WS completion conditions

WS005 is complete when the retained wired path and the selected WLAN path
operate through `net` -> `networkd` -> fixed primitive children; peer
authorization is kernel-attested; system and per-user credentials satisfy the
documented ownership, atomicity, and redaction rules; scan, explicit and auto
association, DHCP, disconnect, cancellation, recovery, and useful diagnostics
pass the automatic fixture matrix; and the selected physical adapter passes
the declared final acceptance campaign. Direct primitive recovery remains
available to root without making `networkd` optional for ordinary users.

## 1. Objective and ownership boundary

The authoritative control topology is:

```text
person / desktop environment
            |
            v
        /sbin/net
            |
            v
       networkd  -- one authenticated AF_UNIX socket
        |   |   |
        |   |   +-- /sbin/dhcpc     (IPv4 lease, route, resolver)
        |   +------ /sbin/wifi      (WLAN ioctl, L2 only)
        +---------- /sbin/ifconfig  (link/address primitive)
```

`networkd` does not implement radio protocols and `/sbin/wifi` does not become
a resident backend. The common kernel WLAN layer owns long-lived scan,
authentication, association, key/rekey, controlled-port, and disconnect state
needed after a one-shot `wifi` process exits. A chip driver owns only the
hardware/firmware-specific radio, management/data transport, channel, key-slot,
and lifecycle operations exposed through that common layer.

This design supersedes, rather than preserves, the earlier resident
`/sbin/wpa` child, pluggable stdin/stdout backend, and `/etc/wpa/` database.
Those concepts have no compatibility requirement because they were never a
released public contract.

## 2. Scope and non-goals

Initial scope includes:

- a versioned WLAN UAPI and fake-device/state-machine fixture;
- finite and cancellable scan with a bounded snapshot and generation number;
- explicit association and disconnection;
- WPA2-Personal PSK with CCMP, including 4-way/group-key handling, rekey, and
  controlled-port state;
- one TP-Link Archer T3U Nano V1.0 / RTL8822BU USB implementation;
- root and per-user plaintext profiles with an `auto` flag;
- high-level `net wifi` orchestration through DHCP; and
- truthful hot-unplug, timeout, and failure reporting.

The first usable radio milestone is restricted to station-mode, non-DFS
2.4 GHz, 20 MHz. WEP, WPA/WPA1, WPA3/SAE, 802.1X/EAP, AP mode, ad-hoc mode,
monitor mode, roaming,
power-save optimization, DFS, regulatory certification claims, throughput
tuning, and the later built-in RTL8822CE target are outside the first
acceptance. They require separately planned follow-ups rather than silent
expansion of the initial driver.

## 3. Fixed control and authorization decisions

1. `networkd` listens on the single `/run/networkd.sock`. A second root/user
   socket is not part of v1.
2. AF_UNIX snapshots the connecting process's PID, effective UID, and effective
   GID at connection time. `networkd` retrieves the kernel-attested record with
   `getsockopt(SOL_SOCKET, SO_PEERCRED, ...)` immediately after `accept`.
3. The peer-credential UAPI uses a public fixed-width record distinct from the
   kernel-internal `struct ucred`. It is valid only for connected AF_UNIX
   endpoints; unsupported/unconnected uses fail explicitly.
4. The socket is owned by `root:network` and has mode `0660`. The initial base
   account database assigns the FreeBSD-compatible GID 69 to `network`, while
   `networkd` resolves the name rather than hard-coding the number. Root and
   members admitted by that group may connect. Root-only operations still
   check the attested effective UID; request fields can never elevate access.
5. Mutable WLAN ioctls are privileged in the kernel. Authorized ordinary users
   reach them only through root `networkd`; a set-user-ID `/sbin/wifi` is not
   introduced. The same Phase audits existing mutable interface/route ioctls
   and records any broader network-admin defect instead of claiming the socket
   alone is a complete boundary.
6. `net wifi set-key` updates the caller's own file locally and normally does
   not contact `networkd`. Root means effective UID zero and selects
   `/etc/wifi.conf`; another effective UID selects that account's
   `~/.wifi.conf`. Consequently `sudo net ...` intentionally uses the root
   policy.
7. `networkd` never accepts `HOME`, UID, GID, or a credential-file pathname as
   proof of identity. `net` reads the selected profile and sends bounded values
   over the authenticated socket.

The euid-selected file is the official `net` policy, not a per-user secret
isolation boundary. Because `networkd` receives values rather than opening the
file itself, an admitted `network`-group member can construct a valid request
with another bounded SSID/passphrase. Membership therefore grants the declared
WLAN administration authority. Stronger seat/user-scoped credential isolation
would require a separately designed broker and is outside v1.

The `network` group is the v1 local authorization policy. A later seat/session
manager may replace group membership with active-console authorization, but
that is not required to implement the first desktop path.

## 4. Command contracts

The primitive command remains directly usable by root:

```text
wifi <interface> search start
wifi <interface> search stop
wifi <interface> list
wifi <interface> status
wifi <interface> connect <SSID> <passphrase>
wifi <interface> disconnect
```

It performs no DHCP or persistence. Human output is separate from a bounded
machine-readable mode used by `networkd`. The internal child form accepts key
material from a dedicated inherited descriptor; `networkd` never places a
passphrase in child argv, temporary diagnostic files, or logs. The requested
public connect form is retained, with its shell-history/process-list exposure
documented as a direct-administration limitation.

The public high-level commands are:

```text
net wifi search start <interface>
net wifi search stop <interface>
net wifi list <interface>
net wifi set-key <SSID> <passphrase> [auto]
net wifi up <interface>
net wifi down <interface>
net wifi connect <interface> <SSID>
```

`net wifi list` is required because people and desktop software use `net`, not
the primitive `wifi` command. The existing interactive `net` console exposes
the same operations without a separate device-management mode.

## 5. Operation semantics

- `search start` begins a finite, cancellable background search policy in the
  common WLAN layer. Reissuing it is idempotent. `search stop` cancels or waits
  for the current hardware scan at a defined boundary and preserves the last
  completed snapshot.
- `list` returns a bounded snapshot containing SSID bytes, BSSID, RSSI,
  channel/band, security capability, scan state, and generation. “Scanning,”
  “complete with zero results,” and “failed” are distinguishable.
- `set-key` locks, validates, and atomically rewrites the selected profile
  file. Replacing an SSID preserves its position; a new SSID appends. Auto
  selection therefore follows file order rather than an unstated RSSI policy.
- `up` brings the interface up, starts search if necessary, walks visible
  `auto` profiles in file order, waits for complete L2 authorization, then runs
  `dhcpc`. Search stops after a successful association. With no matching auto
  profile it leaves the interface up and the search active, and reports that
  no configured candidate was found.
- `connect` requires an existing profile for the named SSID, switches the L2
  association, and reacquires DHCP so stale address, route, and resolver state
  cannot survive an SSID change.
- `down` cancels any pending WLAN/DHCP operation, removes only L3 state owned by
  that interface/transaction, disconnects, stops search, and brings the link
  down. Resolver reconstruction must preserve other active interfaces.
- A failed `up` or `connect` reports the exact scan/profile/authentication/
  association/key/DHCP stage. Validation failure before mutation preserves the
  old state. Failure after mutation never reconnects the prior SSID: it retires
  transaction-owned L2/L3 work and leaves the interface administratively up,
  disconnected, search-stopped, and without managed WLAN L3 state. Incomplete
  retirement is reported as `degraded`, never as success.

All waits have one documented total deadline per operation. Driver, kernel,
child, and protocol cancellation must meet at an ownership-safe boundary; a
killed child alone is not evidence that an in-kernel request stopped.

## 6. Protocol and secret boundary

The existing whitespace-tokenized, small-buffer `ZNV1` request path cannot
represent arbitrary SSIDs, passphrases, or bounded scan lists. WLAN commands
therefore introduce `ZNV2` with:

- one canonical ASCII header, no longer than 32 bytes,
  `ZNV2 <request-id> <opcode> <payload-length>\n`, followed by exactly the
  declared number of payload bytes;
- explicitly encoded integers and length-delimited byte strings rather than a
  copied C struct;
- 4096-byte request, 32768-byte response, 64-result, and field-specific
  maxima;
- exactly one terminal response per request, containing any bounded typed
  state/results and no unsolicited second frame;
- exact malformed/truncated/oversized rejection; and
- no secret echo in errors or diagnostics.

Existing wired operations migrate to the same framing in p006. There is no
unversioned fallback, and an old client receives an explicit version failure
rather than being parsed as a WLAN request.

## 7. Work items

| ID | Status | Deliverable | Dependencies | Acceptance gate |
| --- | --- | --- | --- | --- |
| NET-00 | Complete with follow-ups | Existing `networkd`, `net`, `dhcpc`, boot orchestration, and fd 3 readiness | WS002 p020 | Retained regression baseline |
| NET-01 | Complete (`q029`) | RTL8156 USB-Ethernet physical data path | WS004 NCM sequence | Latitude external fetch passes |
| NET-05 | Planned in WS011 | Interactive `net`, `/etc/net.conf`, confirmed commit, VLAN/bridge model | WS011 | WS011 evidence |
| NET-10 | Active follow-up pool | Independent ECM/reconnect/sustained USB-Ethernet reliability | WS004/WS005 | Separately queued gates |
| NET-20 | Superseded by NET-25--NET-31 | Resident versioned `networkd`-to-`wpa` backend contract | 2026-08-30 topology decision | No implementation; retained as history |
| NET-21 | Superseded by NET-28 | Root-only `/etc/wpa/` database | 2026-08-30 profile decision | No implementation; retained as history |
| NET-22 | Superseded by WS004 p026-p030 | `/sbin/wpa` RTL8822CE backend | Archer-first decision | No implementation; RTL8822CE remains later hardware target |
| NET-23 | Superseded by NET-29--NET-31 | Old `net` WLAN backend integration | Fixed primitive topology | No implementation |
| NET-24 | Superseded | Pluggable WPA backend family | Fixed primitive topology | No implementation |
| NET-25 | Planned as p002 | WLAN v1 contract freeze | User decisions recorded above | P-book has no unresolved implementation-changing ambiguity |
| NET-26 | Planned as p003 | AF_UNIX peer credentials and one-socket authorization | NET-25 | Credential spoof/race/group/privilege fixtures pass |
| NET-27 | Planned as p004 plus WS004 p027 | Primitive `/sbin/wifi` and stable WLAN ioctl contract | NET-25, common WLAN fixture | search/list/status/connect/disconnect pass without DHCP/persistence |
| NET-28 | Planned as p005 | System/per-user `wifi.conf` and `set-key` | NET-25 | ownership/mode/symlink/locking/atomicity/redaction tests pass |
| NET-29 | Planned as p006 | `ZNV2`, peer authorization, `wifi` child secret-FD bridge | NET-26--NET-28 | malformed/auth/timeout/cancel/crash fixtures pass |
| NET-30 | Planned as p007 | Requested high-level `net wifi` operations | NET-29, WS004 WLAN fixture | full fake-device association/DHCP/down transaction passes |
| NET-31 | Planned as p008 | Archer end-to-end and repeatability acceptance | NET-30, WS004 p030 | physical L2, DHCP, transfer, down, reconnect, final repetition pass |

## 8. Cross-WS dependencies

- WS002 supplies init, fd 3 readiness, `networkd`, `net`, and `dhcpc` baseline.
- WS004 owns generic kernel WLAN facilities, USB RTL8822BU hardware,
  firmware, lifecycle, and test-double boundaries in p026-p030.
- WS011 owns `/etc/net.conf`, interactive candidate/commit behavior, and later
  VLAN/bridge design. WLAN secrets never enter `/etc/net.conf`.
- WS009 will document administrator/user commands, plaintext-secret risk,
  supported security modes, firmware provenance, and recovery.

## 9. Reconsideration boundaries

Return to planning rather than broadening an implementation Phase if:

- the physical unit is not USB `2357:012e`/RTL8822BU or has materially
  different descriptors;
- redistribution or loading of the required firmware cannot satisfy the
  recorded package/license policy;
- a one-shot primitive plus common kernel WLAN state cannot safely handle
  rekey, disconnect, cancellation, or hot unplug without a resident userspace
  supplicant;
- WPA2-Personal/CCMP requires a public UAPI incompatible with the frozen p002
  contract;
- peer credentials cannot be captured without a generally unsafe AF_UNIX ABI;
- ordinary-user control needs a seat/session policy rather than the approved
  `network` group; or
- physical acceptance requires DFS, WPA3, enterprise authentication, or a
  different radio before the initial milestone can be honestly claimed.
