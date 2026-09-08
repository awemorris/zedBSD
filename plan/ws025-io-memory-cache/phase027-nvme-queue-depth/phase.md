# ws025-p027: 条件付き NVMe 多重発行

日付: 2026-09-07

Phase ID: `ws025-p027`

Status: planned; explicitly not adopted in WS025 integration (q122); implementation not started.

Parent: [WS025](../ws.md)

依存: ws025-p019 と queue depth が律速である測定

追加の先行条件: [ws025-p031](../phase031-driver-layout-style/phase.md) のドライバ整理を完了してから実装する。下記の旧ソースパスは p031 の移行表で解決する。既存の採用条件は維持する。

## 目的と境界

NVMe の native async queue を利用して device 待ちを重ねる。BOT へ適用しない。

## 変更対象

- `src/drivers/pci-nvme.c`
- `src/kern/disk.c`
- `src/drivers/dma.c`

実装時点の source owner と file 名を再確認し、WS024 の移行済みコードを二重実装しない。

## 設計と実装手順

1. まず queue depth 1 の同等 adapter と counter を入れ、CID/slot/generation/PRP の owner を固定する。
2. 実機上限内の bounded queue を公開し、複数 submit/out-of-order completion と interrupt/wakeup を実装する。
3. timeout/reset/abort で同じ CID の古い completion を新要求に使わず、PRP/backing を retirement まで保持する。
4. fsync の frontier と read/write 公平性を検証し、queue depth 別の throughput/p95/CPU/失敗率で既定値を選ぶ。

[共通 I/O 契約](../io-design.md) と [memory 設計](../memory-design.md) を維持する。

## 受け入れ

- ASYNC01–ASYNC08、FLUSH02–FLUSH06 と NVMe fixture/実機セル。depth 1 の回帰と selected depth の改善を記録する。
- 該当 ID の詳細は [受け入れ行列](../acceptance.md)。変更範囲の focused fixture と supported build を実行し、source/config・counter・結果を記録する。

## 停止・再開と成果物

実機/律速の根拠がなければ planned のまま残す。p026 の mandatory 条件へ黙って追加しない。

成果物は production patch、必要な fixture、実行後の `results.md`。途中状態は同ファイルに事実・失敗地点・残条件を記し、未実施を PASS にしない。Queue 選択前には実装せず、build/runtime は共有環境で直列に行う。

## q122 adoption decision

Not adopted in the mandatory WS025 implementation. Measure a depth-1 bottleneck on target NVMe hardware before selecting bounded queue depth.
See [effective policy and evidence basis](../phase026-integration-defaults/effective-policy.md).
This records the conditional decision, not completion of this optional phase.
