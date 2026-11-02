<!-- awesome-plan project=zedbsd record=ws319 -->

# WS319 i915（5330、Alder Lake） の Vulkan Video: H.264 encode（VDEnc、HuC の firmware）、H.265 decode・encode

- Master: [plan/master.md](../master.md)（2026-10-30 作成）。Primary Milestone: MG006。
- Status: incomplete（2026-11-02 10:07 ユーザーの指示で P4 が H.265 decode に着手）。前: planning（2026-10-30 Q1 が作成。[WS318](../ws318/ws.md)（libmedia）の completed の後に始める）
- 目標: i915（5330、Alder Lake） の video の hardware のうち zedBSD で未実装の物を、kernel の Vulkan の executor（`src/drivers/gpu/i915/`）の Vulkan Video（`VK_KHR_video_decode_*`・`VK_KHR_video_encode_*`）として実装し、libmedia から使えるようにする。

## ユーザーの決定

- （2026-10-30）ユーザーから、H.264・H.265 の decoder・encoder の表の未の部分を埋めるように Vulkan Video の driver を実装する、driver は個別の WS に分けて依存にしてよい、先に libmedia を作り切ってから個別の driver を実装する、という趣旨の指示を受けた（詳しくは [WS318](../ws318/ws.md)）。
- （2026-10-30）ユーザーから、VPU が独立した hardware でも OS の API として Vulkan Video の driver として見せる、という趣旨の指示を受けた。
- （2026-10-30）ユーザーから、H.265 の patent pool への支払いは行う方針なので H.265 を実装してよい、という趣旨の決定を受けた。WS236（2026-10-14）の HEVC を行わない決定を置き換え、H.265 の decode・encode を実装する。

- （2026-11-02 10:07）ユーザーから、i915 の H.265 decode を P4 で実装する（5330 を使う）、H.264 の decode が Video Player で正常かも確かめて直す、という趣旨の指示を受けた（WS318 の completed を待たない）。

## Phase

| Phase | 目的 | 状態 | 依存 |
| --- | --- | --- | --- |
| ws319-p001 | 調べと設計: hardware の能力の確認、firmware、出典と license、Vulkan Video の対応の範囲、試験の道 | planning | WS318 completed |
| [ws319-p002](phase002/phase.md) | H.264 の Video Player の GPU の道（VP_GPU_PRESENT）を 5330 で確かめて直す | cleared（2026-11-02 P4 q1926-i01、Q1 が 12:05 に受け入れ、00ac64803 で main に merge） | WS083 |
| [ws319-p003](phase003/phase.md) | H.265 decode: HCP の long format を Vulkan Video として（設計・実装・host 試験） | cleared（2026-11-02 P4 q1926-i01、Q1 が 12:05 に受け入れ、00ac64803 で main に merge） | WS322 p005・p006 |
| [ws319-p004](phase004/phase.md) | 5330 の実機: HEVC_v1 の stream ごとの表、Video Player の H.265 の GPU の道 1080p30/60 | cleared（2026-11-02 P4 q1926-i01、Q1 が 12:05 に受け入れ、00ac64803 で main に merge） | p003 |

## Resume point

- 2026-11-02 12:05 Q1: p002〜p004 を受け入れて main に merge（00ac64803、source は先に 01c046344 までで merge 済み）。main で hevc-slice の host 試験 PASS（147 本、15 本は断る）。HEVC_v1 146 本: 128 本が全 frame 一致、3 本（SAODBLK_A・B、VPSSPSPPS_A）は ffmpeg の参照の誤りで i915 は正しい、15 本は理由付きで断る、失敗 0。Video Player の GPU の道: H.264・H.265 とも 1080p30 late 0、1080p60 は late 19（H.264）・52（H.265）。次の再開: 1080p60 の late（swapchain の acquire の待ち 8〜13 ms）、実の desktop の session での再生、H.264・H.265 encode。

- 2026-10-30 Q1: 作成。WS318 の completed の後。
- 2026-11-02 P4（q1926-i01）: p002 cleared（H.264 の GPU の道）、p003・p004 cleared（H.265 decode、HCP の long format、HEVC_v1 の Main 131 本が正しい）。残り: 1080p60 の late、H.264 encode・H.265 encode は未着手。試験は `plan/ws319/tests/`（run-5330-video.sh、make-clips.sh、probe-table.py、hcp/、hevc-slice/、gpu-hold/）。
