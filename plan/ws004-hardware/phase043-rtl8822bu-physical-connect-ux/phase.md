# WS004 Phase 043: RTL8822BU physical connect state and command UX

Last updated: 2026-09-04

WSID: `ws004`

Phase ID: `p043`

Combined ID: `ws004-p043`

Status: in-progress (`q070`)

Parent: [WS004 hardware](../ws.md)

Tests: [WS004 test index](../tests/README.md)

## Objective

Make the prioritized RTL8822BU path work predictably on the physical target,
using the two newly repeatable observations rather than the passing q069 QEMU
run as the correction boundary:

1. `wifi INTERFACE connect ...` before any completed scan currently returns
   `ENOENT`; repeated attempts appear unusable until the missing scan
   prerequisite is supplied.
2. After a completed physical scan selects a supported 2.4-GHz BSS, connect
   can terminate in less than one second with `ETIMEDOUT`, no authentication,
   and zero protocol retries.  That localizes the failure before the ordinary
   over-air retry budget, at radio preparation or initial authentication-frame
   submission/completion.

This is one additional Phase in the existing WS004 WLAN/hardware work.  It may
adjust the thin `/sbin/wifi` frontend where required to expose and drive the
physical state machine, but it does not create a new command workstream,
implement profile persistence or DHCP, add 5-GHz support, repair AX211, or
perform the final five-run WS005 p008 acceptance campaign.

## Frozen user-visible contract

The primitive command grammar becomes:

```text
wifi [--quiet] INTERFACE up
wifi [--quiet] INTERFACE down
wifi [--quiet] INTERFACE search start
wifi [--quiet] INTERFACE search stop
wifi [--quiet] INTERFACE list
wifi [--quiet] INTERFACE status
wifi [--quiet] INTERFACE connect SSID PASSPHRASE
wifi [--quiet] INTERFACE disconnect
```

`--quiet` is accepted only in the shown global position, immediately before
the interface name.  In quiet mode the command writes nothing to either
standard output or standard error, including on failure; callers determine
success from a zero exit status and failure from a nonzero exit status.  Quiet
mode does not change the operation, timeout, cancellation, or credential
handling.

`up` and `down` use the existing generic network-interface administrative
control path; they do not add a HAL API or create a second WLAN ownership
mechanism.  Both operations are idempotent.  `down` cancels an active scan or
connection, disconnects an authorized station, clears volatile BSS/key state,
lowers carrier, and leaves the interface administratively down.  Direct
`search` and `connect` on an administratively down interface report
`ENETDOWN`; the higher-level future `net wifi up` orchestration remains
responsible for issuing `wifi ... up` first.

## Connect and progress semantics

One `wifi INTERFACE connect SSID PASSPHRASE` invocation has one monotonic
30-second deadline measured from command admission through controlled-port
authorization.  Scanning, BSS selection, radio preparation, 802.11
authentication, association, and the WPA2 four-way handshake consume this
same budget; no stage receives a fresh 30 seconds.

- If the current completed snapshot contains a supported exact-SSID BSS,
  connect selects it and begins connection immediately.
- If there is no usable completed snapshot or no supported exact-SSID BSS,
  connect starts a finite scan instead of returning `ENOENT`.
- If a scan completes without the target, another bounded scan may begin while
  time remains.  The operation ends with `ETIMEDOUT`, not a misleading file
  error, if no supported target becomes connectable within the total budget.
- Once a supported target is selected, the existing deterministic security
  filter remains authoritative.  In particular, an unsupported PMF-required
  or SAE-only record is not selected merely because it shares the SSID.
- Every terminal success, timeout, cancellation, down, detach, and transport
  failure retires the active operation and leaves subsequent `status`,
  `search`, and `connect` usable.  An unsuccessful pre-scan connect must not
  poison later operations.

Normal human output follows public state changes and never prints credentials
or derived keys.  Use specific stages rather than a generic `Connecting...`:

```text
Scanning... (30 seconds remaining)
Selecting a supported BSS...
Preparing the radio and starting 802.11 authentication...
Associating with the access point...
Completing the WPA2 four-way handshake...
Connected: controlled port authorized.
```

