<!-- awesome-plan project=zedbsd record=queue-q319 -->

# Queue q319: libcのsourceとcrtを `src/libc/` へ移す

<!-- awesome-plan-current:start -->
Status: finished
Active Queue: none（q319 finished）
Executor: メインセッション（サブエージェントは使わない）
Last Queue: q318 finished（履歴 `plan/history/queue-q318.md`）
<!-- awesome-plan-current:end -->

Approval: current user「では、実行してください。」（2026-09-23、p003の範囲にcrtの移動とcrt0.Sの改名を含める確認への回答）
Start UTC: 2026-09-23T00:45:37+00:00
運用方針: `plan/master.md`「実行体制とQueue運用方針」（Phaseは1つ、メインセッションが実行）

| Order | Attempt | Phase | Status | Scope |
| --- | --- | --- | --- | --- |
| 1 | q319-i01 | [ws035-p003](../ws035/phase003/phase.md) | cleared | `libc/` のsourceを `src/libc/` へ、`src/crt/` を `src/libc/crt/` へ、`crt0.S` を `crt0-i386.S` へ。参照216行の置換 |

## 確認

buildの正常性（kernel・userland・disk imageがwarning 0）と、Phaseの最後に `plan/tools/boot-test.sh` を1回だけ。
host fixtureの46件は流さない（2026-09-23ユーザー指示）。VMのlogは使わない。

## 境界

`libc/include/` は動かさない（p023）。bootヘッダはp004。amd64以外のbuildは壊れてよい。
HAL・UAPIの意味は変えない（includeのパス変更のみ、承認済み）。

## 結果（Finish UTC: 2026-09-23T00:52:58+00:00）

ws035-p003 cleared。65ファイルを `src/libc/`（crtは `src/libc/crt/`）へ移し、`crt0.S` を `crt0-i386.S` へ改名。
参照41ファイルを置換。amd64・i915のbuild warning 0、disk-image エラー0、include監査PASS、boot-test PASS。
このQueueから、メインセッションがPhaseを1つずつ実行する体制になった。回帰試験の範囲も、Phaseの性質で決める方針に変えた
（移動だけのPhaseはbuildの正常性とboot-testだけ）。
