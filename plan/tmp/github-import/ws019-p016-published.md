<!-- awesome-plan project=zedbsd record=ws019-p016 -->

既存計画の取り込み。本文はローカルの現行記録、リポジトリ資料へのリンクは基準コミットの履歴です。Issue作成・open状態は実行承認や未完了判定を意味しません。Awesome Planの状態同期導入前のため、実際の状態は本文を参照してください。

元資料: `plan/ws019/phase016/phase.md`

親: [ws019](https://github.com/awemorris/zedBSD/issues/20)

# WS019-p016: retained boot-source identities

Date: 2026-09-09
Status: completed (q131); see [results](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws019-installation/phase016-boot-source-provenance/results.md)
Parent: [WS019](https://github.com/awemorris/zedBSD/issues/20)

## Scope

UEFI firmware実行ESPと選択config/kernel volumeを区別してhandoffへ保存する。selected FAT serialだけを両者の証拠にしない。既存sysctlのread-only公開としてvalidity、partition schemeと安定partition identityを返す詳細ABIを確定して実装。configured boot0による上書きと分離する。未対応boot方式はunknownを返し、installerが推測しない。

## Acceptance

使い捨てQEMUでESPとpayload分離、別config volume、boot0 override、曖昧候補、identity欠落を確認。source artifactsの安定性をコマンド間で保証できない場合は具体的な不足をunclearedとして残す。

Timebox: 120 active minutes per execution queue. Queue投入前に詳細ABI・fixtureを確定する。未完了はunclearedとして他の独立作業へ進む。

## Source findings after p015 (read-only preparation)

- bootloader/uefi/bootx64.c discover_config_volume already parses the loaded-image device path and retains the selected config volume path separately. Preserve each partition signature by value before firmware resources expire; do not store firmware pointers in the kernel interface.
- Configuration selection already counts matches and warns while choosing the first. Preserve the count for installer refusal of ambiguity; do not change ordinary boot selection as an incidental installer change.
- bootloader/include/amd64-handoff.h V6 carries memory ownership after the V5 parameter prefix. Append a separately versioned UEFI successor rather than silently alter the V6 layout. BIOS V6 and its memory acceptance remain intact.
- src/hal/amd64/bsp-pcat/boot.c currently saves only the config FAT serial as boot UUID. Keep that existing root-selection behavior and add independent retained source identities. BIOS/non-UEFI exposes unavailable provenance, not fabricated selectors.
- Existing sysctl has string/number leaves and a userland renderer. Prefer read-only kern.boot leaves for source selectors/validity/match count so Noct can query an existing command. Exact public/raw record sizes and parser validation are frozen at Queue entry.

These are preparation notes, not implementation or acceptance evidence.

## q131 implementation contract

UEFI V7 appends an 88-byte pointer-free provenance record to the unchanged V6
prefix. Record: version and config match count (uint32 each), then two 40-byte
partition identities (scheme/index uint32, first LBA/block count uint64, raw
16-byte signature). The loader copies firmware and selected-config identities
before ExitBootServices. Existing config FAT UUID continues to select boot0.

The kernel validates and copies the record into generic boot ownership, then
formats GPT signatures as PARTUUID selectors. Unsupported MBR provenance is
reported unavailable; it must not prevent an otherwise supported MBR boot.
Missing legacy provenance gives unavailable selectors and zero matches.
Malformed V7 record fails handoff validation; incomplete/unknown forms are not
silently advertised as known identities. V6 BIOS/memory layout stays unchanged.

Read-only two-component sysctl OIDs KERN_BOOT_FIRMWARE=5,
KERN_BOOT_CONFIGURATION=6, KERN_BOOT_CONFIG_MATCHES=7 expose
kern.boot.firmware_partition, kern.boot.config_partition (NUL-terminated
selector strings, or unavailable), kern.boot.config_matches (uint64).
Noct uses the existing sysctl command, whose string renderer is extended.
No new command or private helper is needed.

Host acceptance uses actual loader path parser and generic boot consumer,
valid/truncated/zero-GUID/wrapped geometry/version cases, V7 envelope classifier,
and legacy absence. QEMU uses fresh ordinary USB boot copies: separate ESP and
config UUIDs, boot0 override, extra unrelated disk, duplicate same-disk config,
and missing provenance with the retained old V6 loader artifact. Source hashes
and expected GPT identities are recorded. Build supported x86 variants with
explicit configs. No claim that these selectors freeze contents across commands.
