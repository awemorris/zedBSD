# WS004 Phase 030: WLAN lifecycle, reconnect, and hardware hardening

Last updated: 2026-09-01

Phase ID: `ws004-p030`

Status: automatic milestone in progress (`q060`); later shared physical
closure remains outside this Queue

Parent: [WS004 hardware expansion](../ws.md)

Tests: [WS004 test index](../tests/README.md)

## Objective

Harden the completed minimal RTL8822BU WLAN profile across rekey, transient
link loss, bounded reconnect, USB errors, interface up/down, reset, unplug,
reinsert, shutdown, and repeated physical use. Completion means the declared
2.4-GHz/20-MHz WPA2-Personal/CCMP L2 profile is reliable and diagnosable; it
does not imply 5-GHz, DFS, VHT, roaming, or DHCP support.

This Phase begins only after one simple attach/scan/connect/DHCP/ping/fetch
path works in WS005 p009. Q059 satisfied that prerequisite. Its detailed
abnormal and semi-normal behavior is a second layer of work and remains wholly
unimplemented by that normal-path result.

The common kernel WLAN layer owns long-lived rekey/reconnect and controlled
port state while an interface instance remains up. WS005 owns persistent SSID/
passphrase policy, `wifi`/`net wifi` commands, automatic profile choice, and
running `dhcpc` after L2 authorization.

## Dependencies

- `ws004-p029` automatic milestone: initial association, four-way handshake,
  CCMP key-CAM/Ethernet model, controlled port, and sensitive-state contract.
- `ws004-p012`: checked net-device close/removal/shutdown and stale identity
  purge.
- `ws004-p011` and `p015`: checked USB request/callback drain and interface
  ownership through disconnect and controller teardown.
- A deterministic fake authenticator/USB transport that can force GTK rekey,
  deauthenticate a station, restart its radio, and retain deterministic
  2.4-GHz settings. A real controlled AP is used once by WS005 p009 for the
  simple communication path and later by the shared WS005 p008 lifecycle
  checkpoint; p009 does not force rekey or recovery.
- [`ws005-p009`](../../ws005-networking/phase009-wlan-minimum-connectivity/phase.md):
  one physical normal path reaches secure carrier and useful IP communication
  before this lifecycle matrix is started.

This Phase was not part of q059. A later Queue may now execute its finite
automatic milestone. It does not authorize an additional physical adapter/AP
checkpoint or repeated human action; final lifecycle evidence remains the
single later shared WS005 p008 request after all automatic gates.

## Frozen reconnect policy

The desired connection exists only while the current `wlanN` is
administratively up and has an accepted p029 connection request. For a
transient beacon-loss, deauthentication/disassociation, firmware report, or
recoverable USB/radio error:

1. Close the controlled port and lower carrier before releasing packets or
   modifying a key.
2. Retire admitted TX, invalidate the active BSSID/AID/key generation, delete
   hardware keys, and scrub PTK/GTK/nonces while retaining only the PMK and
   redacted desired SSID needed for same-interface reauthentication.
3. Perform a targeted scan within the p028 legal channel profile, then execute
   fresh authentication, association, nonces, PTK/GTK, and four-way handshake.
   Never restore a CAM image or continue a pre-reset PN.
4. Retry after 0, 1, 2, 4, and 8 seconds, with no more than five failed attempts
   or 30 seconds in one reconnect generation. Success clears the backoff;
   exhaustion enters `FAILED`, leaves carrier down, and requires a new explicit
   connect/up request.
5. Explicit disconnect/down, a new SSID request, suspend/shutdown, or device
   removal cancels backoff immediately and makes every later timer/event stale.

The core does not retain a passphrase or PMK after a USB device instance is
removed. Reinsert creates a new `wlanN` instance; WS005 rereads its selected
credential database and submits a new request if policy says it should. This
keeps hardware lifetime separate from persistent credential ownership.

## Rekey contract

Support both WPA2 group-key handshake and a new four-way pairwise rekey while
connected:

- validate EAPOL descriptor/version, replay counter, MIC, nonce, encrypted key
  data, GTK KDE/index, and active authenticator exactly as in p029;
- stage a replacement CAM slot/key generation before switching. Publish the
  new generation atomically, then retire the old key only after all TX/RX that
  references it has drained;
- never reinstall the same PTK/GTK or reset a PN in response to a retransmitted
  message. Resend the required EAPOL acknowledgement without changing installed
  state;
