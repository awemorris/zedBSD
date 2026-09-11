# ws025-p002 実装・検証記録

Date: 2026-09-07. Queue: q089. Status: completed。

## 実装した契約

v6 は v5 BIOS/UEFI の prefix を維持し、末尾に 64-byte memory envelope を置く。
range は 32 bytes（64-bit base/size/attributes と 32-bit type/flags）、
boot allocation は 24 bytes（64-bit base/size と 32-bit owner/lifetime）。
共通 envelope の全 field offset と各構造体 size を header で検査する。
BIOS と UEFI の flags の位置は異なるため、version と exact size で分類してから読む。

BIOS の E820 応答検証関数はローダ自身から呼ぶ。20-byte 応答には有効属性を補い、
24-byte の disabled entry は無視する。opaque token の数値増加は要求しないが、
直前と同じ非ゼロ token は拒否する。成功後の CF は終了、最初の CF だけが
明示的な v5 degraded fallback となる。最大 256 個の raw record と normalized range を
別々に制限し、超過は停止する。循環 token も有限容量で停止し、部分 map を渡さない。
E820 の type 5 は unusable として reserved にし、共通型の MMIO と混同しない。

共通正規化は allocation-free の bounded boundary sweep。
usable/boot-reclaim はページ内側、保護範囲は外側へ丸め、BIOS の重複は強い予約を優先する。
同じ優先順位で属性が矛盾した部分は reserved / mixed とする。入力順で結果が変わらない。
穴は出力しない。失敗時は count=0 のままとし、不完全な出力を公開しない。

UEFI は Loader/BootServices を boot-reclaim として保持する。
runtime/NV 属性、未知型は reserved とし、Attribute の 64 bit を保存する。
UEFI の重複は拒否する。最終 GetMemoryMap と ExitBootServices の間に割当ては挟まず、
退出成功後に最終 snapshot を正規化する。
kernel、低位 bootstrap block、EFI loaded image、raw map pool を予約する。
BIOS は kernel、loader segment、bootstrap tables、transition stack を予約する。

consumer は初期 1 GiB 内の配列アドレスを検査してから自前の配列にコピーし、
range の整列・順序・幅、予約の owner/lifetime と対象物の包含を検査する。
UEFI prefix と extension に重複する値は一致を要求する。
全 usable 終端の報告を 1 GiB で切らず、allocator の上限は別に維持する。
ページ数は 64-bit のまま上限と比較し、比較前の uint32_t 切捨てを防ぐ。
boot-reclaim はこの Phase では公開しない。
`A64 MEMORY` は source / usable / boot_reclaim / highest_usable / allocator を表示する。

## 確認済みの途中結果

- production-linked host fixture: 旧 parameter handoff、旧 UEFI normalizer、新 normalizer、
  BIOS/UEFI adapter と v6 ownership を通常・ASan/UBSan で実行。
  E820 応答の 20/24-byte、SMAP/長さ、停滞/減少 token、CF 終了、容量超過も注入した。
- v6 UEFI / 4 GiB / SMP4 / xHCI USB root で既存 I/O baseline が PASS。
- v6 BIOS / 4 GiB / SMP4 / xHCI USB root でログインと `hw.memory.stats` が PASS。
  usable=4294430720、usable_highest_end=6442450944、mapped=1073741824。
  これは高位情報の保持の証拠であり、高位ページの通常割当ての証拠ではない。
- amd64 の build、stage2 checksum/size、EFI artifact の通常チェックが PASS。
  最終 source に対する再検証と pcat/pc98 build は下記の完了記録まで未完扱い。

## 起動検証で修正した点

E820 を導入すると、BIOS の BDA 0x40e の読み取りが従来の reserved-only 検査で拒否された。
低位 1 MiB は引き続き allocator から予約しているため、BIOS の BDA/EBDA/ROM 探索を
この範囲に限定して許可した。ACPI table 本体の高位検査は型付き map に従う。

最初の専用 runner は CI 構成に合わない IDE 接続で root disk を見失った。
runner を既存 baseline と同じ xHCI USB に修正した。これを kernel の成功には数えない。
失敗ログも temp/p002 に保持した。

## 証拠と残り

再利用 fixture は [tests](../tests/)。個別 command/log と使い捨て image は
`../temp/p002/`（git ignored）。受け入れ runner は元 image の SHA-256 が変わらないことを確認する。

MEM01/02/04 と MEM05 の v6/legacy ABI 部分を本 Phase で担当する。
MEM03/05 の allocator 公開、高位 PFN 使用、全旧新 artifact の最終統合は p005 に残す。
低 RAM BIOS/UEFI と supported x86 builds の最終結果を記録後に q089 を閉じる。

## 最終確認

`temp/p002/final-checks.json` に全 command と終了値を記録した。
`host-accepted` の 4 fixture × 通常/sanitizer は全 PASS。
amd64 (`build-final-amd64.log`)、pcat、pc98 の `make -j16` は全 PASS。
`final-{bios,uefi}-{256,4096}` の 4 セルは USB root / SMP4 のログインと sysctl まで全 PASS。
元 image の SHA-256 は各セルで不変。最終 source/artifact の hash を同じ temp に保存した。

| firmware / MiB | usable bytes | boot-reclaim bytes（未公開） | usable highest end | allocator initial bytes |
| --- | ---: | ---: | ---: | ---: |
| BIOS / 256 | 267898880 | 0 | 268296192 | 262438912 |
| UEFI / 256 | 209817600 | 51978240 | 266596352 | 209166336 |
| BIOS / 4096 | 4294430720 | 0 | 6442450944 | 1067884544 |
| UEFI / 4096 | 4236349440 | 51978240 | 6442450944 | 1051598848 |

全セルの現行 direct-map span は 1073741824 bytes。
UEFI の boot-reclaim は allocator に戻していないため、従来の usable 合計に混ぜない。
BIOS stage2 raw は 0xd000 の link-time 上限内で、共有 i386 build の checksum/size gate も通過した。

q089 / p002 を完了とし、依存する p003 に進む。
旧 kernel と新 loader の全実 image 組合せおよび高位 allocator は p005 の統合行列に残す。
