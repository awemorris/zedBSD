<!-- awesome-plan project=zedbsd record=queue-q317 -->

# Queue q317: `include/drivers` の階層化と、kernelからlibcを切り離す設計

<!-- awesome-plan-current:start -->
Status: finished
Active Queue: none（q317 finished）
Executor: root（このエージェント）。Phaseはサブエージェントで並行実行
Last Queue: q316 finished（履歴 `plan/history/queue-q316.md`）
<!-- awesome-plan-current:end -->

Approval: current user「q317 queueを承認します。実行してください。」（2026-09-23）
Start UTC: 2026-09-22T19:56:09+00:00
運用方針: `plan/master.md`「実行体制とQueue運用方針」

| Order | Attempt | Phase | Status | Scope | 実行 |
| --- | --- | --- | --- | --- | --- |
| 1 | q317-i01 | [ws035-p002](../ws035/phase002/phase.md) | cleared | `include/drivers/` を `src/drivers/` の階層へ。i915・Venusの公開ヘッダを `include/drivers/pci/pci-i915.h`・`pci-venus.h` にし、登録関数を改名 | phase-runner（Opus 5.5 High） |
| 2 | q317-i02 | [ws035-p033](../ws035/phase033/phase.md) | cleared | kernelからlibcを切り離す設計（kcrt、`src/kern/heap.c`、include整理、検査）。敵対的レビュー付き | driver-designer＋design-reviewer（Fable 5.1 High） |

## 選んだ理由と並行条件

優先順位の先頭のrefactorの最初の移動（p002）と、次の移動（p034・p035）の設計（p033）。p033は文書だけで、p002の
ソース変更と重ならない。p033の設計はp002後のパスを前提に書く。

## 時間と上限

p002は見積240分、p033は180分（レビュー込み）。build 1回1800秒。同条件の変更なしretryは3回まで。
aggregate `make check` は使わない。amd64以外のbuildは壊れてもよい（ユーザー決定）。

## 権限

HALは `#include` のパス変更だけ承認済み（p002ではHALの変更は0行の見込み）。UAPIは変えない。
commitはrootが `git commit -m WIP` で行い、pushしない。

## 結果（Finish UTC: 2026-09-22T20:55:40+00:00）

| Attempt | Phase | 結果 | 主な成果 |
| --- | --- | --- | --- |
| q317-i01 | ws035-p002 | cleared | 38ヘッダを `src/drivers/` の階層へ移動、`pci-i915.h`・`pci-venus.h`、登録関数の改名。amd64・i915・Venusのbuild warning 0、libcヘッダの読込み増加なし。`refactor-refs.py` の膨張不具合を修正 |
| q317-i02 | ws035-p033 | cleared | `kcrt-design.md`（kcrt、`src/kern/heap.c`、uapiへのABI移動、Vulkanヘッダ、検査、p034・p035の受け入れ）。design-reviewerの17件の指摘をすべて反映 |

p033で見つかった判断事項と、p023への新しい依存はWS035の `ws.md` に記録した。