- keep ordinary data blocked if the AP requires pairwise reauthentication and
  the controlled port is no longer valid. A group-only rekey may retain the
  authorized pairwise path while its finite handshake is valid;
- on MIC/replay/unwrap/CAM/timeout failure, lower carrier, delete staged keys,
  and enter the bounded full reconnect path rather than accepting the old and
  new key ambiguously; and
- expose counters and a redacted reason/generation, never key bytes, nonce,
  replay counter values tied to credentials, or passphrase material.

## USB, firmware, and interface lifecycle

Every acquisition remains in the p028/p029 ownership ledgers. Harden these
ordered transitions:

- build Association Request capability bits from the station's reviewed local
  feature set instead of copying optional capability claims from the selected
  AP, and cover AP-advertised unsupported optional bits in the fake peer;
- send a bounded best-effort Deauthentication frame before teardown whenever
  the transport is usable, while never delaying controlled-port closure, key
  retirement, or radio stop when that transmission fails;

- `IFF_UP`: load/validate/start firmware if needed, arm RX, enter IDLE, and
  accept scan/connect. A failed open leaves the device published but down and
  retryable.
- `IFF_DOWN`: block commands and TX, lower carrier, cancel scan/connect/retry/
  rekey timers, deauthenticate if transport is usable, drain RX/TX/control and
  callbacks, delete keys, scrub secrets, power off, then return.
- firmware stall/error: stop new work, lower carrier, checked-cancel USB
  producers, reset/power-cycle, reload the same pinned blob, and perform a fresh
  handshake. If any retirement is unproven, quarantine the complete object and
  fail visibly rather than free DMA.
- warm rebind or low-power recovery: detect surviving firmware state and apply
  the reviewed RTL8822B RPWM `0xfe58` toggle/acknowledgement sequence before
  reusing the WCPU. Q057 cold-open correctness does not prove this path; cover
  both retained-firmware and fully powered-off cases here with finite waits.
- USB transfer timeout/stall: attribute the endpoint/request and connection
  generation, recover only that transport when safe, and escalate to the same
  full reset path after a bounded retry. Do not reset an unrelated xHCI device.
- unplug: make `wlanN` unobservable through `net_device_gone()`, wake ioctl
  waiters with `ENODEV`, cancel timers/work, join callbacks, release USB/DMA/
  firmware/key/common state exactly once, and preserve unrelated USB storage.
- reinsert: allocate a fresh object and ifindex, reuse the lowest free `wlanN`
  name, and expose an empty scan/connection generation. No old BSS, PMK, key,
  carrier, address, ARP, or route identity follows the removed object.
- shutdown: the network-device barrier completes before USB/xHCI/controller
  teardown. No callback, firmware command, log, or reconnect timer runs after
  terminal shutdown begins.

System suspend/resume is outside the current platform contract. When a real
suspend framework exists it requires a separate Phase rather than treating
shutdown/reopen as equivalent evidence.

## Fault and concurrency matrix

`HW-T34` extends production-source fake-radio and fake-USB fixtures through:

- GTK rekey and pairwise rekey success, retransmission, replay, wrong MIC,
  malformed KDE, CAM exhaustion/program/delete failure, TX drain, atomic swap,
  and no key/PN reinstall;
- link loss during every authentication, association, four-way, data, group
  rekey, pairwise rekey, disconnect, and reconnect transition;
- scan/list/connect/disconnect/status races with close, firmware recovery,
  unplug, and terminal shutdown;
- timeout, stall, short/foreign/duplicate USB completion on each endpoint and
  generation, including a callback deliberately blocked across detach;
- firmware missing/wrong digest/start failure/crash during open and reconnect,
  with bounded retry and complete secret/DMA retention or release;
- five-attempt/30-second backoff exhaustion, explicit cancellation during each
  delay, successful retry reset, and no timer after removal;
- net-device carrier, ARP/route purge, ifindex/name reuse, packet queue, and
  statistics behavior across at least 100 synthetic lifecycle iterations; and
- concurrent xHCI USB-storage traffic so WLAN recovery cannot hide a global
  controller reset or corrupt an unrelated request.

Run the fixture in ordinary, ASan/UBSan, compiler-analyzer, and race-stress
variants. Also run `make -j16`, configured amd64/i386 builds, IDE boot, xHCI
USB-root boot, USB function/binding/concurrent-URB, net-device hotplug, NCM, and
NVMe regressions. `make check` is not used.

## Shared physical checkpoint with WS005 p008

