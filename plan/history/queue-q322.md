<!-- awesome-plan project=zedbsd record=queue -->

# Queue q322: bootヘッダを `include/kern/` へ

<!-- awesome-plan-current:start -->
Status: finished
Active Queue: none（q322 finished）
Executor: メインセッション
Last Queue: q321 finished（履歴 `plan/history/queue-q321.md`）
<!-- awesome-plan-current:end -->

Approval: current user「続けてください。」（2026-09-23、p004案の提示後）
Finish UTC: 2026-09-23T01:37:27+00:00

| Order | Attempt | Phase | Status | Scope |
| --- | --- | --- | --- | --- |
| 1 | q322-i01 | [ws035-p004](../ws035/phase004/phase.md) | cleared | bootヘッダ7ファイルを `include/kern/` へ。include guardの整理、参照50ファイルの置換 |

## 結果

kernel warning 0、disk-image エラー0、boot-test PASS。HALの変更は `#include` 10行のみ（承認範囲）。
**これでWS035のrefactor（p002・p003・p023・p036・p004）がすべてcleared**。

## q322後のQueue外作業（2026-09-23）

ユーザーの指示「pc98版でログインプロンプトまで進めるかチェックしてください」「pc/atも起動できるようにしてください」
により、Queueを組まずにpcat・pc98のbuildと起動を通した。結果は [WS036](ws036/ws.md) の
「Queue外で達成した分」に記録し、ws036-p002・p005〜p009をclearedにした。
i386 HALへの `hal_mmio_read8`・`hal_mmio_write8` の追加は、差分を提示してユーザーの事前承認を得てから適用した。