The remaining-time value is derived from the one monotonic deadline and is
refreshed at most once per second while scanning.  On an interactive
terminal the countdown is refreshed in place (`\r`, fixed two-digit seconds
field) and the next line starts on its own row; when standard output is not
a terminal, every refresh is one complete line.  State-transition lines are
printed once.  A terminal message names the public stage and error without
including the passphrase, PMK, PTK, GTK, nonces, or raw credential-bearing
frames.  Quiet mode suppresses all of these lines.

## Physical timeout correction

The present one-second transition limit is an internal radio/WPA handoff
deadline, not the requested end-to-end connect timeout.  Do not merely replace
it with an arbitrary larger constant.  First make the first failing boundary
observable, then align the internal deadlines with real USB execution:

1. Distinguish channel programming, hardware security enable, key/security
   clear, initial 802.11 Authentication TX submission, and its USB completion.
   Retain only bounded nonsecret stage/error/timing diagnostics.
2. Verify that monotonic kernel ticks and the supplied deadline use the same
   domain on physical amd64 hardware.
3. Give each synchronous USB/control operation a bounded transport deadline
   consistent with the controller's real completion behavior, while never
   exceeding the remaining overall 30-second connection budget.
4. Keep protocol retransmission deadlines separate from radio preparation.
   An unacknowledged transmitted Authentication frame may consume the existing
   protocol retry policy; a setup or submit failure must identify its own
   stage instead of appearing as retry exhaustion.
5. Preserve cancellation, detach, URB/DMA retirement, and secret erasure while
   changing deadline ownership.  QEMU speed must not remain an unstated timing
   requirement for real xHCI hardware.

No public HAL API change is authorized.  A public WLAN UAPI change is also not
the default: reuse the existing scan and connection states for human progress
where they are sufficient.  If a genuinely new stable UAPI state is required
to distinguish an essential operation, stop that part and return for an
explicit interface decision rather than casually revising the header.

## Ordered implementation

1. Add focused fake-device coverage for connect with no snapshot, target
   appearing in a later scan, total-deadline expiry, cancellation, and a new
   operation after each terminal result.
2. Add `wifi ... up`, `wifi ... down`, and the global `--quiet` parser/output
   contract using the existing administrative interface mechanism.
3. Change direct connect into the bounded scan/select/connect state machine
   and add progress rendering from existing public status.
4. Instrument and localize the physical zero-retry timeout, then correct the
   transport/transition deadline ownership at the measured failing boundary.
5. Run automatic regressions and produce one immutable amd64 image for one
   physical RTL8822BU acceptance check.

Normal-path physical connectivity comes before exhaustive new malformed and
fault matrices.  Cheap regression cases directly covering the two observed
failures are required; unrelated abnormal-path perfection remains in the
existing p030/p010 lifecycle work.

## Verification

- Focused host tests prove:
  - empty/no-target scan state starts scanning rather than returning `ENOENT`;
  - a later supported BSS advances through selection to authorization;
  - the total deadline is exactly one 30-second budget under a fake clock;
  - timeout, cancel, down, and detach permit a subsequent scan/connect;
  - `up`/`down` are idempotent and down cancels active work;
  - `--quiet` produces zero stdout and zero stderr on representative success,
    validation failure, scan timeout, connect failure, and down paths while
    preserving zero/nonzero exit status; and
  - ordinary output reports ordered, nonsecret stages.
- Existing WLAN common-core, WPA2 engine/codec/CCMP/L2, RTL8822BU, USB/xHCI,
  lifecycle, and credential-erasure focused regressions pass.
- Configured amd64 and i386 builds pass with `make -j16`; do not run aggregate
  `make check`.
- One disposable amd64 OVMF/xHCI control proves that the QEMU-passthrough path
  still scans and connects.
- One physical run on the exact RTL8822BU target starts from no completed scan,
  explicitly brings the interface up, invokes connect once, reaches
  authenticated/associated/keyed/authorized state within 30 seconds, then
  disconnects, goes down, comes up, and can start another scan.  Retain only
  redacted stage/timing/status evidence.

