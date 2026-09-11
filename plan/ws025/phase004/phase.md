# ws025-p004: range 別 allocator と DMA 制約

日付: 2026-09-07

Phase ID: `ws025-p004`

Status: completed; Queue q091; evidence in results.md

Parent: [WS025](../ws.md)

依存: ws025-p003

## 目的と境界

高位 RAM と sparse map を扱う allocator/DMA を実装し、p005 の公開 gate を用意する。

## 変更対象

- `src/hal/amd64/page.c`
- `src/hal/amd64/space.c`
- `include/hal/hal.h`
- `src/drivers/dma.c`
- `include/drivers/dma.h`
- `src/kern/vm-reclaim.c`
- `src/kern/vm-object.c`
- `src/hal/amd64/ap-trampoline.S`

実装時点の source owner と file 名を再確認し、WS024 の移行済みコードを二重実装しない。

## 設計と実装手順

1. 実在 RAM extent 別 bitmap/rotor/free summary を early allocation から構築する。PFN/page count/size/rounding を checked wide arithmetic にし、hole 分の bitmap を持たない。
2. 初期 arena、kernel、table と DMA reserve を移管し、free/reserved/allocated の重複と解放漏れを防ぐ。管理 RAM 合計と highest end を分ける。
3. 制約付き hal_pmem allocation を別 API として追加する。既存 request の未初期化 field を増やさず、他 HAL の互換路も範囲を保証する。
4. DMA coherent allocation は mask/boundary/alignment 内を最初から探索する。DMA32 の低位 reserve と normal の高位優先を設け、streaming map は対応外を truncation せず失敗/bounce とする。
5. hal_space_map、page table、VM page/accounting、swap、driver descriptor の 32-bit PA 仮定を点検する。AP 初期 CR3 の低位制約は保持する。
6. extent/word 探索の IRQ-off/最大走査を測る。lock 外探索は同期読取りと最終再検証が整うまで有効化しない。

[共通 I/O 契約](../io-design.md) と [memory 設計](../memory-design.md) を維持する。

## 受け入れ

- MEM07–MEM13。高位強制 allocation、aligned contiguous、free/realloc、exhaustion、並行 alloc/free、hole/予約排除を検証する。
- 32-bit DMA mask に空き低位 page があれば取得できる。低位枯渇時も高位を切詰めて成功返却しない。supported HAL の compile/allocator 回帰が通る。
- 該当 ID の詳細は [受け入れ行列](../acceptance.md)。変更範囲の focused fixture と supported build を実行し、source/config・counter・結果を記録する。

## 停止・再開と成果物

未監査の DMA/VM 経路が残る場合は p005 の通常公開 gate を閉じたまま記録する。

成果物は production patch、必要な fixture、実行後の `results.md`。途中状態は同ファイルに事実・失敗地点・残条件を記し、未実施を PASS にしない。Queue 選択前には実装せず、build/runtime は共有環境で直列に行う。
