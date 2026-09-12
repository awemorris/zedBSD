<!-- awesome-plan project=zedbsd record=queue -->

# Queue q307: 回転するテクスチャ付き直方体で3D実装を検証

<!-- awesome-plan-current:start -->
Status: finished
Active Queue: none
Last Queue: q307
Result: q307-i01 / ws014-p005 cleared
Executor: none
<!-- awesome-plan-current:end -->

Authorization: current user「直方体をテクスチャ付きで描画し、時間とともに回転させるだけのサンプルを、userland/base/vkdemoとして作れませんか？」「p005にしましょうか。」「GitHubは承認します。」
Started UTC: 2026-09-12T15:24:35.675241+00:00
Timebox: 240 active minutes estimate; progress/boundary review each120 minutes; bounded individual attempts.

| Order | Attempt | Phase | Status | Scope |
| --- | --- | --- | --- | --- |
| 1 | q307-i01 | [ws014-p005](https://github.com/awemorris/zedBSD/issues/387) | cleared | textured rotating cuboid, VS/FS, API coverage and real GPU/frame verification |

Dependency: p003 cleared / q306 finished → p005 → p004。
Upcoming Work Outlook: p004最終API整理・規約確認、その後別WS029 native i915。今回未実行。
History: q306はplan/history/queue-q306.mdとp003/Queueの確認済み公開本文に保存済み。

## q307開始: p005をp004の前へ追加（2026-09-13）

ユーザーがテクスチャ付きの回転直方体デモをuserland/base/vkdemoとして作り、vertex/fragment shaderとAPI不足を確認するよう依頼。[p005](https://github.com/awemorris/zedBSD/issues/387)を追加し、p003 cleared → p005 → p004の順とする。q307/q307-i01はp005だけを実行。p003/q306のclear/終了は維持し、p004とnative i915は未実行。

独自GLSL→SPIR-V、実texture/depth/graphics pipeline、時間の進む同一process、GPU readbackとVNC実画面、独立した幾何/texture照合で確認する。既存GPU APIを再利用し、必要なU共通化と実測された不足だけを補う。HALの追加変更は未許可。見積240 active minutes、120分ごとの点検、有限build/VM/pollを適用する。GitHub同期はユーザー明示承認済み、git add/commit/pushはユーザーが行う。

## q307完了: p005 cleared（2026-09-13 JST）

userland/base/vkdemoにテクスチャ付き回転直方体を実装。独自vertex/fragment shader、実texture、depth、Vulkan pipelineを使用する。q307-vkdemo-002で固定3時刻と実時間3枚のGPU readback/VNC hashが一致し、独立したray/texture期待値との照合も不一致0。正常終了後、同じVMで通常2秒・12frameの回転を再openしてDONE/shell復帰、QEMU exit0まで確認した。

新規ioctlは不要。Uの共通Venus clientとgraphics操作を追加し、実測で発見したKのblob unmap待機の早期timeoutを修正した。clock進行中は10秒deadlineを維持し、clock停止中だけ連続poll上限を使う。HAL追加変更なし。有限host tests、shader/CLI/画像検証、専用amd64 build、p003回帰がPASS。

q307 finished、q307-i01/p005 cleared、active Queueなし。p003/q306の完了を維持し、次はp004（planning、未実行）。p001 planning、WS014 incomplete。native i915は別WS029。汎用libvulkan/ICD・全Vulkan適合・汎用WSI/zero-copyは未実装。

実測結果とAPI表は[p005](https://github.com/awemorris/zedBSD/issues/387)。ローカルのplan/ws014/phase005/results.md、api-coverage.md、evidence/とplan/history/queue-q307.mdへ保存。GitHubは計画/結果本文を同期し、source・資料・画像のgit add/commit/pushはユーザーが行う。