## Completion conditions

- Scan-before-connect is automatic and never exposes the old pre-scan
  `ENOENT` behavior.
- A failed attempt does not make later WLAN operations return stale `ENOENT`,
  `EBUSY`, or another retired-operation error.
- Normal and quiet command forms, explicit administrative up/down, one shared
  30-second deadline, and specific progress states match this Phase contract.
- The physical RTL8822BU normal path reaches controlled-port authorization;
  its former zero-retry sub-second timeout is removed, and retained diagnostics
  identify the corrected transport stage without exposing secret material.
- Automatic regressions and configured builds pass without a HAL API change,
  5-GHz claim, AX211 claim, DHCP/profile addition, or final repeatability
  claim.

The later WS005 p008 frozen-artifact five-consecutive-run campaign remains the
final repeatability gate.  This Phase intentionally asks the human for only
one physical confirmation after the implementation and automatic tests are
ready.

## q070 implementation checkpoint

The automatic candidate was completed on 2026-09-04 and remains in progress
only at the one physical acceptance boundary:

- direct connect now starts and waits for finite scans as needed under one
  command-wide 30-second monotonic deadline, reports ordered public progress,
  cancels its admitted work on failure, and never reports the old pre-scan
  `ENOENT` as the terminal result;
- primitive administrative `up`/`down` and the global `--quiet` form use the
  existing generic interface controls without a HAL or public WLAN UAPI
  change;
- radio preparation receives the remaining overall connection deadline, and
  the short authentication/association transition deadline begins from the
  radio callback's actual completion time instead of its stale admission
  time;
- bounded, nonsecret driver diagnostics distinguish channel programming,
  security enable/clear, authentication submission, and association
  submission failures;
- the direct-command fake-clock fixtures cover automatic scan success, exact
  30-second scan expiry/cancellation, ordered progress, quiet validation, and
  idempotent quiet up/down; the WLAN/WPA2/RTL8822BU/USB/xHCI focused suites and
  configured amd64/i386 builds pass; and
- the first physical candidate selected the supported BSS but ended with
  `ETIMEDOUT`, `authenticated=no`, `associated=no`, `key=no`, and zero
  retries. No driver stage-failure diagnostic appeared between invocation
  and the error, localizing the remaining synchronous cost before the first
  authentication transaction completed;
- the corrected normal path no longer clears all twelve driver-owned CAM
  slots before the first authentication frame. A fresh hardware epoch or a
  completed disconnect must instead prove that no live, staged, retired, or
  uncertain key/association generation exists; an invariant violation fails
  closed. Actual disconnect and key rollback paths retain their checked CAM
  cleanup barriers;
- ordinary mode flushes a nonsecret preparation message before entering the
  synchronous connect ioctl, and the driver reports channel/security timing
  plus the remaining budget on successful preparation as well as its bounded
  failure stages; and
- the revised immutable amd64 candidate at `build/q070-amd64/hdd-image.img`
  initially exposed one more physical ordering defect: the command consumed
  its full 30-second deadline, printed BSS selection twice, and ended with
  `ETIMEDOUT`, no authentication/association/key, and zero retries.  The first
  connection admission had returned transient `EBUSY` while receive/poll work
  briefly owned the radio, and the CLI incorrectly converted that ownership
  barrier into a replacement scan;
- RTL8822BU connection preparation now closes transmit admission, retains one
  connection generation, and waits within the existing overall deadline for
  the overlapping radio operation to retire.  It no longer exposes that
  transient barrier to the WPA engine or leaves a partially admitted failed
  generation;
- the CLI treats `ENOENT` alone as a missing usable BSS snapshot.  A transient
  zero-generation `EBUSY` waits and retries without clearing the completed
  snapshot or starting a second scan; a nonzero generation remains a real
  conflicting connection and is not silently replaced.  Focused fixtures
  prove both the driver serialization and no-rescan CLI sequence; and
