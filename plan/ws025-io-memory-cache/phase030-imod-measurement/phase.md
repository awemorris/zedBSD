# ws025-p030: 条件付き IMOD 実機比較

日付: 2026-09-07

Phase ID: `ws025-p030`

## q143 finite implementation contract

Connect the existing `ZEDBSD_XHCI_IMOD` override to the private USB/HID runner.
Validate values before building, retain exact build commands and hashes, and
build each value in a separate directory. A maintained comparator uses QMP PCI
discovery to locate xHCI BAR0, reads RTSOFF from MMIO, and then reads the actual
interrupter-zero IMOD register while the unchanged storage/HID campaign runs.
Require its low 16 bits to match 0, 160 or 4000 respectively. Record QEMU runtime
and existing functional oracle results, not a physical IRQ latency claim.
Do not change production default or infer untested WLAN/USB2/3 combinations.

Status: uncleared (q143); 比較機構実装済み。0/160/4000の実レジスタ・USB/HID比較合格。計測とwrite/fsyncセルは残る。

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

## q125 現ソースに合わせた詳細化

ユーザーのPriority全件自走指示により着手条件を確認。完了できないPhaseはunclearedとし他WSへ進む。

対象: src/drivers/pci/pci-xhci.c、plan/ws004-hardwareの測定fixture。

現状: 現行IMOD=4000。0/160/4000の実機比較が採用条件で、QEMUの同値結果は代用不可。現在テスト機SSHがtimeout。

設計手順: (1) 各測定artifactにIMOD設定とreadback、driver/source hashを記録。(2) 同一USB2/USB3 storage、WLAN/HID並行条件で0/160/4000を比較。(3) IRQ/s、CPU、p50/p95/p99、timeout/reset、shutdownを採取。(4) correctnessで劣る値を除外し、有意な改善がなければ4000を維持する。

今回の未クリア理由: 実機比較の実行環境と複合topologyを取得できず、既定値選択に必要なデータがない。

再開条件: 比較可能な実機topologyと遠隔または直接起動経路。

旧停止節のplanned維持はq122時点の判断。今回の実行結果はunclearedとして扱う。

## 現在の実行条件（q140中の読み取り確認、2026-09-09）

上記q122/q125の実機不在による停止は履歴。ユーザー指示によりQEMUで実装・
機能比較できる部分を実行し、物理機器の性能評価と区別する。
現ソース `src/drivers/pci/pci-xhci.c` には `ZEDBSD_XHCI_IMOD` の比較用override、
16bit上限assert、既定4000と起動時のIMODレジスタ書込みがすでにある。
新たな設定所有者は作らない。次のQueueでは専用artifactの0/160/4000を既存の
USB storage/HID受け入れへ接続し、設定値と実レジスタ読戻し、timeout/reset、
整合性と終了を記録する。QEMUの処理時間だけで物理性能の最適値を決めず、
実機比較のない既定値は4000を維持する。実施済みとの主張ではない。
