# WS005 Phase 008: Archer T3U Nano physical acceptance

Last updated: 2026-09-05

Phase ID: `ws005-p008`

Status: planned; not queued

Parent: [WS005](../ws.md)

Orchestration dependency: [`net wifi` orchestration](../phase007-net-wifi-orchestration/phase.md)

Earlier development checkpoint: [minimum connectivity](../phase009-wlan-minimum-connectivity/phase.md)

Primitive hardening dependency: [p010](../phase010-wifi-primitive-hardening/phase.md)

Managed reconnect dependency: [p011](../phase011-networkd-managed-wlan-reconnect/phase.md)

Tests: [WS005 test index](../tests/README.md)

Target identity: [WS004 p026](../../ws004-hardware/phase026-archer-t3u-nano-identity-firmware/phase.md)

WLAN common/driver dependencies: [p027](../../ws004-hardware/phase027-wlan-uapi-common-core/phase.md), [p028](../../ws004-hardware/phase028-rtl8822bu-usb-scan/phase.md), [p029](../../ws004-hardware/phase029-wpa2-ccmp-l2/phase.md), [p030](../../ws004-hardware/phase030-wlan-lifecycle-hardware-hardening/phase.md), [p041](../../ws004-hardware/phase041-rtl8822bu-5ghz-quality/phase.md), and [p044](../../ws004-hardware/phase044-wlan-async-operation-boundary/phase.md)

## Objective

Accept one precisely identified Japan-market TP-Link Archer T3U Nano as the
first physical WLAN target without turning hardware access into an iterative
development loop. Its label has no printed hardware revision; the exact
retained USB descriptor is the unit's binding authority.

WS005 p009 intentionally performs one earlier, narrow development check to
prove basic scan/connect/DHCP/ping/fetch communication before hardening. That
result is not this Phase's provisional acceptance, consumes none of the
five-run ledger, and carries no reconnect/recovery claim.

After every automatic lower-layer, protocol, orchestration, build, static, and
regression gate passes, p008 permits exactly one provisional physical check.
If that check passes, the identical candidate artifacts become immutable and
one final batch must produce five consecutive successful runs. The final batch
is initiated by one human gate and proceeds without another approval, retry
request, code/config change, or manual success decision between runs.

The marketing name alone is not hardware identity. The FCC V1.0 record is
documentary RTL8822BU family evidence and the independent software mappings
make `2357:012e`/RTL8822BU the target while rejecting the original RTL8828BU
guess. The purchased unit is labelled `Archer T3U Nano`, region Japan, with no
printed revision; its descriptors remain authoritative. p008 does not bind or
accept it until those values and the pinned firmware match the p026 identity
profile.

## Dependencies

- The p009 one-run minimum-connectivity development checkpoint is complete.
- All automatic/model gates from WS004 p026-p030/p041/p044 and WS005 p004-p007
  plus p010/p011 are complete. Their final physical acceptance evidence is supplied only
  through the bounded p008 procedure; p009 remains separately labelled
  development evidence.
- The p003 AF_UNIX peer-credential contract and `root:network` socket policy
  pass their native and lifecycle regressions.
- The supported full build, focused host/native tests, sanitizers/analyzers
  where supported, QEMU networking regressions, wired networkd/fd-3 behavior,
  DHCP transition/rollback tests, and USB storage/concurrency regressions pass.
- A bounded physical procedure, exact adapter, controlled access point,
  nonsecret expected network values, and external data oracle are available.
- Firmware provenance, redistribution, loading, timeout, and device-removal
  behavior are resolved before the image is offered to the physical gate.

## Scope

- identity capture for the exact Archer T3U Nano physical unit, including its
  Japan label, explicit absence of a printed revision, and descriptor profile;
- one provisional attach/scan/associate/DHCP/reconnect/data/down check after
  automatic evidence is complete;
- one combined provisional record feeding the p028 scan, p029 secure L2, p030
  retained lifecycle, p041/p044 asynchronous W52, and p008/p011 orchestration
  owners without separate physical gates;
