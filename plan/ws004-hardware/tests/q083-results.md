# Q083 Archer T3U Plus retirement completion

Last updated: 2026-09-06

Status: completed

Phase: [ws004-p046](../phase046-archer-t3u-plus-driver/phase.md)

## Starting evidence

[Q081 cell 4](q081-results.md) proves both-band authorization, DHCP, ICMP,
HTTP checksum and down. [Q082](q082-results.md) used the same frozen image
with a stronger final-reopen fixture. Its sole launch passed 2.4-GHz data,
then `net wifi disconnect` returned EBUSY (17). Subsequent WLAN status stayed
connected, authenticated, associated, keyed and authorized with error zero;
ifconfig retained UP/RUNNING but no longer had an IPv4 address. The host,
adapter, observer and image restoration checks passed and guest.img was deleted.

The exact failed stage is not established by the old generic networkd message.
The source deliberately returns EBUSY from checked driver inverse operations
while admitted TX/report ownership or hardware queues remain active, but common
retirement would first lower carrier and retain failed/pending cleanup intent.
That does not match the unchanged live L2 snapshot, absent an unobserved later
state transition. No driver barrier is weakened on this evidence.

Networkd runs one machine-mode wifi disconnect with a 10-second child lifetime
bound. Wifi performs one disconnect ioctl. Networkd then attempts owned L3
cleanup even when that child fails, retains the connection ownership token on
error, and does not schedule a retry from the existing connected policy state.
L3 cleanup removes only its owned address/route/resolver and does not lower
carrier. A diagnostic must distinguish identity validation, disconnect-child
failure and L3 failure before selecting the correction.

## Finite verification

Q083 permits a 60-minute review and at most three 20-minute exact-device
launches. The first source change adds failure-only numeric diagnostics at
the networkd retirement stage and the USB disconnect-ioctl return. No frame,
SSID, BSSID, credential or child output is logged. Focused gates and supported
serialized x86 builds precede each changed image. The accepted firmware pin,
measured SuperSpeed profile and 20-MHz W52 scope remain unchanged.
One diagnostic guest may run three consecutive dual-band cycles under the
same 20-minute deadline, stopping at its first failure. This exercises the
intermittent data-to-disconnect boundary while preserving the original error.

Launches: 1 / 3. Runtime result: PASS; remaining launches are unused.
The acceptance contract required both-band normal data/teardown, fresh reopen between
bands and after the final 5-GHz stop, complete down, and checked restoration.

## Source-supported retirement correction

Further review resolves the apparently inconsistent terminal snapshot. Once
common retirement returns EBUSY, it has lowered carrier and recorded failed
cleanup intent. Networkd retains its old connected policy on the child error.
A queued matching carrier-down event then switches that policy to reconnecting,
and `recover_managed_wlan` can issue a new CONNECT ioctl. That reconnect restores
WPA2/carrier without rerunning DHCP, so the already-cleared L3 state remains
empty. The later connected snapshot therefore does not establish a pre-ioctl
failure, and the exact initial EBUSY still requires runtime observation.

Driver fixtures already prove that inverse operations legitimately return
EBUSY until admitted TX/report ownership drains. The user-facing disconnect
primitive claims synchronous completion but issues the ioctl only once. Q083
corrects that source-supported gap with EBUSY-only retries of the same primitive
under one five-second monotonic window, shorter than networkd's ten-second
child ceiling. It uses bounded wait slices, rejects identity replacement and
permanent errors, and publishes success only for a checked idle/down response.
It does not repeat networkd policy/L3 cleanup or weaken hardware barriers.
Completing the transient retirement before returning to networkd lets the
existing manual-disconnected state take effect before queued carrier events.
The retry-admission window does not interrupt an already-running kernel ioctl.

## Automatic gates and candidate identity

The USB diagnostic, networkd stage diagnostic and wifi command fixtures all
pass ordinary, ASan/UBSan and GCC analyzer gates. The wifi fixture covers 18
new retirement cases, including transient/permanent EBUSY, a shared deadline,
stopped-clock bound, identity replacement, SIGINT/SIGTERM, permanent errors,
late checked success and invalid terminal responses. The two diagnostics pass
independent review; root reviewed the retry source and its final mutation-side
interrupt check. All three configured x86 image builds pass with `make -j16`
after the retry change, and the updated userland fixture is embedded and hashed.

