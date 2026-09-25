<!-- awesome-plan project=zedbsd record=ws064 -->

# WS064: base の make の並列（`-j`）

<!-- awesome-plan-current:start -->
Status: incomplete
Primary Milestone: MG002
Related Milestones: MG001
Objectives: O1
Parent: [Master](../master.md)
Queue: なし
Resume point: p001・p002・p004 cleared（`make -j4` 4.18〜4.38 秒、host 5.14〜5.18 秒）。残りは規約の p003（ユーザーの指示で最後）
<!-- awesome-plan-current:end -->

## 目標

2026-09-26 ユーザー指示: 「makeは-jに対応させて、並列makeの実行時間もホストと同等以上にしてください。」（[F-016](../future-work.md) の昇格）

base の make（`userland/base/make/`）は `-j` を受け付けて無視し、command を 1 つずつ走らせる。POSIX と GNU make の慣習に沿って `-j N` で job を並列に走らせる。子の make（再帰の `$(MAKE)`）との job の数の分け合い（jobserver）も要る（automake の Makefile は再帰する）。

受け入れ: expat の `make -j4` が guest で host の `make -j4`（5.2 秒）と同等以上。make の差分試験（`plan/ws046/tests/make-diff.py`、91 件）が通り、`-j` の case を足す。並列の失敗（1 つの job の失敗で止まる、`-k`）と出力の扱い。規約。

## Phase 一覧

| Phase | 内容 | Status | 依存 |
| --- | --- | --- | --- |
| [ws064-p001](phase001/phase.md) | 設計と実装: job の並列、jobserver（pipe の token、`MAKEFLAGS`）、失敗と `-k`、差分試験の `-j` の case | cleared（q448-i01。差分試験 100/100（guest）、host の expat `-j4` 5.08 秒（GNU 5.06）、guest 7.3〜7.8 秒） | — |
| [ws064-p002](phase002/phase.md) | 並列の make の性能（expat の `make -j4` を host と比べ、kernel・libc の並列の費用を詰める） | cleared（q449-i01。4.94〜5.00 秒、host 5.14〜5.18 秒。mutex の速い道、vfork と posix_spawn、fork・destroy の lock の保持） | p001 |
| [ws064-p003](phase003/phase.md) | WS064 の変更の規約の適合（ユーザーの指示で最後） | planned | p002 |
| [ws064-p004](phase004/phase.md) | `/bin/sh` の process の生成を posix_spawn（vfork）に広げる（2026-09-26 ユーザー指示） | cleared（q450-i01。fork: make 1417 → 214、configure 966 → 423。configure 7.1 秒、`make -j4` 4.2〜4.4 秒） | p002 |
