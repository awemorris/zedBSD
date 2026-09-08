# ws025-p029: 条件付き UAS driver

日付: 2026-09-07

Phase ID: `ws025-p029`

Status: planned; explicitly not adopted in WS025 integration (q122); implementation not started.

Parent: [WS025](../ws.md)

依存: ws025-p019、ws025-p024、UAS 対応実機と descriptor

追加の先行条件: [ws025-p031](../phase031-driver-layout-style/phase.md) のドライバ整理を完了してから実装する。下記の旧ソースパスは p031 の移行表で解決する。既存の採用条件は維持する。

追加の回帰 gate: [ws025-p032](../phase032-pc98-boot-regression/phase.md) の PC-98 QEMU 起動回復を完了してから実装へ進む。

## 目的と境界

BOT と別 owner の UAS driver を実装する。

## 変更対象

- `src/drivers (新 UAS owner)`
- `include/drivers/usb.h`
- `src/drivers/pci-xhci.c`
- `src/kern/disk.c`

実装時点の source owner と file 名を再確認し、WS024 の移行済みコードを二重実装しない。

## 設計と実装手順

1. 実機の protocol/interface/pipe/stream capability を記録し、対応対象を固定する。印字された製品名だけで決めない。
2. command/status/data の owner、tag/stream、task management、queue 上限を設計し、descriptor parser と同期 depth 1 から実装する。
3. USB/disk の既存 generation/async/SG 契約に接続し、reset/cancel/timeout/out-of-order を受け入れてから多重化する。
4. 選択失敗時の BOT fallback が device の interface 切替え契約内で可能か確認し、live DMA を残して切替えない。

[共通 I/O 契約](../io-design.md) と [memory 設計](../memory-design.md) を維持する。

## 受け入れ

- 専用 descriptor/protocol fixture、ASYNC/SG/REC/FLUSH の該当セル、実機 read/write/fsync/再接続。
- 該当 ID の詳細は [受け入れ行列](../acceptance.md)。変更範囲の focused fixture と supported build を実行し、source/config・counter・結果を記録する。

## 停止・再開と成果物

実機未入手なら parser 以外の production 実装を開始せず planned に置く。BOT 並列化として代用しない。

成果物は production patch、必要な fixture、実行後の `results.md`。途中状態は同ファイルに事実・失敗地点・残条件を記し、未実施を PASS にしない。Queue 選択前には実装せず、build/runtime は共有環境で直列に行う。

## q122 adoption decision

Not adopted in the mandatory WS025 implementation. Obtain target UAS descriptors and stream/pipe capabilities with a concrete hardware acceptance path.
See [effective policy and evidence basis](../phase026-integration-defaults/effective-policy.md).
This records the conditional decision, not completion of this optional phase.
