# ws025-p030: 条件付き IMOD 実機比較

日付: 2026-09-07

Phase ID: `ws025-p030`

Status: planned; Priority 1に再選択（2026-09-09）、実装未開始。q122での見送りは履歴として保持。

Parent: [WS025](../ws.md)

依存: ws025-p009、ws025-p025、比較できる実機

追加の先行条件: [ws025-p031](../phase031-driver-layout-style/phase.md) のドライバ整理を完了してから実装する。下記の旧ソースパスは p031 の移行表で解決する。既存の採用条件は維持する。

追加の回帰 gate: [ws025-p032](../phase032-pc98-boot-regression/phase.md) の PC-98 QEMU 起動回復を完了してから実装へ進む。

## 現在の選択

ユーザーがp027〜p030を次に実施する最優先項目として選択した。
旧見送りを継続する扱いではなく、p031残件・現ソース・下記の測定/実機条件を確認してQueue化する。
今回の計画整理で実装や測定を実施済みとは扱わない。

## 目的と境界

IRQ latency と負荷を測定し、既定値を変えるか維持するか決める。

## 変更対象

- `src/drivers/pci-xhci.c`
- `plan/ws004-hardware`

実装時点の source owner と file 名を再確認し、WS024 の移行済みコードを二重実装しない。

## 設計と実装手順

1. 現行値と 0/160/4000 を同一 artifact・同一 topology で比較できる測定設定を用意する。設定・実 readback を記録する。
2. USB2/USB3 storage の read/write/fsync と WLAN/HID 並行を反復し、IRQ/s、CPU、p50/p95/p99、timeout/reset を採る。
3. correctness/回復退行がある値を除き、device/controller 差が大きければ単一 global default を変えない。
4. 既定維持も完了結果にできる。変更する場合は回復/ownership/高位 DMA の既存 gate を再実行する。

[共通 I/O 契約](../io-design.md) と [memory 設計](../memory-design.md) を維持する。

## 受け入れ

- IO11–IO12、REC01–REC06 と実機性能セル。QEMU の同値結果だけで値を選ばない。
- 該当 ID の詳細は [受け入れ行列](../acceptance.md)。変更範囲の focused fixture と supported build を実行し、source/config・counter・結果を記録する。

## 停止・再開と成果物

実機比較ができなければ現行値を維持し planned とする。試験用値をそのまま production default に残さない。

成果物は production patch、必要な fixture、実行後の `results.md`。途中状態は同ファイルに事実・失敗地点・残条件を記し、未実施を PASS にしない。Queue 選択前には実装せず、build/runtime は共有環境で直列に行う。

## q122 adoption decision

Not adopted in the mandatory WS025 implementation. Run real supported topology comparisons at 0/160/4000; keep current 4000 until that evidence exists.
See [effective policy and evidence basis](../phase026-integration-defaults/effective-policy.md).
This records the conditional decision, not completion of this optional phase.