- the next immutable amd64 candidate at `build/q070-amd64/hdd-image.img` is
  253755392 bytes with SHA-256
  `826608c32e43e952ad615d01c19ccd5fd35e7f6b4c22f30d2a6ce3da0f872a89`.
  The initially published 252723712-byte file was inadvertently built with
  the Apple UEFI-only layout.  The replacement is explicitly built from
  `config/ci/config-amd64.mk` as the PC/AT UEFI+BIOS Hybrid layout and reaches
  root/data overlay, active swap, `/sbin/init`, and `login:` through
  OVMF/q35/xHCI USB storage in 20 seconds;
  WLAN/WPA2/RTL8822BU/USB/xHCI focused suites and configured amd64/i386 full
  builds pass.

The authorized passthrough host did not enumerate the exact `2357:012e`
device at this checkpoint, so no substitute USB device was passed through and
no QEMU radio-connect claim is made.  This does not prevent the requested one
physical-machine check with the immutable image; its result is the remaining
completion evidence.

The third physical observation, taken on 2026-09-04 with that candidate,
advanced past both previously corrected boundaries and exposed the next one:

- automatic scan-before-connect ran once under the shared deadline (thirty
  down to eighteen displayed seconds), selected a supported BSS exactly once,
  and radio preparation completed and reported its success diagnostic; the
  CLI then announced 802.11 authentication and the command ended in
  `ETIMEDOUT` with no association and no driver stage-failure diagnostic
  after the authentication announcement; and
- every driver connect diagnostic printed its conversions literally as
  `%llu`.  The production amd64/i386 `hal_printf` implemented no `l`/`ll`
  length modifiers, and the focused suites could not see this because the
  fixture `hal_printf` intentionally discards output.  The instrumentation
  that this Phase depends on therefore observed nothing on the physical
  machine.

The 2026-09-04 correction makes that boundary observable before any further
deadline or transport change:

- amd64 and i386 `hal_printf` accept the `l` and `ll` length modifiers for
  `%d`/`%u`/`%x`/`%X` with full 64-bit magnitudes, preserving every existing
  conversion, width/zero-fill form, and unknown-conversion passthrough; the
  new `run-hal-printf-format-host-test.sh` gate formats through the real
  amd64 implementation into a captured console in ordinary and ASan/UBSan
  builds.  The i386 kernel keeps its existing linked compiler builtins for
  the 64-bit divisions;
- the driver reports bounded nonsecret connect-path evidence: management
  submissions print success as well as failure with their cookie and
  remaining budget; pre-key CCX TX report completions print acknowledged,
  error, and delivery verdict; a reserve that must tombstone earlier frames
  prints the expired and total missing-report counts with the recovery flag;
  and authentication, association-response, disassociation, and
  deauthentication frames print subtype, length, RSSI, and either the station
  delivery verdict or the classification rejection, while beacons and keyed
  data traffic stay quiet.  One physical run can now distinguish frames the
  firmware never reported, frames aired but never acknowledged, and
  acknowledged frames whose responses never arrived, were rejected before
  delivery, or never satisfied the engine;
- a failed ordinary-mode connect prints the redacted public status line
  (stage, scan state, retries, error) before the terminal message, which now
  names the failing stage as the Phase contract requires; quiet mode still
  writes nothing.  A new fixture scenario locks the terminal-timeout ioctl
  sequence, the stage/retry summary, cancellation of the admitted attempt,
  and credential clearing;
- the WLAN/WPA2/CCMP/L2/crypto/common-core, RTL8822BU driver, xHCI, USB
  binding/recovery/zero-packet, console, HAL printf, and wifi command focused
  suites pass; configured `config/ci/config-amd64.mk`, `config-pcat.mk`, and
  `config-pc98.mk` builds pass with `make -j16`; and the disposable OVMF
  q35/xHCI USB-storage control boots the rebuilt image to `login:` in nine
  seconds with no failure marker; and
- the next immutable amd64 candidate at `build/q070-amd64/hdd-image.img` is
  253755392 bytes with SHA-256
  `8c5715c3b361ef6740266ff2fa77aab5ed2a92cde4db2abdf5e608b14a22a754`, built
  from `config/ci/config-amd64.mk` with the PC/AT UEFI+BIOS Hybrid layout.

