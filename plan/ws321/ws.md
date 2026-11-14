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
| [ws321-p001](phase001/phase.md) | 調べと設計: HEVC の block（register・clock・割り込み・2 段の pipeline・SAND の出力・制限）、出典と license、Vulkan Video への写し、試験の道 | cleared（2026-11-02 P3 q1923-i01、Q1 が 2026-11-02 10:17 に受け入れ。設計と review 37 件の反映） | — |
| [ws321-p002](phase002/phase.md) | 実装: slice header と命令の FIFO（host 試験）、HEVC の block の driver、bcm2711 の executor の H.265 の reader と run（session の memory の SAND、TFU で NV12 へ）、`CONFIG_DRIVER_BCM2712_VIDEO`（既定 n） | cleared（2026-11-02 P3 q1923-i01、Q1 が 2026-11-02 10:17 に受け入れ。warning 0、host 試験 147 本 不一致 0・fuzz 100000 件） | p001 |
| [ws321-p003](phase003/phase.md) | 実機の受け入れ: vkvideo-probe の H.265 の適合の stream を ffmpeg と照合、Video Player の GPU の道で H.265 を再生 | cleared（2026-11-02 P3 q1923-i01、Q1 が 2026-11-02 10:17 に受け入れ。Pi 5 で HEVC_v1 146 本のうち 128 本が全 frame 一致・15 本は理由付きで断る・3 本未決（VF2 と同じ）、Video Player GPU の道 H.265 1080p30 600/600 late 0・1080p60 1197 late 3、画面の PNG） | p002、ws322-p006（libmedia の H.265 の backend） |
| ws321-p004 | 規約の全文の見直し（AGENTS §2） | in-progress（2026-11-15 P2 q2111） | p002 |

## Resume point

- 2026-11-02 10:20 P3（q1923-i01）: p001〜p003 を終えた（[phase002](phase002/phase.md)・[phase003](phase003/phase.md)）。Pi 5 は元の kernel・cmdline に戻した（cksum 一致）。次: Q1 の受け入れ、`CONFIG_DRIVER_BCM2712_VIDEO` の既定 y の判断（Q1）、SAODBLK_A・B と VPSSPSPPS_A の正しい参照での照合（ws322-p006 の残りと共に）、p004、速さ（phase 1・2 の重ね、割り込み）。

- 2026-11-02 08:45 P3（q1923）: p001 の設計を [phase001](phase001/phase.md) に書いた（出典は Raspberry Pi の Linux rpi-6.12.y の hevc_dec、GPL-2.0、値と手順だけ。firmware は無い）。次: p002。

- 2026-10-30 Q1: 作成。
- 2026-11-02 08:32 Q1: ユーザーの指示で P3（q1923）が p001〜p003 を続けて行う。

- 2026-11-02 10:17 Q1: P3 の p001〜p003 を受け入れて main に merge（fa5992dd0）。Pi 5 で HEVC_v1 128 本一致・15 本は理由付きで断る・3 本（SAODBLK_A/B、VPSSPSPPS_A）は参照の誤りの見込みで未解決。Video Player の GPU の道で H.265 1080p30・60 を再生。CONFIG_DRIVER_BCM2712_VIDEO を Pi 5 で既定 y にした（p003 が通ったため）。main で rpi5 の vmunix warning 0、hevc-slice の host 試験 PASS、menuconfig の host 試験 PASS。- 2026-11-02 10:42 Q1: SAODBLK_A/B は ffmpeg が誤った参照（RTX の NVDEC の 500 枚の I420 の md5 が適合の zip の md5 と一致、ffmpeg はどちらとも違う。WS324 P1）。この板の SAODBLK の不一致も、vkvideo-probe --raw と I420 の md5 で zip の md5 と照らし直す（次の再開の時）。
