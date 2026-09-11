# ws025-p003 実装・検証結果

Date: 2026-09-07. Queue: q090. Status: completed.

## 結果

RAM を `0xffff800000000000` からの専用 64 TiB 窓へ分離した。
この窓全体を present にせず、typed map の usable / boot-reclaim のみを配置する。
kernel image の VMA/LMA は維持し、image 変換と RAM 変換を分離した。
RAM 変換は present leaf と物理アドレスの一致を確認し、hole・別窓・範囲外を拒否する。
CPU physical width と窓の上限を超える RAM は診断して停止する。

RAM は原則 NX、text/rodata の RAM alias は read-only。
image は実際の kernel extent だけを map し、従来の広い image alias を残さない。
VGA/ROM は `0xffffffffc1400000` の uncached 窓、低位 identity は AP trampoline の
1 page に限定した。console と graphics font の両方の参照先を変更した。
user root は upper-half 256 entries を共有し、将来の vmap を含む空の PDPT も先に所有する。
process teardown の private table リストに shared table を登録しない。

## 初期 table arena

通常 heap/physical allocation を呼ばず、初期 bitmap の free page を予約へ移す専用経路を使う。
探索は monotonic cursor とし、各 page の所有権は予約 run として記録する。
`amd64_early_reservation` が p004 の allocator への移管情報になる。

低位が不足したら、すべての table page の RAM alias が既に構築済みか確認する。
page-table walk の stack から戻った後に部分 CR3 を有効化し、保持していた bootstrap alias を
使い続けないようにしてから、既に mapped な高位 usable RAM に拡張する。
boot 所有領域と既存 arena を除外する。table 数と予約 bytes の一致を起動時に検査する。
拡張不能なら要求した 4096 bytes、確保済み table 数、mapped bytes と段階を出して停止する。

一般 allocator はまだ 1 GiB 以下。p003 の高位使用は direct map と必要時の table arena のみ。
user の通常 allocation、DMA mask 対応と高位公開は p004/p005 が担当する。

## Cache 属性の判断

UEFI の属性は capability として保持し、通常 RAM は WB capability を要求する。
WB を提供しない特殊な RAM は停止診断とし、image/user/table-walker と異なる cache type の
RAM alias を作らない。異なる cache type で同じ PA を alias する実装は使用しない。
根拠は [Intel SDM Volume 3A の memory cache control](https://cdrdv2-public.intel.com/812386/253668-sdm-vol-3a.pdf)
と [UEFI GetMemoryMap の属性定義](https://uefi.org/specs/UEFI/2.10_A/07_Services_Boot_Services.html)。
これは RAM 容量の診断用 cap ではなく、現在の HAL の cache policy の適用条件。
MMIO/ROM/framebuffer は RAM 窓から除外する。

## 検証

- `temp/p003/host-final`：production mapper と既存 memory/handoff fixture を通常・ASan/UBSan で実行、全 PASS。
  1/4 GiB 境界、2 MiB/4 KiB leaf、hole、RO/属性境界、CPU 幅、窓上端、overflow、collision、
  table 確保途中の failure と再開を検証。
- `final-checks.json`：amd64/pcat/pc98 の supported build と BIOS/UEFI × 256/16384 MiB の
  USB-root / SMP4 / login / sysctl が全 PASS。
- `arena-final-16384`：test-only low arena を 275 pages に制限。部分 CR3 切替え後、
  low=1126400 bytes、high=28672 bytes、3 runs、282 tables で起動 PASS。
  通常 image はこの制限を含まない。
- `diagnostic-final.json`：low arena 1 page で `NEED bytes=4096` と停止診断を確認。
  診断文の追加後に通常 build を復元し、`restored-uefi-256` の login まで PASS。
  他の行列は診断文追加直前の同じ mapping/allocator 処理に対する結果。
- 16 GiB の BIOS/UEFI は PA 1073741824 と 4294967296 の CPU read と direct round-trip が PASS。
  image と RAM の変換区別、RAM 側 W^X、arena 所有権、低位 CR3 を起動時に検査する。

| 通常構成 | mapped RAM bytes | arena bytes | tables |
| --- | ---: | ---: | ---: |
| BIOS 256 MiB | 267898880 | 1060864 | 259 |
| UEFI 256 MiB | 261795840 | 1093632 | 267 |
| BIOS 16 GiB | 17179332608 | 1122304 | 274 |
| UEFI 16 GiB | 17173229568 | 1155072 | 282 |

全 runtime は使い捨て image を使い、元 image の SHA-256 不変を検査した。
個別 command/log/source hash は git-ignored `temp/p003/` に保存。
`hw.memory.stats` の mapped は窓の span から実 mapped RAM 合計へ変更した。
allocator の span/予約/公開量の整理は p004 の結果で別に示す。

MEM06 と MEM07–MEM09 の mapping 部分を完了。allocation、高位通常公開、
動的 vmap の更新・TLB はそれぞれ p004/p005/p023 に残る。