The requested physical step remains exactly one repetition of the previous
check with this image: from no completed scan, bring the interface up, invoke
one direct connect, and retain the redacted console output.  The new
diagnostics identify the first failing transport stage either way, and the
failure-status line reports the engine's retry count, which past observations
could not.

The fourth physical observation, taken the same day with the instrumented
image, completes the localization that this Phase was opened for:

- every scan dwell's probe submission reported firmware transmit success, the
  supported BSS was selected once, and six authentication submissions
  (`cookie=1..6`) each completed over USB with `error=0`;
- all six CCX reports returned `acknowledged=0`: the firmware transmitted
  each authentication frame and observed no MAC acknowledgement.  The entire
  burst consumed roughly one tick of the remaining budget, terminal
  `ETIMEDOUT` followed, and no authentication response, deauthentication, or
  classification rejection was ever delivered;
- the new failure-status line worked but exposed two evidence defects: it
  printed `retries=0` because the engine's failure cleanup erased the retry
  count, and it printed `rssi=-110` for the selected transition-mode
  (WPA2+WPA3-capable) BSS.  `-110` is exactly a zero raw PHY-status power
  byte minus the fixed 110 offset, and the user reports the access point is
  well within reliable range, so the constant is suspected to be a sentinel
  from zeroed PHY-status content rather than a real measurement; and
- USB transport, xHCI, driver submission, CCX reporting, deadline ownership,
  and the engine state machine are therefore all proven on the physical
  machine.  The one remaining failing boundary is that the transmitted
  authentication frame earns no acknowledgement from an in-range access
  point, which points at the transmit RF side (power calibration, antenna
  path, or another radio-programming gap) rather than protocol logic.

The same-day correction candidate prepares that RF investigation without
guessing at register changes:

- an unacknowledged authentication or association report no longer resubmits
  immediately: the attempt retires its report cookie and the existing step
  deadline drives the next spaced retransmission, so the bounded retry budget
  spans seconds instead of one sub-second burst.  EAPOL keeps its immediate
  policy because the authenticator paces that exchange.  A new engine fixture
  case proves the spaced timeline, duplicate-report rejection, response
  acceptance while waiting, and terminal exhaustion;
- the engine failure cleanup deliberately preserves the retry count, so the
  status line and CLI failure summary now report real retry evidence;
- attach prints one bounded nonsecret efuse calibration record per RF path
  (CCK and BW40 base indexes, OFDM diff, RFE option, channel plan, without
  any identity bytes), deciding whether transmit power sits at a healthy
  midrange index or the near-zero floor; and
- each scan generation prints one raw eight-byte PHY-status sample with the
  derived RSSI, deciding whether `-110` is real received power or zeroed
  report content.

WLAN engine/common-core/L2/CCMP/codec, RTL8822BU driver, and wifi command
focused suites pass; configured amd64 and pcat builds pass with `make -j16`;
and the OVMF q35/xHCI USB-storage control boots the rebuilt image to
`login:`.

