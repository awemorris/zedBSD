<!-- awesome-plan project=zedbsd record=ws322-p006 -->

# ws322-p006 H.265 decode: PowerVR の executor の H.265 の wire の reader と Wave5 の HEVC の道

- 親: [WS322](../ws.md)。設計: [design.md](../design.md) 第 3 版の §7（Wave5 の decoder）・§8（executor）・§14.1・§14.3・§15 の p006。
- Status: in-progress（2026-11-02 07:26 P2 q1919-i01、worktree p2-ws322、branch agent/p2-ws322、基点 main 5d6afb993）
- 依存: p004（H.264 の道、engine の口）、p005（libvulkan の H.265 decode の転送、capset の bit 2。wire は main のまま変えない）。WS324 の libmedia の H.265 の解析（h265-stream・h265-dpb・h265-std・h265sw-*、merge 済み）。

## 目的

VF2 の Wave511 で H.265（Main、8 bit 4:2:0、progressive）を Vulkan Video の decode として出す。p005 が送る H.265 の wire を PowerVR の executor が読み、VPS・SPS・PPS を StdVideo の値から NAL に書き戻して slice の前に置き（stateful の firmware）、p004 の engine の道（ring、pool、生きた slot の frame buffer、pool から GPU の transfer）で decode する。

## 範囲

1. 試験の道具 vkvideo-probe の H.265（WS324 p006i の中身、2026-11-02 Q1 の判断 A で WS322 p006 が持つ。別の commit で先に出し、P1 の nvrtx の確かめにも使う）: libmedia の h265-stream・h265-dpb（17 slot、after_eos で DPB を flush しない: h265_dpb_begin に任せる）・h265-std で StdVideoH265 を作り、DPB の出す順に各 frame の NV12 の SHA-256 を出す（`--expect` は ffmpeg の framehash と比べる）。H.264 の道は変えない。
2. `src/drivers/vpu/vpu-codec.h`・`vpu-h265-ps.c`: StdVideoH265 の値（executor が写す自分の形）から VPS・SPS・PPS の NAL を書く（PTL、DecPicBufMgr、scaling list、short-term の set は明示の形、long-term、PCM、tile、deblocking、range extension の flag。VUI・HRD・timing は書かない: decode の値を変えない）。
3. `wave5-video.c`: stream の codec（H.264・H.265）、H.265 の parameter set の前置き（違う時か IRAP の時に VPS・SPS・PPS）、coded size は SPS の pic_width/height_in_luma_samples、instance は IRAP から、POC は PicOrderCntVal。engine の operations に H.265 decode。
4. executor `vk/vk-video.c`（と H.265 の読みを分けた `vk/vk-video-h265.c`）: H.265 の profile（Main、8 bit 4:2:0）・capabilities（H.265 の maxLevelIdc 5.1、64×64〜4096×2176（level 5.1 の MaxLumaPs）、17 slot・16 参照）・session（std header `VK_STD_vulkan_video_codec_h265_decode`）・parameters（VPS 16・SPS 16・PPS 64、鍵 (VPS, SPS, PPS)）・picture info と DPB slot info の読み、decode の job への写し。capset の bit 2 は engine が H.265 decode を出す時（`render-*.c` の capset、p003 の bit 0 と同じ所）。
5. host 試験 `plan/ws322/tests/p006/`: (T1) HEVC_v1 の 147 本の parameter set を h265sw-ps → h265-std → executor の形 → vpu-h265-ps → h265sw-ps で読み直して decode に効く field が全部一致、(T2) libvulkan の H.265 の encoder の bytes を vk-video の reader で読み直す（全 field）、(T3) engine の H.265 の道（偽の core）。p004・p005 の host 試験を流し直す。
6. 実機（VF2 10.0.30.2）: HEVC_v1 の Main の stream（class in の 136 本と派生 1 本のうち Wave511 の限り（Main、≤ 4096×2176、level ≤ 5.1）に入る物）を vkvideo-probe --expect で host の ffmpeg の NV12 の SHA-256 と照らす。1 本の script（配置 → 起動 → 実行 → 回収 → 要約）。p004・p007 の回帰を同じ板で流す。
- 範囲の外: Main10・4:2:2・interlaced（field_seq）、libmedia の H.265 の Vulkan の道と Video Player の H.265（WS318）、encode（p008〜）、wire・libvulkan の変更（WS324 と共有、変える時は Q1 経由）、速さの測り（p012）。

## UAPI・HAL

- HAL は変えない。`include/uapi/` は変えない（opcode は H.264 と同じ、capset の bit 2 は p005 の libvulkan の約束）。toolchain は触らない（worktree の sysroot の写しは p005 で作った物）。

## 受け入れ

| 項目 | 確かめ方 |
| --- | --- |
| host 試験 | `plan/ws322/tests/p006/run.sh` が全部 0 failed（T1 は 147 本の全部の parameter set）。`p004/run.sh`・`p005/run.sh` が前と同じ |
| build | VF2 の既定の config と `config-vf2-video.mk`・`config-vf2-video-test.mk` の vmunix と vkvideo-probe ほかが warning 0。amd64 の vkvideo-probe も warning 0 |
| HEVC の適合 | VF2 で vkvideo-probe --list が `VK_KHR_video_decode_h265` と codecs 0x3。Wave511 の限りに入る Main の各 stream が全部の frame で ffmpeg と一致するか、一致しない物・断る物はそれぞれ原因（firmware の限り、stream の性質）を示す。stream ごとの表を下の結果に |
| 非退行 | p004・p007 の板の回帰（H.264 の hash、JVT、立て直し、vf2-regress）が前と同じ形 |
| 後始末 | 板の vmunix・設定を元に戻し cksum が一致 |

## 結果

（実施中）

## 未実施

（実施中）
