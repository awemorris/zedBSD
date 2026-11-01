<!-- awesome-plan project=zedbsd record=ws321 -->

# WS321 Pi 5（BCM2712） の Vulkan Video: H.265 decode（HEVC の block を Vulkan Video として）。H.264 は hardware に無く libavcodec

- Master: [plan/master.md](../master.md)（2026-10-30 作成）。Primary Milestone: MG006。
- Status: incomplete（2026-11-02 08:32 ユーザーの指示で P3 が着手。WS318 の completed を待たない）
- 目標: Pi 5（BCM2712） の video の hardware のうち zedBSD で未実装の物を、kernel の Vulkan の executor（`src/drivers/gpu/bcm2712/`）の Vulkan Video（`VK_KHR_video_decode_*`・`VK_KHR_video_encode_*`）として実装し、libmedia から使えるようにする。

## ユーザーの決定（2026-10-30、原文）

- 「…H.264とH.265のデコーダとエンコーダの表の中で未の部分を埋めるようにVulkan Video driverを実装して…ドライバは個別のWSに分けて依存関係にしてもかまいません。先にlibmediaの方を作りきってcompleteにしてから、個別のドライバを実装する流れにします。…」（全文は [WS318](../ws318/ws.md)）
- 「VPUが独立だとしても、我々はVulkan Video driverとして見せる必要があります。それがOSのAPIなので。」
- H.265（2026-10-30 ユーザー「H.265のパテントプールへの支払いは行う方針です。H.265の実装を行っていいです。」）: WS236（2026-10-14）の「HEVCはライセンスがないのでやらないです」を置き換え、H.265 の decode・encode を実装する。

- （2026-11-02 08:32）ユーザーから、Pi 5 の H.265 decode を P3 で実装し、Video Player の GPU の道（VP_GPU_PRESENT）で再生できるところまで完了させる、という趣旨の指示を受けた（WS318 の completed を待たずに始める）。

## Phase

| Phase | 目的 | 状態 | 依存 |
| --- | --- | --- | --- |
| [ws321-p001](phase001/phase.md) | 調べと設計: HEVC の block（register・clock・割り込み・2 段の pipeline・SAND の出力・制限）、出典と license、Vulkan Video への写し、試験の道 | in-progress（P3 q1923、設計を書いた） | — |
| ws321-p002 | 実装: slice header と命令の FIFO（host 試験）、HEVC の block の driver、bcm2711 の executor の H.265 の reader と run（SAND の pool、TFU で NV12 へ）、`CONFIG_DRIVER_BCM2712_VIDEO` | planned | p001 |
| ws321-p003 | 実機の受け入れ: vkvideo-probe の H.265 の適合の stream を ffmpeg と照合、Video Player の GPU の道で H.265 を再生 | planned | p002 |

## Resume point

- 2026-11-02 08:45 P3（q1923）: p001 の設計を [phase001](phase001/phase.md) に書いた（出典は Raspberry Pi の Linux rpi-6.12.y の hevc_dec、GPL-2.0、値と手順だけ。firmware は無い）。次: p002。

- 2026-10-30 Q1: 作成。
- 2026-11-02 08:32 Q1: ユーザーの指示で P3（q1923）が p001〜p003 を続けて行う。