- Production image SHA-256:
  `07dd66442d9335a1803950ed08045ee02d304d6c8e651cd50b1bc0edde498a57`.
- Fixture SHA-256:
  `cac4aa78181153a3e7894319fea2be5ad242e638534203078335eb1a59d85705`.
- Runtime image SHA-256:
  `512d4353854d55a470891c3ac03c49b755295472246916112d3a882eb95d712b`.
- Runner SHA-256:
  `b91e260f925ff141d9d6af180a87cf8e1f24e3cb2ae7f1dc6ab81397718b0184`.
- Observer remains
  `f00505128a591cadf760ba621263a8b7a4b57b51ef977bc2c88ca1c590143700`.

The first cell selects three diagnostic cycles, bounded by the same 20-minute
guest lifetime and first-error stop. Its completed result follows below.
Independent review of the final wifi retry and all 18 fixture cases is clear.
The supplied-value exclusion check passes across 745 planning/source/evidence
files before launch; the private credential file remains ignored by Git.

## Terminal acceptance: three complete cycles and final reopen

The exact SuperSpeed device passed all six ordinary user connection flows:

| Band | Channel / frequency | Completed cycles | WPA2-PSK/CCMP + DHCP | ICMP replies | HTTP checksum | Disconnect / down |
| --- | --- | --- | --- | --- | --- | --- |
| 2.4 GHz | 1 / 2412 MHz | 3 / 3 | pass each cycle | 9 / 9 | 4480 bytes, `4004478192`, each cycle | pass each cycle |
| 5 GHz | 44 / 5220 MHz | 3 / 3 | pass each cycle | 9 / 9 | 4480 bytes, `4004478192`, each cycle | pass each cycle |

After the final 5-GHz stop, the pre-enable snapshot generation was 33. Reopen
produced a fresh completed snapshot at generation 38. Final disable returned
`state=down scan=complete administrative=down authenticated=no associated=no
key=no authorized=no retries=0 error=0`. The runner and guest commands exited
successfully. Evidence is retained in
[result.json](../temp/q083-runtime/cell1/result.json),
[console-redacted.log](../temp/q083-runtime/cell1/console-redacted.log) and
[summary.json](../temp/q083-runtime/cell1/summary.json).

No disconnect-ioctl, managed-cleanup, control, channel-failure or recovery-start
diagnostic appeared. Therefore the hardware run did not force the new EBUSY
retry branch; the deterministic command fixture proves that branch, while the
hardware run proves the resulting ordinary command/data/lifecycle behavior.
Q082's exact first EBUSY source was not reproduced and is not claimed as
conclusively identified. No further unchanged trial is needed for acceptance.

The observer records 357 C2H first records and 3878 ordinary first records,
with all 61,280 observed vendor controls successful and a largest host control
interval of 281 microseconds. It stopped without error. These are bounded host
USB observations, not aggregate frame totals or guest latency guarantees.

QEMU exited, every restoration flag passed and the credential-bearing guest
was deleted. Final independent host checks verify `2357:0138` present/unbound
at 5000 Mb/s, wired RTL8156 still using r8152 with its management route, and
AX211 still using iwlwifi. All completed remote q080/q081/q082/q083 temporary
directories were removed after local evidence retention. No host firmware,
network configuration or secret guest disk remains from the trials.

P046 and Q083 are completed. The actual acceptance is QEMU/KVM USB passthrough
of this SuperSpeed cut-D/RFE3 unit, 2.4-GHz channel 1 and 20-MHz W52 channel 44.
Binding/speed fixtures also cover measured High-Speed and the previous Nano,
but no new physical High-Speed or Nano acceptance is claimed. Other W52 data
channels, DFS, wider channels, automatic HS-to-SS switching, native-controller
qualification and throughput benchmarking remain outside this result. The
separate prior Nano RF-quality issue is not closed by this unit's evidence.

Final local checks preserve all five frozen image/runner/observer identities,
confirm supplied-value exclusion across 752 planning/source/evidence files,
and retain private credential permissions 0700/0600. `git diff --check` passes;
HEAD remains unchanged and no commit was made. Independent final document
review agrees with the runtime and restoration artifacts.