The fifth physical observation ran that instrumented candidate and confirmed
the corrections while narrowing the boundary further: the prepared line
reported real timings (twelve channel ticks, one security tick, 2985
remaining), the six authentication attempts were driven by the spaced step
deadline, the failure line reported truthful `retries=5`, and every report
still returned `acknowledged=0`.  The selected in-range transition-mode BSS
displayed `rssi=-90` where the fourth observation displayed `-110`, so the
PHY-status power content varies rather than being a fixed zero sentinel, yet
both values remain roughly forty decibels below a same-room expectation.
The efuse and phy-sample records printed before the photographed console
region and remain to be collected.  A source audit against the upstream
rtw88 reference found the crystal-capacitor programming (0x24/0x28 fields),
RFE select values (0x705770 eFEM / 0x745774 iFEM), TR-switch word, TX AGC
base registers (0x1d00/0x1d80), and TRX path mode all faithful, and the
probe and authentication descriptors agree on QSEL 18, RATE_ID 8, USE_RATE
plus DISDATAFB, and the forced 1-Mbps CCK data rate; the working probe path
differs from the failing authentication path essentially only by broadcast
versus unicast addressing.  Because a unicast report distinguishes an
aired-but-unacknowledged frame from one the firmware dropped before airing,
the same-day candidate adds the raw CCX status byte to the bounded tx-report
diagnostic.  Focused driver gates, the configured amd64 build, and the OVMF
q35/xHCI `login:` control pass.  The next immutable amd64 candidate at
`build/q070-amd64/hdd-image.img` is 253755392 bytes with SHA-256
`efcbea4d4eefb6dcd86fbc23220d4366406d318f5d4fc2b2d6a99ccc0928b0a6`.  The
requested physical evidence is: the attach-time efuse calibration lines (a
replug reprints them), one scan's phy-sample line, one `wifi list` snapshot
comparing neighbor RSSI values, and one near-AP connect showing the new
tx-report status bytes.  Together these select between the
transmit-power-floor, RF-path, and firmware-queue explanations before any
radio register change is authored.

The sixth physical observation, taken with that candidate next to the
access point, reached the Phase's declared physical outcome.  From no
completed snapshot, one direct connect ran the automatic scan (the probe
report showed status `0x20` with the acknowledgement bits clear), selected
the supported BSS once, prepared the radio (`channel-ticks=12`,
`security-ticks=0`, `remaining=2985`), and then completed every stage with
first-attempt acknowledgements: the authentication frame (`cookie=1`,
`status=00`), its response (`subtype=0b`, 30 bytes, `-84 dBm`), the
association request (`cookie=2`) and response (`subtype=01`, 137 bytes,
`-83 dBm`), and the EAPOL exchange (`cookie=3`), ending with `Connected:
controlled port authorized`.  The retained evidence carries no credential,
identity, or key material.  Combined with the fourth and fifth observations,
this localizes the remaining physical deficiency precisely: the functional
path is correct, and the radio link budget is roughly forty decibels below
expectation in both directions, so authentication is acknowledged only next
to the access point.  That RF-side deficit is recorded as
[`BUG-009`](../../known-bugs.md) for a later finite Phase rather than being
folded into this one.  The subsequent direct `wifi wlan0 disconnect` on the
authorized station completed synchronously with `disconnected state=idle`,
and the following administrative `wifi wlan0 down` succeeded.

With the physical path proven, the user asked that the diagnostics drop from
debug level to user-diagnostic level before closure — background traffic
must not write to the console — and that the scan countdown refresh one
line in place.  The closing candidate implements both:

- `RTL8822BU_TRACE` (default 0, overridable at compile time) keeps the
  debug-level lines in the source but compiled out: the efuse calibration
  record, radio preparation timing, successful management submissions, every
  CCX report, delivered management responses, and the per-scan PHY sample.
  User-level diagnostics stay on and are all failure-bounded: a failed
  management submission with its stage, admission/serialization/channel/
  security preparation failures, a connect-stage management frame rejected
  before delivery, and missing firmware reports.  Scanning, connecting, and
  steady-state traffic therefore print nothing on the normal path; the
  driver fixture passes at both trace settings;
- the CLI countdown uses `\r` with a fixed two-digit seconds field on an
  interactive terminal and terminates that line before any following
  output, while a pipe still receives one complete line per refresh; a new
  fixture scenario covers both shapes; and
- WLAN driver and wifi command focused suites, configured amd64 and pcat
  builds, and the OVMF q35/xHCI `login:` control pass.  The closing
  immutable amd64 candidate at `build/q070-amd64/hdd-image.img` is
  253755392 bytes with SHA-256
  `fffc64c2995799999417173aba3f339fb4a40553ad932cbeea2b29a20b274ff2`.

The single physical acceptance check closes with one run of this candidate:
up, one direct connect next to the access point, a short period of ordinary
traffic with a silent console, disconnect, down, up, and a new scan.  Its
confirmation clears this Phase.
