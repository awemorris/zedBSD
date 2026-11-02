<!-- awesome-plan project=zedbsd record=ws321 -->

# WS321 Pi 5（BCM2712） の Vulkan Video: H.265 decode（HEVC の block を Vulkan Video として）。H.264 は hardware に無く libavcodec

- Master: [plan/master.md](../master.md)（2026-10-30 作成）。Primary Milestone: MG006。
- Status: incomplete（2026-11-02 08:32 ユーザーの指示で P3 が着手。WS318 の completed を待たない）
- 目標: Pi 5（BCM2712） の video の hardware のうち zedBSD で未実装の物を、kernel の Vulkan の executor（`src/drivers/gpu/bcm2712/`）の Vulkan Video（`VK_KHR_video_decode_*`・`VK_KHR_video_encode_*`）として実装し、libmedia から使えるようにする。

## ユーザーの決定

- （2026-10-30）ユーザーから、H.264・H.265 の decoder・encoder の表の未の部分を埋めるように Vulkan Video の driver を実装する、driver は個別の WS に分けて依存にしてよい、先に libmedia を作り切ってから個別の driver を実装する流れにする、という趣旨の指示を受けた（詳しくは [WS318](../ws318/ws.md)）。
- （2026-10-30）ユーザーから、VPU が独立した hardware でも、OS の API として Vulkan Video の driver として見せる、という趣旨の指示を受けた。
- （2026-10-30）ユーザーから、H.265 の patent pool への支払いは行う方針なので H.265 を実装してよい、という趣旨の決定を受けた。WS236（2026-10-14）の「HEVC は license が無いので行わない」という決定を置き換え、H.265 の decode・encode を実装する。

- （2026-11-02 08:32）ユーザーから、Pi 5 の H.265 decode を P3 で実装し、Video Player の GPU の道（VP_GPU_PRESENT）で再生できるところまで完了させる、という趣旨の指示を受けた（WS318 の completed を待たずに始める）。

## Phase

| Phase | 目的 | 状態 | 依存 |
| --- | --- | --- | --- |
| ws321-p001 | 調べと設計: hardware の能力の確認、firmware、出典と license、Vulkan Video の対応の範囲、試験の道 | in-progress（P3 q1923） | — |
| ws321-p002 | 実装: bcm2712 の HEVC の block の driver と Vulkan Video の executor（libvulkan の H.265 の転送・capset の bit 2 は main に有る） | planned | p001 |
| ws321-p003 | 実機の受け入れ: vkvideo-probe の H.265 の適合の stream を ffmpeg と照合、Video Player の GPU の道で H.265 を再生 | planned | p002 |

## Resume point

- 2026-10-30 Q1: 作成。
- 2026-11-02 08:32 Q1: ユーザーの指示で P3（q1923）が p001〜p003 を続けて行う。
