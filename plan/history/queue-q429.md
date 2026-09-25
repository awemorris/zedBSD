<!-- awesome-plan project=zedbsd record=queue-history q429 -->

# Queue q429: disk の無い mount の `st_dev`（ws059-p001、BUG-047）

<!-- awesome-plan-current:start -->
Status: finished（2026-09-25）
Active Queue: q429
Executor: メインセッション（サブエージェントは使わない）
<!-- awesome-plan-current:end -->

Approval: 2026-09-25 ユーザー「…引き続きパフォーマンス問題の修正と、バグ修正と、上記観点での修正をお願いします。」（バグ修正）。範囲は [ws059-p001](ws059/phase001/phase.md)。HAL は変えない。
Timebox: このセッション。

| Order | Attempt | Phase | Status |
| --- | --- | --- | --- |
| 1 | q429-i01 | ws059-p001 | cleared（`mount_device_number()`: disk の無い mount に `0x80000000 | (1 + slot)` の `st_dev`。全 mount で `st_dev` が異なり、coreutils の `df` が全 mount を出す。回帰 boot PASS・sh 1388/1425（同じ集合）・make 91/91・SMP 0。WS059 完了） |

依存: なし。

Upcoming Work Outlook: ws057-p003（判断待ち）、ws056-p001 の判断（BUG-046）、ws046-p012、BUG-036・039・040・041、WS055。