- promotion of the passing candidate to an immutable artifact manifest;
- one uninterrupted five-run final acceptance batch on those frozen artifacts;
- exact first-failure capture, redaction, artifact hashes, and run ledger;
- retained recovery responsiveness and no leaked USB/driver/network state after
  down, removal, failure, or the end of each run.

## Non-goals

- assuming all products sold as Archer T3U Nano use one USB id or chipset;
- accepting a different Archer model, descriptor profile, clone, or unrecorded
  firmware;
- debugging or changing code during a physical run;
- requesting repeated provisional checks after each automatic change;
- restarting the five-run counter inside p008 after a failed run;
- broad interoperability outside Japan W52 across access points, bands,
  roaming, suspend/resume,
  enterprise authentication, WPA3, throughput, or power-management matrices;
- claiming boot-time Wi-Fi policy not selected by p007, or restoring the
  superseded public per-interface grammar; and
- treating p009's earlier one-run development result as provisional or final
  acceptance, or counting it toward the five consecutive runs.

## Exact hardware identity contract

Before Queue eligibility, retain the expected product/SKU label
`Archer T3U Nano`, Japan region, explicit absence of a printed revision, and the
authoritative candidate descriptor profile. During the one provisional check,
capture and compare at least:

- USB vendor id, product id, and `bcdDevice`;
- device, configuration, interface, and endpoint descriptors, including
  alternate setting, direction, transfer type, maximum packet size, and
  interval;
- manufacturer/product strings and a redacted or hashed serial if present;
- negotiated USB speed, physical port/controller path, and selected driver;
- separately installed firmware filename/version and cryptographic digest,
  with its package, acquisition source, and license record;
- published WLAN interface name and permanent MAC in a privacy-preserving run
  record.

Any mismatch is `hardware identity mismatch`, not a driver failure and not a
reason to broaden the match table during the check. The first check determines
whether the exact unit matches the predeclared target. No acceptance result may
be generalized to another VID:PID, `bcdDevice`, interface/endpoint profile,
firmware digest, label, or physical unit.

## Automatic preflight gate

The provisional request is forbidden until a machine-produced preflight
manifest shows all required automatic evidence green on one candidate build.
At minimum it records:

- source revision and clean/declared worktree state;
- compiler/toolchain identity and complete build result;
- focused WLAN UAPI, crypto/key, driver, primitive, ZNV2, peer-authentication,
  wifi.conf, orchestration, deadline/cancel, rollback, removal, and secret-
  redaction test results;
- sanitizer/analyzer results where supported and explicit unsupported entries;
- QEMU wired/networkd/dhcpc and relevant USB concurrency regressions;
- absence of unresolved expected failure, flaky retry, skipped mandatory test,
  or result produced by an older artifact;
- candidate kernel/image/binary/firmware/config digests and exact boot media.

The automatic preflight is repeatable without the adapter. Any automatic
failure returns to its owning lower Phase. It never consumes the one
provisional physical check.

## Candidate and frozen artifact manifest

Immediately before the provisional check, seal a candidate manifest containing
all acceptance-relevant inputs:

- boot image pathname, byte size, and digest;
- kernel and, if independently replaceable, `/sbin/net`, `/sbin/networkd`,
  `/sbin/ifconfig`, `/sbin/wifi`, and `/sbin/dhcpc` digests;
- build configuration and driver match data;
- the separately installed `rtl8822b-firmware` manifest, installed
  firmware path `/lib/firmware/rtw88/rtw8822b_fw.bin`, 161,240-byte size,
  version 30.20.0, SHA-256
  `a72da690597bfa99d8eb6fc2ab090d18d8ad92ac2befd35db1c9e3662d8d8418`,
  license SHA-256
  `a61351665b4f264f6c631364f85b907d8f8f41f8b369533ef4021765f9f3b62e`,
  the immutable acquisition endpoint
  `https://github.com/endlessm/linux-firmware.git` at
  revision `2f56219d20e4becccd718963fc3bcc671c543ce5`, and official-upstream
  provenance commit `458e40fdbb4dad5134ec230a42df21aea1b5baf8` with WHENCE
  SHA-256 `34f954c7d068ec4fd5fcc216471912dd3cf40ff60a7ffa8d06ff6f9b5999551f`;
