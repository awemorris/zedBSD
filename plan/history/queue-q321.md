<!-- awesome-plan project=zedbsd record=queue-q321 -->

# Queue q321: softfloatを `src/libc/` へ

<!-- awesome-plan-current:start -->
Status: finished
Active Queue: none（q321 finished）
Executor: メインセッション
Last Queue: q320 finished（履歴 `plan/history/queue-q320.md`）
<!-- awesome-plan-current:end -->

Approval: current user「softfloatは、src/libc/…へ移します。…zed-のプレフィクスのファイル名はやめよう。」（2026-09-23）
Finish UTC: 2026-09-23T01:19:19+00:00

| Order | Attempt | Phase | Status | Scope |
| --- | --- | --- | --- | --- |
| 1 | q321-i01 | [ws035-p036](../ws035/phase036/phase.md) | cleared | softfloat 8ファイルを `src/libc/` 直下へ。`zed-` 接頭辞の除去、sparcv9のディレクトリ廃止、include guardの改名、参照16ファイルの置換 |

## 結果

buildエラー0（`build/amd64` を消した状態から）、boot-test PASS。詳細はPhaseの記録。
