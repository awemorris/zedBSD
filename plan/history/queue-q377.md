<!-- awesome-plan project=zedbsd record=queue-q377 -->

# Queue q377: rpi4 の console の font を PC/AT の 8x16 へ（ws044-p001）

<!-- awesome-plan-current:start -->
Status: finished（2026-09-24）
Active Queue: q378
Executor: メインセッション（サブエージェントは使わない）
<!-- awesome-plan-current:end -->

Approval: 2026-09-24 ユーザー指示「rpi4のコンソールのフォントはPC/ATのものを複製して使ってください。パブリックドメインなので大丈夫です。」と、HAL の差分としての承認（同日「承認、すぐ進める」）。
q376（ws043-p002 sed）はこの依頼を先にするため中断した（q376-i01 uncleared）。q377 の後に再開する。

| Order | Attempt | Phase | Status |
| --- | --- | --- | --- |
| 1 | q377-i01 | [ws044-p001](../ws044/phase001/phase.md) | cleared（QEMU raspi4b の画面で 8x16 の login prompt、build warning 0） |