- wifi.conf format version, active policy UID class, and initial nonsecret
  global policy state; secrets and
  reusable hashes of weak passphrases are not copied into the public ledger;
- expected adapter identity, host USB controller/port, access-point identity,
  band/channel/security mode, DHCP server identity, and data-oracle identity;
- exact command script, monotonic deadlines, pass/fail predicates, reset rule,
  log destinations, and redaction rule.

The manifest fixes the shared bounds rather than treating them as human
choices: ZNV2 header/request/response and wifi machine output are
32/4096/32768/32768 bytes, a scan list is at most 64 records, diagnostics are
512 bytes, the secret descriptor is fd 4, child termination grace is one
second, scan/direct-connect/DHCP/compound limits are 15/30/10/90 seconds, and
one automatic-selection generation makes at most four profile attempts. The
kernel never runs a high-level reconnect; one p010 `/sbin/wifi connect` child
owns the complete 30-second scan/select/connect retry window.

If the provisional check passes, that exact manifest is promoted to `frozen`.
No source, binary, image, firmware, driver match, command script, deadline,
credential policy, access-point security setting, or oracle may change before
or during the final five runs. A required change invalidates the candidate and
ends p008 without another physical request; the correction and new acceptance
belong to a later approved Phase.

## One provisional physical check

After automatic preflight, request one explicit physical action for the exact
adapter and run one bounded script. This is the single combined provisional
checkpoint for WS004 p028/p029/p030/p041/p044 and WS005 p008/p011, not
separately repeatable hardware gates. Its one ledger records and routes the
p028 identity/firmware/scan evidence, p029 WPA2/CCMP L2 evidence, retained
p030 lifecycle evidence, p041 W52 evidence, p044 asynchronous event evidence,
and p008/p011 DHCP/orchestration evidence. The script
performs, in order:

1. Boot the candidate image cleanly with the target initially absent, capture
   the host USB controller baseline, then attach it by the frozen reset rule.
2. Capture and validate the exact identity contract before allowing the driver
   match; confirm firmware load, endpoint setup, interface publication, and
   bounded readiness without an error/retry storm.
3. With a preprovisioned redacted `auto` profile, run `net wifi enable` once.
   Prove networkd records the authenticated policy UID, discovers all `wlanN`
   in stable order, and permits only one managed association. Run `net wifi
   list` and prove its aggregate view identifies the target interface and the
   controlled W52 SSID/channel/frequency/security without exposing a key.
4. Prove the same global enable generation selects the profile in file order,
   chooses the first eligible stable-order WLAN, reaches secure `IFF_RUNNING`,
   obtains DHCP, and owns the expected address/route/resolver without stale
   prior-network L3 state.
5. Induce one transient radio link loss after success. Require immediate
   kernel carrier-down plus one matching link event, exactly one networkd
   `/sbin/wifi` child for the same selected SSID after rereading the policy
   owner's store, AP restoration within that child's unchanged 30-second
   window, fresh secure L2 authorization, preserved coherent DHCP/L3 state,
   and no retained networkd passphrase.
6. Pass the frozen L2/L3 data oracle: ARP/neighbor resolution, bounded ping,
   and a bounded payload transfer whose length and digest match the source.
7. Induce one longer controlled loss so one 30-second recovery child fails.
   Prove stale L3 is removed, policy remains `auto-searching`, the same scan
   generation does not cause a tight retry loop, and every operation-local key
   copy is wiped. Restore the AP and use the scripted next scan generation to
   prove a later fresh selection can reconnect and reacquire DHCP.
8. Run `net wifi disconnect`, prove `manual-disconnected` suppresses automatic
   reconnection while all managed radios remain up and scanning, then run `net
   wifi connect <SSID>` and prove the manual path uses the saved profile and
   same global interface rule. Finish with `net wifi disable` and prove
   scans/association/DHCP/policy ownership are retired, carrier clears, direct
   recovery commands remain responsive, and removal retains no callback,
   child, key, descriptor, or USB reference.

