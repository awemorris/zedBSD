# ws025-p028: 条件付き user page 直接 I/O

日付: 2026-09-07

Phase ID: `ws025-p028`

Status: planned; explicitly not adopted in WS025 integration (q122); implementation not started.

Parent: [WS025](../ws.md)

依存: ws025-p022–p024 とコピー律速の測定

## 目的と境界

必要な workload で page-vector I/O を user buffer へ接続する。

## 変更対象

- `src/kern/syscall.c`
- `src/kern/file.c`
- `src/kern/vmspace.c`
- `src/kern/vm-object.c`
- `src/drivers/dma.c`

実装時点の source owner と file 名を再確認し、WS024 の移行済みコードを二重実装しない。

## 設計と実装手順

1. 既存 bounce を adapter として残し、対応 file/device の page-vector operation を限定公開する。
2. user pin、COW、content lease、DMA map の異なる寿命を管理し、aligned/boundary/partial/fault/short result を扱う。
3. unmap/truncate/exit/cancel と競合しても DMA backing を変更/解放しない。cache coherence と書込み snapshot を保証する。
4. memcpy bytes/CPU が実測で減るか比較し、USB の待ち時間だけが律速なら有効化しない。

[共通 I/O 契約](../io-design.md) と [memory 設計](../memory-design.md) を維持する。

## 受け入れ

- SG01–SG05、ASYNC01–ASYNC08、IO04–IO06、CACHE03–CACHE09 と direct I/O 専用 fault/mapping セル。
- 該当 ID の詳細は [受け入れ行列](../acceptance.md)。変更範囲の focused fixture と supported build を実行し、source/config・counter・結果を記録する。

## 停止・再開と成果物

pin だけで DMA が可能とみなさない。効果または契約の根拠が不足したら既存 copy 経路で運用する。

成果物は production patch、必要な fixture、実行後の `results.md`。途中状態は同ファイルに事実・失敗地点・残条件を記し、未実施を PASS にしない。Queue 選択前には実装せず、build/runtime は共有環境で直列に行う。

## q122 adoption decision

Not adopted in the mandatory WS025 implementation. Measure a remaining user-copy CPU bottleneck and define the complete pin/COW/unmap/cancel contract.
See [effective policy and evidence basis](../phase026-integration-defaults/effective-policy.md).
This records the conditional decision, not completion of this optional phase.
