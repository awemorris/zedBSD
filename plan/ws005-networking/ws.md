# WS005: networking and WLAN

Last updated: 2026-09-05

WSID: `ws005`

Status: active; q059 completed the minimum direct `/sbin/wifi` command in p004
and one physical-equivalent RTL8822BU scan/WPA2/CCMP/DHCP/ping/fetch path in
p009. Q070 subsequently closed the physically accepted 2.4-GHz WS004
RTL8822BU baseline. The user has now reopened WS004 p041 for W52 5-GHz and
added p044's asynchronous-kernel ownership correction and resident networkd
link recovery as WS005 p011. Detailed primitive, protocol, command composition,
and managed-link work remains in p010, p006/p007, and p011 before this WS's
separately owned p008 final acceptance.

Parent: [master plan](../master.md)

Last verified result: `ws005-p002` is now complete after q053 verified the
frozen topology, ownership, security, supersession, and numeric bounds across
the dependent WS004/WS005 P-books. `ws005-p005` passes the real root, sudo-like,
effective-user, and ordinary-user `/sbin/net wifi set-key` paths, checked
read-side inode replacement, exact metadata/atomicity/redaction gates, and an
abrupt-stop second-boot persistence cell. `ws005-p003` publishes one authenticated
`root:network 0660` socket, returns immutable 12-byte AF_UNIX peer snapshots,
permits admitted non-root `SHOW` while denying mutation, and rejects direct
non-root mutating network ioctls.  Its focused, analyzer, sanitizer, full
build, and PC-98 native runtime gates pass.  The earlier RTL8156 carrier,
DHCP, ping, and external-fetch path remains passing.

Resume point: p002, p003, and p005 are complete. P002's historical
per-interface/profile-forwarding clauses are explicitly superseded by the
2026-09-05 global-policy amendment implemented through p006/p007/p011.
WS004 p026 is also complete:
the purchased Japan-market Archer has no printed revision, its retained exact
descriptor is authoritative, and the separately installed `rtl8822b-firmware`
package boundary is frozen. Q055 completed the generic WLAN core/fake radio in
`ws004-p027`, and q056 completed the independent RTL8822BU pre-radio substrate
in `ws004-p036`. Q057 completed `ws004-p028`'s BSD-3-Clause radio-table import
and conservative automatic scan milestone. Q058 completed WS004 p029 secure
L2 independently of the command stack. Q059 then completed p004's human
direct-root normal path and p009's one useful physical IP path. Continue with
p041, p044, p010, p006, p007, and p011 in dependency order; p044 deliberately
supersedes only p030's kernel-owned automatic reconnect. Q070's physical
RTL8822BU baseline remains complete. None of these WS005 Phases, nor p008's
final five-run acceptance, is complete.

Shared tests: [WS005 test index](tests/README.md)

## Phase registry

| Phase | Status | Result / resume point |
| --- | --- | --- |
| [`ws005-p001`](phase001-usb-ncm-physical-datapath/phase.md) | Complete (`q029`) | RTL8156 NCM carrier/static/DHCP/ping and final Latitude external fetch pass |
| [`ws005-p002`](phase002-wlan-v1-contract/phase.md) | Complete (`q053`) | Frozen v1 topology, ownership, security, supersession, limits, and recovery semantics are synchronized across the dependent P-books; no source or hardware result is claimed |
| [`ws005-p003`](phase003-unix-peer-credentials/phase.md) | Complete (`q040`) | Fixed 12-byte connection-time AF_UNIX identity, checked `root:network 0660` publication, root/non-root operation policy, and kernel ioctl privilege boundary pass focused and native PC-98 gates |
| [`ws005-p004`](phase004-wifi-ioctl-command/phase.md) | Complete (`q059`) | Six direct-root `/sbin/wifi` forms pass focused bounds/secret-clearing gates and the physical normal path without taking DHCP or persistence ownership |
| [`ws005-p005`](phase005-wifi-credential-store/phase.md) | Complete (`q051`) | Real root/sudo-like/non-root `/sbin/net wifi set-key`, read-side replacement rejection, metadata, redaction, atomic update, abrupt stop, and second-boot persistence pass |
| [`ws005-p006`](phase006-networkd-wifi-protocol/phase.md) | In progress in `q071`; focused gates pass | Replace whitespace `ZNV1` with bounded ZNV2 global WLAN requests, authenticated policy-owner selection, daemon-side fixed-store reads, and a secret-FD child path |
| [`ws005-p007`](phase007-net-wifi-orchestration/phase.md) | In progress in `q071`; focused gates pass | Implement the six global `net wifi` forms through `networkd` to interface-specific `ifconfig`, `wifi`, and `dhcpc` children, with deterministic all-WLAN selection and one managed connection |
| [`ws005-p008`](phase008-archer-physical-acceptance/phase.md) | Planned; depends on p007, p010, p011, `ws004-p030`, `p041`, and `p044` | Prove one complete W52 global-policy/DHCP/transfer path, event-triggered userspace recovery, manual suppression, then the final frozen-artifact repeatability campaign |
| [`ws005-p009`](phase009-wlan-minimum-connectivity/phase.md) | Complete (`q059`) | One USB-passthrough development run reached scan, authorized carrier, DHCP, two 3/3 ping checks, an 84255-byte fetch, disconnect, and down |
| [`ws005-p010`](phase010-wifi-primitive-hardening/phase.md) | Ready after WS004 p044; not started | Make `/sbin/wifi` the sole owner of one 30-second asynchronous scan/select/connect retry sequence and complete its abnormal/semi-normal, cancellation, race, boundary, and redaction matrix |
| [`ws005-p011`](phase011-networkd-managed-wlan-reconnect/phase.md) | In progress in `q071`; focused gates pass | Own the authenticated policy UID and four persistent global states, consume link events, and run one same-SSID 30-second recovery child before clean fallback to `auto-searching` |

