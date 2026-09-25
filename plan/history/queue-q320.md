<!-- awesome-plan project=zedbsd record=queue-q320 -->

# Queue q320: libcのヘッダを `include/libc/` へ

<!-- awesome-plan-current:start -->
Status: finished
Active Queue: none（q320 finished）
Executor: メインセッション
Last Queue: q319 finished（履歴 `plan/history/queue-q319.md`）
<!-- awesome-plan-current:end -->

Approval: current user「続けてください。」（2026-09-23、q320案の提示後）
Finish UTC: 2026-09-23T01:10:54+00:00

| Order | Attempt | Phase | Status | Scope |
| --- | --- | --- | --- | --- |
| 1 | q320-i01 | [ws035-p023](../ws035/phase023/phase.md) | cleared | `libc/include/` の139ファイルを `include/libc/` へ。参照206ファイルの置換 |

## 結果

buildの正常性（disk-image EXIT 0、kernel warning 0）、include監査 `--require-none` PASS、boot-test PASS。
途中で5件の取りこぼしを直した（詳細はPhaseの記録）。host fixtureの46件は流していない。
