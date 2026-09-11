<!-- awesome-plan project=zedbsd record=ws004-p046 -->

既存計画の取り込み。本文はローカルの現行記録、リポジトリ資料へのリンクは基準コミットの履歴です。Issue作成・open状態は実行承認や未完了判定を意味しません。Awesome Planの状態同期導入前のため、実際の状態は本文を参照してください。

元資料: `plan/ws004/phase046/phase.md`

親: [ws004](https://github.com/awemorris/zedBSD/issues/5)

# WS004 Phase 046: Archer T3U Plus driver extension

Last updated: 2026-09-06

Phase ID: `ws004-p046`

Status: Completed (`q083`, 2026-09-06)

Parent: [WS004](https://github.com/awemorris/zedBSD/issues/5)

Dependency: [completed p045 feasibility](https://github.com/awemorris/zedBSD/issues/136)

Evidence contract: [HW-T43](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws004-hardware/tests/archer-t3u-plus-intake.md)

## Objective

Make the exact `2357:0138` Archer T3U Plus work through the existing RTL8822BU
driver, common WLAN station core and user commands. Retain the accepted
`2357:012e` Nano path and the currently pinned optional firmware package.
The user explicitly requires both 2.4-GHz and 5-GHz useful communication on
the new adapter. Prior q071/Nano evidence is a regression baseline, not this
unit's acceptance. The supplied AP selections and key stay exclusively in the
user-authorized private runtime credential file, never in M/W/P/Q or test output.

## Implemented boundary

1. Add the measured `0138` High-Speed and SuperSpeed identity tuples to both
   the USB ID table and strict binding parser. Retain configuration/interface/
   alternate/endpoint checks and validate the SuperSpeed companion fields.
   Match actual device speed; never represent a SuperSpeed device as High Speed.
2. Retain the USB speed/packet profile in private RTL8822BU transport state and
   pass it to the radio setup. Select RXDMA_MODE `0x1e` for High Speed and
   `0x0e` for SuperSpeed. RTL8822B's existing v1 RX aggregation `0x2005` is
   speed-independent; do not import another chip's v2 aggregation settings.
3. Inspect actual SYS_CFG1 and EFUSE through the existing chip/board parser.
   Check the cut-dependent USB PHY initialization, including the upstream
   cut-D USB3 setting if applicable. Use existing supported RFE tables and
   calibrated channel rules. Cell 1 established cut D/RFE3, unset country
   `ffff`, hardware-enforced plan `a6`. Decode this measured WORLD_ETSI1
   plan's non-DFS W52 subset while retaining the existing worldwide-minimum
   power ceilings and all calibration checks. Unknown/unforced plans and
   contradictory country combinations remain rejected. No JP country value
   is fabricated and no raw board fact is replaced.
4. Verify firmware, management and data TX packet-boundary handling at both
   endpoint sizes. Existing 512-byte padding also avoids 1024-byte multiples;
   retain it only with explicit boundary evidence, or pass the selected packet
   size through the private transport where needed.
5. Keep firmware acquisition under `userland/firmware/rtl8822b/`, with the same
   immutable GitHub revision, digest, license and install path. Update it only
   if a localized failure establishes a need for different approved bytes.
6. Correct the source-localized 5-GHz management-rate defect found during q080
   review: probe/deauthentication frames currently select fixed rate zero
   (1-Mbit/s CCK), including on W52. Select 6-Mbit/s OFDM on W52, retain the
   existing 2.4-GHz rate, and verify the encoded descriptors on both bands.
   The common probe builder must also advertise the supported OFDM rate set
   on the actual 5-GHz scan channel, with capacity for a maximum-length SSID.
7. Implement the upstream USB register-access completion sequence: accesses
   to the ON sections `0000..00ff` and `1000..10ff` require a checked one-byte
   companion write to `04e0`. Preserve the original read buffer and report
   companion failures through the existing control-transfer error path.
   Cell 3 localized scan failure to three missing firmware TX reports;
   this measured protocol omission is a candidate cause, not a proven fix.
   Cell 4 showed 78 received frames and no C2H notifications after this fix.
   Q081 therefore localizes TX queue/endpoint selection, management descriptors
   and firmware-report setup, preserving checked completion and failure paths.
8. Complete the primary-source-verified firmware startup protocol with the
   GENERAL_INFO reserved-page boundary and PHYDM_INFO actual RFE/cut/RF-path
   messages. These are bounded H2C packet transfers without acknowledgement
   requests, not management transmissions or CCX slots. Check queue capacity,
   lengths and the existing open deadline; abort through checked startup unwind.
9. Route bounded C2H records before applying frame-only CRC/rate/PHY status
   semantics. Keep descriptor, payload-offset and aggregate bounds, the
   two-byte notification minimum, and every existing ordinary-frame check.
   Upstream treats C2H as firmware metadata, not a received MPDU. Verify paired
   notification acceptance/frame rejection and truncated-offset rejection.
10. Correct path-A TRX selections for the supported A+B RF configuration,
    matching upstream bitmask semantics. For known Wi-Fi-only boards
    (`rf_board_option != ff` and upper three bits not `20`), initialize checked
    WLAN/BT grants, WLAN path ownership and BB antenna-switch ownership.
    Retain and verify the antenna band selection already present in the
    channel journal before unpausing TX. Preserve unrelated fields and retain current
    behavior for unknown or Bluetooth-capable board options. No Bluetooth
    feature, coexistence scheduler or speculative power change is added.
11. Preserve common CCMP/EAPOL duplicate rejection while treating its
    DATA/EAPOL `EALREADY` result as a per-frame drop in the USB receive callback.
    Continue later aggregate records and rearm RX instead of counting it as a
    USB transport failure. Q081 cell 2 reached 2.4-GHz authorization, DHCP and
    ping, then localized this erroneous recovery during HTTP transfer. Other
    transport/descriptor/management/CCX error behavior remains checked.
12. Replace the source-proven 20-ms register budget mismatch with the vendor
    500-ms cap and explicitly propagate radio/security operation deadlines
    into the private USB callbacks. Clamp primary, companion and permitted
    STALL retry transfers to the same absolute remaining time. Do not retry
    ambiguous timeouts or change unrelated bulk/diagnostic budgets. Q081 cell 3
    completed 2.4-GHz data/down, then hit this limit during reopen scanning.
    Keep read-only terminal status accessible while quarantined, and reuse
    one checked recovery transaction for a live scan-only stopped device;
    a failed restart stays quarantined without an unbounded retry campaign.
13. Localize and correct the normal managed disconnect EBUSY found by q082
    after successful 2.4-GHz data. Retain checked TX/key/association barriers
    and ownership on failure. Trace the exact userland/common/driver stage
    before attributing the unchanged connected status to an inverse failure;
    add nonsecret stage diagnostics when required. Complete transient drain
    under a finite bound while preserving permanent errors and final state.
    The synchronous `wifi disconnect` primitive retries only EBUSY within one
    five-second monotonic window with identity checks and bounded waits. This
    completes legitimate TX drain before networkd processes queued carrier-loss
    events; it does not retry policy/L3 cleanup or change the driver barriers.

The first extension uses the speed already enumerated by the USB stack. It
does not require zedBSD to implement Linux's automatic HS-to-SS mode switch.
Such switching, additional products, new RF front ends, DFS/wider channels and
general WLAN/USB refactoring are outside this Phase.

## Verification contract and execution history

Q080 selects a 90-minute review timebox and at most four exact-device QEMU
launches, each capped at 20 minutes. A correction launch requires a localized
failure and an actual change; no unchanged retry campaign is authorized.
Q080 closed uncleared after its fourth launch. Q081 continues the same phase
with a new 90-minute review and at most four launches under the same bounds,
starting from the RX-positive/C2H-zero finding rather than repeating a trial.
Q081 cell 4 passes both-band data and reopen between bands, but its fixture
omits reopen after the final 5-GHz stop. Q082 closes that observation with a
corrected fixture, the unchanged verified production image, a 30-minute
review and at most one 20-minute launch. It introduces no driver change.
That launch exposed normal managed-disconnect EBUSY after 2.4-GHz data. Q083
uses a 60-minute review and at most three 20-minute launches to localize/correct
that lifecycle failure and complete the same acceptance contract.
One bounded Linux association baseline may check hardware/firmware TX on each
supplied band (60 seconds each, no DHCP or route changes), with the same pin
and target-only restoration. It cannot substitute for zedBSD acceptance.
The initial automatic milestone should include:

- production-linked USB driver fixtures for Nano and both Plus descriptors,
  malformed/mixed profiles, companion validation, attach/unwind and lifetime;
- RTL8822B core/loader/radio/security fixtures covering speed-selected RXDMA,
  applicable PHY setup and 512/1024/1536-byte boundary neighborhoods;
- existing firmware-package and table-import gates if those files change;
- supported serialized x86 builds with `make -j16` and explicit
  `ZEDBSD_CONFIG`, without the aggregate `make check` target;
- one bounded exact-device `qemu-system-x86_64` passthrough cell on the
  authorized host, using a disposable image after all builds finish, with
  current descriptor and image hashes, board identity, pinned-firmware start,
  scan, checked down and host restoration recorded.

Full product acceptance additionally requires the existing userspace connection
flow to reach WPA2/CCMP authorization and useful IP traffic on a controlled AP,
plus disconnect/down/reopen on each supplied band. The runtime AP selection
and credentials are supplied; keep all supplied values out of source,
retained logs and planning books, and passphrases out of command-line arguments.
Use the existing W52 country/calibration
contract, and distinguish 2.4-GHz evidence from W52 evidence.

The first diagnostic may localize a previously unobserved board or transport
gap. Record the failure stage and a concrete resume condition rather than
claiming completion from Linux success or silently expanding to new hardware.
Preserve the separate RTL8156 wired management interface and AX211 device.

## Completion

The measured Plus profiles bind, board and firmware validation pass, relevant
automatic regressions/builds pass, and the exact adapter completes the normal
scan/connection/data/lifecycle path on both supplied bands under zedBSD. Retain explicit limits for
any speed/band not physically exercised. P045's Linux scan alone does not
satisfy these implementation completion conditions.

## Completed evidence

[Q083](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws004-hardware/tests/q083-results.md) completes three ordinary connection/data/
disconnect/down cycles on each supplied band using the exact SuperSpeed
cut-D/RFE3 board. Channel 1 and W52 channel 44 both pass WPA2-PSK/CCMP, DHCP,
9/9 total ping replies per band and three matching 4480-byte HTTP checksums.
Final post-5-GHz reopen advances the completed scan snapshot from generation
33 to 38, followed by down with authentication/association/key/authorization
cleared and error zero. All scoped gates/reviews and supported serialized x86
builds pass. The existing GitHub firmware pin remains unchanged.

P/W/M/Q contain no supplied SSID or key. The host's management/AX211 paths are
preserved, the target is restored unbound, all frozen image hashes are checked,
and credential-bearing guests and remote trial directories are removed.
The measured HS profiles and previous Nano pass automatic regressions; this
new physical acceptance covers SuperSpeed and 20-MHz channel 44 only for 5 GHz.
DFS, wider channels, automatic speed switching and throughput qualification
are not part of completion. Q082's exact initial EBUSY was not reproduced;
the bounded retry branch is proven by deterministic fixtures, and the final
hardware run passes without a retirement error.