Every command uses one monotonic deadline and records the first failed stage,
stable error, driver/USB boundary, rollback result, and bounded redacted logs.
A hang, manual workaround, second attach attempt, AP setting change, command
rerun, or success judged without all predicates is a provisional failure.

On provisional failure, stop. Preserve the candidate manifest and first-failure
evidence, label it with the owning p028, p029, p030, p041, p044, p008, or p011
boundary, do not
repair code or widen a hardware match in p008, do not ask the user to try
again, and do not enter the final batch. Return the blocker to a newly planned
automatic Phase.

## Final five-consecutive-run batch

After provisional success and manifest promotion, make one final physical
request which starts an unattended or pre-scripted batch of five runs. Do not
pause for confirmation, interpretation, or a repeated human gate between runs.

Each numbered run must use the frozen artifact, exact adapter, host port/reset
rule, AP, credential policy, deadlines, and oracle, and must independently:

1. start from the frozen clean-boot/reset state and revalidate adapter,
   firmware, driver, and interface identity;
2. execute `net wifi enable` and bounded aggregate `net wifi list` detection of
   the controlled SSID and chosen stable-order interface;
3. prove automatic secure carrier and acquire a fresh valid DHCP
   result, and validate address, route, resolver, and no-stale-state predicates;
4. induce one transient loss and prove a kernel link event starts exactly one
   networkd `/sbin/wifi` child whose own 30-second retry reconnects the same
   selected SSID, with no kernel or daemon-level second retry;
5. pass the bounded neighbor, ping, and digest-checked transfer oracle;
6. execute `net wifi disconnect`, prove `manual-disconnected` suppresses
   recovery while the radios remain up and scanning, then `net wifi disable`;
   prove complete bounded retirement and close the run with no leaked secret
   or live work.

A run counts only when every predicate succeeds without rerunning a command or
the run; the bounded retries internal to one `/sbin/wifi connect` invocation
are part of the predicate. Runs must be
consecutive: any timeout, mismatch, malformed result, association/DHCP/data
failure, cleanup failure, retry, manual intervention, or artifact/environment
change fails the batch immediately. Preserve all completed run records and the
first failed record, but do not reset the count or request the remaining runs
in p008. Diagnosis and a new candidate/five-run acceptance require a later
approved Phase.

## Evidence and redaction

The acceptance record contains the candidate/frozen manifest, automatic
preflight result, provisional result, and one ledger entry for each final run
attempted. Each entry includes timestamps/durations, stage results, stable
errors, USB/interface identity, association security outcome, nonsecret DHCP
and route assertions, transfer size/digest, cleanup outcome, and log digests.

Passphrases, PMKs, raw secret descriptors, full profile contents, and reusable
credential hashes are never retained in acceptance evidence. P011 is verified
to retain only the policy UID/nonsecret connection facts and to wipe each
store-read/fd-4 operation secret before the next state transition.
SSID/BSSID/MAC/serial exposure follows the frozen lab-manifest
redaction rule. Raw bounded logs may be retained in a restricted artifact only
when their secret scan passes; the public plan record contains redacted
evidence and digests.

## Planned execution

1. Assemble the Queue-entry manifest for the fixed contract: target profile,
   artifacts, AP/oracle, reset rule, thresholds, script, predicates, and
   evidence schema.
2. Complete and review the automatic preflight manifest. Return any failure to
   its owning automatic Phase; do not touch the physical gate.
3. Seal the candidate manifest and perform the one combined
   p028/p029/p030/p041/p044/p008/p011
   provisional physical check.
4. On provisional success, promote the unchanged candidate to frozen and make
   one final request for the uninterrupted five-run batch.
5. Accept only five consecutive complete ledger entries. On the first failure,
   stop and preserve evidence for a later Phase without a retry request.
6. Record completion in the WS and test index without broadening the claim
   beyond the exact frozen unit/descriptor/environment.

