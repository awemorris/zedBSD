<!-- awesome-plan project=zedbsd record=queue-q353 -->

# Queue q353: 動かない host の試験の削除（ws034-p049）

<!-- awesome-plan-current:start -->
Status: finished（2026-09-24）
Active Queue: なし
Executor: メインセッション（サブエージェントは使わない）
<!-- awesome-plan-current:end -->

Approval: 2026-09-24 のユーザーの判断（「動かないテストは rewrite せず、削除しましょう」「作業を継続してください」）。

| Order | Attempt | Phase | Status |
| --- | --- | --- | --- |
| 1 | q353-i01 | [ws034-p049](../ws034/phase049/phase.md) | cleared |

## 進捗

| Attempt | Phase | 結果 |
| --- | --- | --- |
| q353-i01 | ws034-p049 | **cleared**。host の runner 307 本を走らせ、build できない・今の source と合わない 185 本と、それだけが使う fixture（計 430 file）を削除。残る 112 本は削除後も通る。製品の不具合の疑いがある 3 本は ws031-p049 へ。QEMU 等の 82 本は走らせていない |