`ws002-p020` remains historical ownership of the current wired
`networkd`/`net` baseline; it is not renumbered into this WS. Native device and
common kernel WLAN work is owned by `ws004-p026` through `ws004-p030` and the
p044 ownership correction.

## Goals

- Establish one simple vertical communication path before perfecting detailed
  abnormal, recovery, race, and repeatability behavior. Essential bounds,
  finite waits, checked returns, and secret redaction still apply from the
  first implementation.
- Preserve the completed wired and USB-Ethernet behavior while adding one
  coherent WLAN control path.
- Make `/sbin/net` the interface used by people and desktop software.
- Keep `/sbin/net` stateless and expose only the six global WLAN forms; public
  commands never select a `wlanN` interface or send a credential to networkd.
- Keep `networkd` a privileged, lightweight command orchestrator whose fixed
  primitive backends are `/sbin/ifconfig`, `/sbin/wifi`, and `/sbin/dhcpc`.
- Make networkd the owner of one authenticated active-policy UID, four
  persistent global WLAN states plus bounded connecting/reconnecting
  transients, deterministic discovery across every `wlanN`, and at most one
  managed WLAN connection.
- Keep `/sbin/wifi` a thin WLAN-ioctl command: no persistence, DHCP, routing,
  DNS, interactive database, or resident daemon role.
- Use one authenticated Unix-domain control socket for root and authorized
  ordinary users; never trust an identity carried in the request payload.
- Bring up the exact Japan-market TP-Link Archer T3U Nano as the first WLAN
  target. It has no printed revision; its retained `2357:012e`,
  `bcdDevice=2.10`, `ff/ff/ff`, five-endpoint descriptor is authoritative.
  Public V1.0 evidence remains documentary RTL8822BU family evidence only.
- Extend that accepted adapter to the conservative Japan W52 channels
  36/40/44/48 before final acceptance; DFS and broader regulatory policy stay
  outside the first 5-GHz boundary.
- Keep scan and one-attempt connect asynchronous in the kernel and let each
  `/sbin/wifi connect` child own its sole 30-second high-level retry. On RF
  loss, networkd rereads the active owner's store and runs exactly one such
  child for the same selected SSID. Success returns to `connected`; failure
  cleans stale L2/L3 ownership and returns to `auto-searching`. Networkd never
  retains a passphrase between children.

## WS completion conditions

WS005 is complete when the retained wired path and the selected W52 WLAN path
operate through `net` -> `networkd` -> fixed primitive children; peer
authorization is kernel-attested; system and per-user credentials satisfy the
documented ownership, atomicity, and redaction rules; scan, explicit and auto
association, DHCP, disconnect, cancellation, recovery, and useful diagnostics
pass the automatic fixture matrix; and the selected physical adapter passes
the declared final acceptance campaign. Direct primitive recovery remains
available to root without making `networkd` optional for ordinary users.

