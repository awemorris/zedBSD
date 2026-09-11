<!-- awesome-plan project=zedbsd record=ws025-p002 -->

既存計画の取り込み。本文はローカルの現行記録、リポジトリ資料へのリンクは基準コミットの履歴です。Issue作成・open状態は実行承認や未完了判定を意味しません。Awesome Planの状態同期導入前のため、実際の状態は本文を参照してください。

元資料: `plan/ws025/phase002/phase.md`

親: [ws025](https://github.com/awemorris/zedBSD/issues/26)

# ws025-p002: BIOS E820 / UEFI memory handoff

日付: 2026-09-07

Phase ID: `ws025-p002`

Status: completed; Queue q089, 2026-09-07. [results](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws025-io-memory-cache/phase002-boot-memory-handoff/results.md)

Parent: [WS025](https://github.com/awemorris/zedBSD/issues/26)

依存: ws025-p001

## 目的と境界

loader が全 RAM の型付き range と boot 所有領域を渡せるようにする。

## 変更対象

- `bootloader/include/amd64-handoff.h`
- `bootloader/pcat/bootzbsd.S`
- `bootloader/uefi/memory-map.c`
- `bootloader/uefi/bootx64.c`
- `src/hal/amd64/bsp-pcat/boot.c`
- `src/hal/amd64/bsp-pcat/handoff-validation.c`
- `src/hal/amd64/locore.S`

実装時点の source owner と file 名を再確認し、WS024 の移行済みコードを二重実装しない。

## 設計と実装手順

1. memory-design.md §3 の ZBL6 v6 を byte layout/static assertions から定義する。BIOS/UEFI の form、flags、entry size/count、既存 prefix と parameter/FB offset を検証する。
2. BIOS E820 の 20/24-byte 応答、SMAP、continuation、無効 entry、overflow/capacity を処理する。AH=88h scalar は明示的 legacy fallback に限定する。
3. UEFI の高位 base/size を保持し、BootServices/Loader の reclaim 時期と boot allocation reservations を渡す。final GetMemoryMap/ExitBootServices の隣接契約を維持する。
4. kernel が range をコピーし、正規化と reservation を検証する。報告値の 1 GiB 切捨てを外す一方、通常 allocator の公開範囲は p005 まで分離して制限する。
5. BIOS shared loader の ELF32、stage2 checksum/size/DOS relocation と v1–v5 読取りを維持する。新しい全 RAM image は loader/kernel を v6 の組で生成する。

[共通 I/O 契約](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws025-io-memory-cache/io-design.md) と [memory 設計](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws025-io-memory-cache/memory-design.md) を維持する。

## 受け入れ

- MEM01–MEM05。高位 range と reclaim 属性が producer/consumer の round trip で一致する。
- 旧 BIOS degraded、新 v6 BIOS、新 v6 UEFI、truncated/overlap/map overflow を区別する。新 loader で高位情報を捨てない。
- 該当 ID の詳細は [受け入れ行列](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws025-io-memory-cache/acceptance.md)。変更範囲の focused fixture と supported build を実行し、source/config・counter・結果を記録する。

## 停止・再開と成果物

map 不完全・ABI 不一致は診断して停止する。欠けた range を一つの連続 RAM と推測しない。

成果物は production patch、必要な fixture、実行後の `results.md`。途中状態は同ファイルに事実・失敗地点・残条件を記し、未実施を PASS にしない。Queue 選択前には実装せず、build/runtime は共有環境で直列に行う。
