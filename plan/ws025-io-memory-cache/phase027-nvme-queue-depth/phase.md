# ws025-p027: 条件付き NVMe 多重発行

日付: 2026-09-07

Phase ID: `ws025-p027`

Status: completed (q139); bounded pipeline and QEMU functional acceptance passed.

Parent: [WS025](../ws.md)

依存: ws025-p019。実機性能測定は採用値の根拠として別に記録するが、
QEMUで実装・機能検証できる範囲の停止条件にはしない。

追加の先行条件: [ws025-p031](../phase031-driver-layout-style/phase.md) のドライバ整理を完了してから実装する。下記の旧ソースパスは p031 の移行表で解決する。既存の採用条件は維持する。

追加の回帰 gate: [ws025-p032](../phase032-pc98-boot-regression/phase.md) の PC-98 QEMU 起動回復を完了してから実装へ進む。

## 現在の選択

ユーザーがp027〜p030を次に実施する最優先項目として選択した。
旧見送りを継続する扱いではなく、p031残件・現ソース・下記の測定/実機条件を確認してQueue化する。
今回の計画整理で実装や測定を実施済みとは扱わない。

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
4. fsync の frontier と read/write 公平性を検証する。QEMUでは depth 1/2/4/8
   の機能・発行数・順不同完了・失敗回収を比較する。throughput/p95/CPUの
   実機値は性能主張にのみ必要で、機能実装の合否とは区別する。

[共通 I/O 契約](../io-design.md) と [memory 設計](../memory-design.md) を維持する。

## 受け入れ

- ASYNC01–ASYNC08、FLUSH02–FLUSH06 と NVMe fixture/実機セル。depth 1 の回帰と selected depth の改善を記録する。
- 該当 ID の詳細は [受け入れ行列](../acceptance.md)。変更範囲の focused fixture と supported build を実行し、source/config・counter・結果を記録する。

## 停止・再開と成果物

QEMUで完了できる実装・機能検証は実行する。物理NVMeの性能主張だけを
未検証として残し、実機不在を理由にPhase全体を停止しない。

## q139 execution contract

Use the existing 63-slot/CID/epoch owner. Add a bounded single-BIO pipeline
without creating another command or DMA lifetime. Post up to the selected
window, accept out-of-order completions, copy successful reads only after their
own completion, and retain every caller buffer until all posted slots retire.
On the first error, stop new posts, drain or recover every posted slot, report
the deterministic first logical-chunk error, and never reuse a stale CID/epoch.
FLUSH enters only after prior writes leave the existing BIO admission frontier;
subsequent reads/writes remain excluded until it retires. Depth 1 must exercise
the same implementation as larger windows.

Acceptance requires focused normal and sanitizer model tests for ordered and
out-of-order completion, partial post failure, timeout/reset and flush
serialization; explicit amd64/pcat/pc98 builds; and a disposable QEMU NVMe cell
comparing functional counters at depths 1/2/4/8. Do not claim physical
throughput improvement from QEMU.

成果物は production patch、必要な fixture、実行後の `results.md`。途中状態は同ファイルに事実・失敗地点・残条件を記し、未実施を PASS にしない。Queue 選択前には実装せず、build/runtime は共有環境で直列に行う。

## q122 adoption decision

Not adopted in the mandatory WS025 implementation. Measure a depth-1 bottleneck on target NVMe hardware before selecting bounded queue depth.
See [effective policy and evidence basis](../phase026-integration-defaults/effective-policy.md).
This records the conditional decision, not completion of this optional phase.

## q125 現ソースに合わせた詳細化

ユーザーのPriority全件自走指示により着手条件を確認。完了できないPhaseはunclearedとし他WSへ進む。

対象: src/drivers/pci/pci-nvme.c、src/kern/disk.c、src/drivers/generic/dma.c。

現状: NVME_IO_QUEUE_REQUESTED_DEPTH=64、最大63 slotとslot別lifecycle、io_epoch、completion照合、flush admissionが既存。nvme_disk_submitは各chunkのnvme_io_execute完了を待つ。並行BIOと単一BIO内の発行を区別する。

設計手順: (1) 既存slot/CID/epochとflush境界を維持し、新しいqueue ownerを作らない。(2) depth=1/2/4/8の同一対象・同一workload測定を先に行う。(3) 単一BIOのpipelineを採用する場合、各chunkにoffset・完了bytes・最初のerrorを持たせ、全slot retirementまでcaller backingを保持する。(4) out-of-order、CID再利用、flush待機、reset時の部分完了を既存fixtureへ追加する。

今回の未クリア理由: 対象NVMeでdepth律速を示す測定が未成立。テスト機SSHは接続timeout。既存multi-slotを新実装と数えて完了にはできない。

再開条件: 対象NVMeを確認できる実行環境とdepth比較。p031現ソースのNVMe/共通disk回帰を確認したうえでbounded pipelineを選択する。

旧停止節のplanned維持はq122時点の判断。今回の実行結果はunclearedとして扱う。

## q139 completion

Completed with default per-BIO window 4, BIO admission up to 64 KiB, existing
4 KiB/MDTS-limited commands, and unchanged CID/DMA recovery ownership. Host
fault tests, all three final platform builds, depth 1/2/4/8 native QEMU
write/FLUSH/read/restart and NVMe UFS writeback regressions passed. See
[results](results.md). Historical q122/q125 hardware gates above are superseded
by the user's hardware-independent implementation instruction and q139 contract.
The bounded default is a functional policy, not a measured physical optimum.
