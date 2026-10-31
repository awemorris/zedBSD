<!-- awesome-plan project=zedbsd record=ws291-p023 -->

# ws291-p023: 共有の image の画素の並びを client と compositor で決める（WSI の block-linear、まず設計）

Status: planning（2026-10-31 新設、設計から）
Parent: [WS291](../ws.md)
Queue: q1781（P3）

## 目的

2026-10-31 ユーザーから、RTX の subs-1000 は 0j の (b)（画面に出す image を GPU の得意な並びにする）で進める、という趣旨の指示を受けた。
あわせて、image の並びは普通は client が求める物なので、そこをどう扱うかを示す事を求められた。

p014 の結果: RTX は pitch-linear の色の target と block-linear の深度を組めないので、nvrtx は block-linear の影に描き、pass の終わりごとに
共有の image（WSI の image、pitch-linear）へ全画面を copy する。subs-1000 は 1 frame に submit が 1000 回なので copy も 1000 回（1 回約 29 µs）。

## Q1 の見立て（設計の出発点、設計で確かめる）

- `VkSwapchainCreateInfoKHR` に tiling は無く、swapchain の image の memory の並びは WSI と driver が決める。決める制約は読む相手
  （compositor の合成・display の scanout・他の GPU・CPU の読み）。Linux は compositor が surface ごとに読める並び（DRM format modifier）を
  知らせ（linux-dmabuf の feedback）、WSI が選ぶ。
- zedBSD の `kl_gpu_buffer_v1`（今は版 3）には並びの相談が無く、共有の image はいつも pitch-linear。
- 案: (1) 共有の resource が並びを持つ（読む側が正しく読む）、(2) compositor が surface ごとに読める並びを知らせる（`kl_gpu_buffer_v1` の版を上げる）、
  (3) WSI は読む相手が同じ device の時だけ block-linear を選び、他は pitch-linear、(4) 相手が変わったら知らせて swapchain の作り直し
  （`VK_SUBOPTIMAL_KHR`）。copy は submit ごとから、多くても present ごとに 1 回（合成なら 0）へ。
- HAL は変えない。他の GPU の driver（amdrdna・i915・powervr・venus など）が後で自分の並びを使える形にする。

## 範囲（この Queue）

1. 設計の文書 `plan/ws291/phase023/design.md`: 今の WSI・`kl_gpu_buffer_v1`・compositor・libkeiland-backend・kernel の GPU の resource の
   metadata と import・nvrtx の影の copy・scanout（display が block-linear を読めるか）・画面の撮影（keiland-shot）を読んだ事実、
   並びの表し方、protocol の版の変更、WSI の選び方と作り直し、fallback、他の GPU への広げ方、WS309 p015（game mode の V2）との関係、
   試験（subs-1000・big-1000・desktop・画素の比べ）、実装の Phase の分け方と見積もり。
2. design-reviewer の敵対的レビューと反映。
3. ユーザーへの問い（新しい protocol の版など製品の判断）を `plan/ws291/phase023/user-questions.md` に、AGENTS.md §3 の前置きの形で。
4. code は書かない。実機は使わない。
