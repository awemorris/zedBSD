<!-- awesome-plan project=zedbsd record=ws324 -->

# WS324 RTX 2070（nvrtx） の Vulkan Video: H.264 encode（NVENC）、H.265 decode（NVDEC）・encode（NVENC）

- Master: [plan/master.md](../master.md)（2026-10-30 作成）。Primary Milestone: MG006。
- Status: planning（2026-10-30 Q1 が作成。[WS318](../ws318/ws.md)（libmedia）の completed の後に始める）
- 目標: RTX 2070（nvrtx） の video の hardware のうち zedBSD で未実装の物を、kernel の Vulkan の executor（`src/drivers/gpu/nvrtx/`）の Vulkan Video（`VK_KHR_video_decode_*`・`VK_KHR_video_encode_*`）として実装し、libmedia から使えるようにする。

## ユーザーの決定（2026-10-30、原文）

- 「…H.264とH.265のデコーダとエンコーダの表の中で未の部分を埋めるようにVulkan Video driverを実装して…ドライバは個別のWSに分けて依存関係にしてもかまいません。先にlibmediaの方を作りきってcompleteにしてから、個別のドライバを実装する流れにします。…」（全文は [WS318](../ws318/ws.md)）
- 「VPUが独立だとしても、我々はVulkan Video driverとして見せる必要があります。それがOSのAPIなので。」
- H.265 について: WS236（2026-10-14）のユーザーの決定「HEVCはライセンスがないのでやらないです」と、今回の「H.264とH.265のデコーダとエンコーダ」の関係はユーザーに確認中（Q1、2026-10-30）。確認まで H.265 の Phase は planning のまま始めない。

## Phase

| Phase | 目的 | 状態 | 依存 |
| --- | --- | --- | --- |
| ws324-p001 | 調べと設計: hardware の能力の確認、firmware、出典と license、Vulkan Video の対応の範囲、試験の道 | planning | WS318 completed |

## Resume point

- 2026-10-30 Q1: 作成。WS318 の completed の後。
