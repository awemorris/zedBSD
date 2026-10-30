<!-- awesome-plan project=zedbsd record=ws306-p001 -->

# ws306-p001 設計: ゲーム最適化の UAPI と driver の識別

- Parent: [WS306](../ws.md)
- Status: in-progress（2026-10-31 P5 q1746-i01: 設計の第 3 版と design-reviewer の第 2 回の反映を commit。cleared か uncleared かは Q1 の判定）
- 担当: P5（kernel-and-driver-designer の役、worktree `/home/awe/zedBSD-worktrees/p5-ws306`、branch `agent/p5-ws306`、基点 main 9946e22c0）
- 範囲: `plan/ws306/` だけ。source は変えない。

## 目的

今の main の code（GPU の枠組み・8 つの driver・libvulkan）に照らして、ゲーム最適化の UAPI・latch・driver の口・識別・libvulkan の環境変数と関数・
vulkaninfo の移植と license の監査の手順・driver ごとの option の初期の一覧・Phase の分け方を設計し、design-reviewer の敵対的レビューを受けて反映する。

## 受け入れ

- [design.md](../design.md) が main の code と照合されている（行番号つき）。HAL を変えない理由がある。
- design-reviewer のレビューが [design-review.md](../design-review.md) にあり、全指摘の反映か見送りの理由が design.md にある。
- ユーザーの判断の要る点が [user-questions.md](../user-questions.md) に背景・用語・推しつきである。
- p002 以降の Phase の表が ws.md と design.md §13 にある。

## 結果（2026-10-31 P5）

- design.md 第 3 版（照合 main 9946e22c0）。design-reviewer の第 2 回: Blocker 0・Major 7・Minor 13、全部を反映（design §18）。
- 問い: Q306-7（boot の行 `amdrdna.vk.ngg` を診断用に残すか）、Q306-9（WS291 を待たずに枠組みの Phase を進めるか）。Q306-8 は前提の誤りで取り下げ。
- 標準廉価実装手順は条件 1 を満たす部分が無いので使わない（design §13）。
- 未実施: 実装・試験は p002 以降。Q1 に頼む事: WS226 の ws.md への引き継ぎ（`VkPhysicalDeviceDriverProperties`、`vulkan_zedbsd.h` と `API-PROVENANCE.md` の行を残す事）。