p009 is only the early development checkpoint for that sequence. It does not
replace p006/p007 composition, p010/p044 hardening, p011 recovery, or p008
final acceptance.

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
a resident backend. The common kernel WLAN layer owns asynchronous finite scan,
one-attempt authentication/association, key/rekey, controlled-port, link-event,
and disconnect state needed after an ioctl returns. `/sbin/wifi` owns the
30-second high-level scan/connect retry while it runs. A chip driver owns only the
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
- one descriptor-confirmed Japan-market TP-Link Archer T3U Nano / RTL8822BU USB
  implementation, without claiming a printed hardware revision;
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
6. `net wifi set-key` updates the caller's own file locally. Root means
   effective UID zero and selects `/etc/wifi.conf`; another effective UID
   selects that account's `~/.wifi.conf`. Consequently `sudo net ...`
   intentionally writes the root policy. After a successful atomic update it
   sends only an empty, nonsecret `profiles-changed` notification. Notification
   failure does not roll back or falsify the successful file update.
7. An accepted `net wifi enable` makes its kernel-attested peer effective UID
   the one active policy owner. A later authorized `enable` switches ownership;
   UID zero is always allowed to override. Service restart begins `disabled`
   with no active owner.
8. `networkd` never accepts `HOME`, UID, GID, credential-file pathname,
   interface name, passphrase, or profile record in a global WLAN request. It
   derives the fixed store from the active policy UID and reads that file
   itself only when an operation needs a profile.
9. The persistent store is the sole long-lived passphrase owner. Networkd may
   copy one selected passphrase into a bounded operation buffer and fd 4 for
   one `/sbin/wifi` child, then must wipe every copy when that child ends.