p030 does not own an additional hardware campaign. After every p026--p030
automatic model/fault/race/stress gate and every WS005 command/orchestration
preflight gate passes on one candidate, p030 and
[`ws005-p008`](../../ws005-networking/phase008-archer-physical-acceptance/phase.md)
use the same single provisional physical checkpoint and evidence record.

The shared record includes the Japan-market `Archer T3U Nano` label, explicit
absence of a printed hardware revision, authoritative
VID:PID/bcdDevice/interface/endpoints, firmware upstream revision/digest/
reported version, AP model and firmware, channel/security/width, host
controller, complete artifact manifest, build fingerprint, and exact first
failing transition. Unrelated SSIDs/BSSIDs and every credential are redacted.
The one bounded script supplies both WS004 lifecycle and WS005 complete-command
predicates:

1. Validate the exact identity, firmware, one `wlan0`, scan, association,
   four-way handshake, controlled-port/carrier, DHCP composition, and bounded
   data oracle already owned by p008.
2. Trigger one predeclared controlled link loss while the same interface
   remains up. Require immediate carrier-down, the frozen finite reconnect
   policy, a fresh handshake/key generation, restored L2, and then restored
   WS005 data without manual retry.
3. Run one explicit `net wifi down`, prove carrier/key/timer/child/L3 cleanup,
   then remove the adapter once and prove checked USB/net-device retirement
   while one concurrent USB-storage read remains correct.
4. Record any GTK rekey observed during the bounded run. Forced group/pairwise
   rekey, repeated hotplug, endpoint/firmware faults, and long race campaigns
   remain automatic p030 gates; they do not create extra user-operated runs.

A failure stops the checkpoint, preserves the exact candidate/first boundary,
and returns to a newly planned automatic correction. It is not retried within
p030 or p008. A successful checkpoint promotes the exact candidate manifest to
p008's frozen artifact; p030 references that evidence rather than requesting
another boot.

Final repeatability is owned only by p008: one approved unattended batch of
five consecutive cold boots with the frozen artifact, adapter, AP, commands,
deadlines, and oracle. There is no separate p030 repetition count. The fifth
and final run adds at most ten minutes of bounded bidirectional traffic with a
concurrent USB-storage read; the earlier four use the ordinary bounded p008
payload. Any run failure ends the batch without a counter reset or another
physical request in this Phase.

## Automatic milestone and shared physical closure

- Rekey and transient reconnect preserve strict replay/reinstall protection,
  controlled-port ordering, fresh key/PN state, and bounded diagnostics.
- Close, recovery, unplug, reinsert, and shutdown retire every timer, callback,
  URB, DMA buffer, key, credential, common object, and net-device identity
  exactly once or retain the complete graph for a checked retry.
- All automatic lifecycle/fault/race/storage regression gates pass.

The three conditions above are p030's automatic milestone. They are sufficient
for WS005 p008 to seal one candidate and request its single combined
provisional checkpoint. Q059's earlier one-run normal-path observation is
developmental feedback only and does not satisfy or consume this checkpoint.

- The one shared p030/p008 lifecycle checkpoint passes and p030 references its
  retained redacted evidence; it does not duplicate the human action. DHCP is
  a WS005 predicate in that combined run, not a replacement for p030's L2/
  lifecycle predicates.
- The unchanged frozen artifact passes the five-consecutive-cold-boot campaign
  owned by p008, including no more than ten minutes of sustained traffic in the
  final run; this shared record is the only repeatability evidence required by
  p030.
- The completed claim is explicitly limited to station-mode 2.4-GHz/20-MHz
  WPA2-Personal/CCMP L2. Deferred capabilities remain visible in WS004.

The shared provisional ledger feeds back simultaneously to p028 attach/scan,
p029 secure L2, p030 lifecycle, and p008 DHCP/E2E. The later five-run p008
ledger supplies final physical repeatability. These feedback conditions close
the physical claims but are not inputs to p030's automatic milestone, so the
p008 dependency graph is acyclic.

## Reconsideration boundary

Stop and return to the owning Phase if recovery requires a global xHCI reset,
firmware loses state without a detectable event, hardware cannot prevent key/
PN reinstall, callbacks cannot be drained, the exact adapter descriptor or
identity changes, or repeated physical failure shows a missing radio/
calibration/coexistence requirement. Do not make reconnect unbounded, retain
credentials across device removal, suppress carrier transitions, skip rekey/
replay checks, or expand into 5 GHz/DFS/VHT/DHCP to work around the failure.