## Queue-entry evidence checks

These checks instantiate the fixed acceptance contract for the available lab;
they do not reopen product policy or add human checkpoints:

- Record the purchased Japan-market label, explicit absence of a printed
  revision, and compare the authoritative `2357:012e`/RTL8822BU p026 profile,
  `bcdDevice`, interface, endpoint signature, and firmware identity. A mismatch
  stops the one provisional run and returns to a new identity Phase; it never
  widens this target to RTL8828BU or another descriptor profile or unit.
- Import the separately installed `rtl8822b-firmware` entry from p026/p036. Its
  acquisition manifest names only
  `https://github.com/endlessm/linux-firmware.git` revision
  `2f56219d20e4becccd718963fc3bcc671c543ce5`; the evidence retains official
  `linux-firmware` provenance commit
  `458e40fdbb4dad5134ec230a42df21aea1b5baf8`, the frozen blob/license hashes,
  install path, and missing/mismatch behavior. The candidate proves that the
  package was separately installed, not fetched by the base build or kernel.
- Record the complete stable WLAN discovery order and prove the target is the
  first eligible interface reporting the chosen visible profile. Even in the
  one-radio frozen topology, assert exactly one managed connection and no
  hidden public interface selection.
- Record one controlled AP identity/channel at p041's fixed Japan W52 non-DFS
  20-MHz WPA2-Personal/CCMP profile and the scripted transient-success plus
  one-command-exhaustion methods.
  Unsupported security never downgrades.
- Record the DHCP server, subnet/route/resolver predicates, neighbor/ping
  targets, transfer endpoint, payload size/digest, and whether each assertion
  is local-LAN or external. These are fixture coordinates, not selectable
  protocol behavior.
- Record clean boot, attach, USB power/reset, host controller/port, AP reset,
  and per-run isolation so the provisional and all final runs have the same
  reproducible start without a mid-batch human action.
- Carry the fixed 15-second scan, 30-second `/sbin/wifi` connect, 10-second DHCP,
  90-second compound, one-second child grace, and four-attempt auto bounds into
  the script. Carry p044's no-kernel-retry rule, p011's no-retained-passphrase
  rule, one internal same-SSID reconnecting attempt followed by clean
  auto-searching fallback, and no-nested-retry rule; no human retry is
  permitted.
- Produce the candidate/frozen manifest, secret provisioning, raw-log access,
  SSID/BSSID/MAC/serial redaction, evidence-retention, and automatic-preflight
  records before requesting hardware.
- Encode one combined provisional result with
  p028/p029/p030/p041/p044/p008/p011 subrecords.
  On success, promote the unchanged candidate and run one final five-run
  batch without a human gate between attempts; any change or failure ends
  p008 and requires a later Phase.

## Completion conditions

- Every declared automatic gate passes on the candidate artifact with no
  unresolved failure, stale result, or retry-masked flake.
- The one combined provisional check passes every p028/p029/p030/p041/p044 and
  p008/p011 identity, global enable/list/selection, W52 scan, association,
  DHCP, event/userspace reconnect, manual disconnect, disable, removal, and
  cleanup predicate on the exact target.
- The unchanged frozen manifest then produces five consecutive complete final
  run records after one batch request, with no retry, intervention, or
  midstream change.
- Evidence is bounded and redacted, recovery remains responsive, and no secret,
  callback, child, descriptor, key, driver, or USB reference remains live.
- The claim names only the exact adapter identity, firmware, artifact, host/AP
  environment, and tested security mode; it does not imply another Archer T3U
  Nano unit or descriptor profile, or general WLAN interoperability.

## Interruption and resumption

Before Queue selection, attach every Queue-entry evidence record above and
prove all automatic dependencies green. Before the provisional check, an
automatic failure resumes in the owning lower Phase and p008 may later consume
new automatic evidence. After the single combined provisional request is made,
any physical failure or required change ends this acceptance attempt. During
the final batch, the first failed run ends the batch. In both cases preserve
the exact first failure and plan a new bounded Phase; do not insert another
human gate or retry into p008.