The euid-selected file is the official policy source, not a per-user secret
isolation boundary. Membership in `network` grants the declared WLAN
administration authority, including replacing the current policy owner through
an authorized `enable`. Stronger seat/session arbitration remains outside v1.

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
wifi <interface> up
wifi <interface> down
wifi <interface> connect <SSID> <passphrase>
wifi <interface> disconnect
```

WS004 p043 extends every primitive form with the optional global placement
`wifi --quiet <interface> ...`, makes quiet operation produce no stdout or
stderr, and makes direct connect automatically scan for up to one shared
30-second connection deadline. P010, after WS004 p044, makes that deadline and
all high-level retries solely userspace-owned. A missing supported BSS (`ENOENT`) starts the
scan, whereas transient driver ownership (`EBUSY`) waits without discarding a
completed snapshot or starting a replacement scan. These are physical
RTL8822BU corrections in that existing hardware WS, not a separate WS005
Phase. The primitive remains L2-only and does not gain profile persistence or
DHCP.

It performs no DHCP or persistence. P004 first supplies only human output.
P006 later adds the bounded machine-readable mode used by `networkd` and the
internal child form accepting key material from a dedicated inherited
descriptor; `networkd` never places a
passphrase in child argv, temporary diagnostic files, or logs. The requested
public connect form is retained, with its shell-history/process-list exposure
documented as a direct-administration limitation.

The public high-level commands are:

```text
net wifi set-key <SSID> <passphrase> [auto]
net wifi enable
net wifi disable
net wifi list
net wifi connect <SSID>
net wifi disconnect
```

These are the complete v1 public WLAN forms. They never accept an interface.
`auto` is the only optional policy token for `set-key`; omission means manual,
and an explicit `manual` operand is rejected.
`net wifi list` is required because people and desktop software use `net`, not
the primitive `wifi` command, and returns a bounded aggregate over every
discovered WLAN interface. The existing interactive `net` console exposes the
same operations without a separate device-management mode.

## 5. Operation semantics

- `set-key` locks, validates, and atomically rewrites the selected profile
  file. Replacing an SSID preserves its position; a new SSID appends. `auto`
  marks automatic eligibility and omission writes `manual`; the CLI rejects
  an explicit `manual` operand. The successful local write remains
  authoritative even if its nonsecret notification fails.
- `enable` establishes or switches the authenticated policy UID, enters
  `auto-searching`, brings up and scans all eligible `wlanN`, walks visible
  `auto` profiles in file order, and permits at most one association. For each
  profile it chooses the first stable-discovery-order WLAN reporting a
  supported visible candidate. DHCP starts only after secure L2 authorization.
- `list` returns a bounded stable-order aggregate containing interface
  identity, SSID bytes, BSSID, RSSI, channel/band, security capability, scan
  state, and generation. It does not change policy or expose credentials.
- `connect SSID` requires an enabled policy and an exact saved profile. Manual
  and auto profiles are both eligible. It chooses the first stable-order WLAN
  reporting that SSID and reacquires DHCP so stale address, route, and resolver
  state cannot survive a network or interface change.
- `disconnect` cancels managed work, retires the one L2/L3 connection, and
  enters `manual-disconnected` while preserving the active policy UID. It
  leaves every managed WLAN interface up and scanning, but completed scans do
  not resume automatic selection.
- `disable` cancels all managed WLAN/DHCP work, removes only managed L3 state,
  brings managed WLAN interfaces down, clears the owner, and enters `disabled`.
- RF loss from `connected` enters internal `reconnecting`. Networkd rereads
  the current active-owner store and invokes exactly one ordinary 30-second
  `/sbin/wifi connect` child for the same selected SSID. Success returns to
  `connected`; failure removes stale managed L2/L3 state and settles in
  `auto-searching`, with no nested daemon retry and no retained passphrase.
- A failed `enable` or `connect` reports the exact scan/profile/authentication/
  association/key/DHCP stage. Validation failure before mutation preserves the
  old state. Failure after mutation retires transaction-owned work and enters
  the prescribed global state without fabricating success. Incomplete
  retirement is `degraded`.

All waits have one documented total deadline per operation. Kernel scan and
single-attempt connect admission return promptly; `/sbin/wifi` observes their
asynchronous generations under its userspace deadline. Driver, kernel,
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

The global WLAN opcodes are `enable`, `disable`, `list`, `connect`,
`disconnect`, and `profiles-changed`. Only `connect` carries an SSID. No WLAN
request carries an interface, UID, home, pathname, passphrase, or profile
record.

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
| NET-25 | Complete as p002 (`q053`) | WLAN v1 contract freeze | User decisions recorded above | P-book and dependent design records are synchronized; no implementation result claimed |
| NET-26 | Planned as p003 | AF_UNIX peer credentials and one-socket authorization | NET-25 | Credential spoof/race/group/privilege fixtures pass |
| NET-27 | Complete as p004 (`q059`) | Minimum human direct-root `/sbin/wifi` over the stable WLAN ioctl contract | NET-25, WS004 p027-p029 | one normal search/list/status/connect/disconnect sequence passes without DHCP/persistence |
| NET-28 | Complete as p005 (`q051`) | System/per-user `wifi.conf` and `set-key` | NET-25 | ownership/mode/symlink/locking/atomicity/redaction and abrupt-stop/remount tests pass |
| NET-29 | Queued as p006 in q071 | `ZNV2`, peer authorization, global no-secret WLAN requests, policy-store reader, and `wifi` child secret-FD bridge | NET-26--NET-28 | malformed/auth/store/timeout/cancel/crash fixtures pass |
| NET-30 | Queued as p007 after p006/p010 | Six global `net wifi` operations, stable all-WLAN selection, and one-connection transaction | NET-29, NET-33, WS004 p044 | full fake-device automatic/manual association, DHCP, disconnect/disable, and policy-state matrix passes |
| NET-31 | Planned as p008 | Archer end-to-end and repeatability acceptance | NET-30, NET-33, NET-34, WS004 p030/p041/p044 | physical W52 global policy, DHCP, transfer, event/userspace recovery, manual suppression, and final repetition pass |
| NET-32 | Complete as p009 (`q059`) | Minimum physical WLAN communication checkpoint | NET-27, WS004 p026-p029 | one runtime-only-credential run reaches carrier, DHCP, ping, and bounded fetch |
| NET-33 | Ready as p010 after WS004 p044; not started | Primitive CLI sole 30-second high-level retry plus abnormal/semi-normal hardening | NET-27, NET-32, WS004 p044 | asynchronous generations, bounded retry/invalid/race/cancel/detach/redaction fixtures pass |
| NET-34 | Queued as p011 in q071 | Resident global policy UID/state, store reread, link-event recovery, and one-connection arbitration | NET-29, NET-30, NET-33, WS004 p044 | no retained passphrase, deterministic selection, one same-SSID 30-second recovery child, explicit suppression with scans retained, clean `auto-searching` fallback, and identity-safe cleanup pass |

## 8. Cross-WS dependencies

- WS002 supplies init, fd 3 readiness, `networkd`, `net`, and `dhcpc` baseline.
- WS004 owns generic kernel WLAN facilities, USB RTL8822BU hardware,
  firmware, lifecycle, and test-double boundaries in p026-p030. P044
  supersedes p030's automatic reconnect only, retaining the other lifecycle
  evidence while adding asynchronous-operation and link-event boundaries.
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
