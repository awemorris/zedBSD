<!-- awesome-plan project=zedbsd record=ws083 -->

# WS083: Vulkan Video の拡張と i915 の対応（最初の目標 H.264 の decode）

<!-- awesome-plan-current:start -->
Status: planning（p001 の設計の第 2 版、P1）
Primary Milestone: MG006
Related Milestones: MG002
Objectives: O1
Parent: [Master](../master.md)
Queue: なし
Resume point: 2026-10-07 夕 P1（q833）: p001 の設計の第 2 版（[design.md](design.md)、review の反映は §14）、UAPI の差分は [proposed/](proposed/README.md)。次は design-reviewer の再 review と人の判断（design.md §10 の H1〜H5・HD1〜HD6）。判断の要らない p003 の engine record・worker の部分から実装できる。2026-10-07 ユーザーの回答で、p001・p002 と host で作れる所まで P1 が進める（10-02 の「別セッション」の指示を置き換え、p003 以降の実機は 5330 が戻ってから）。以前: p001（設計）から。OSC のデモ（fg010）には必須ではない
2026-10-02 user: fg019（ベータ1、10/17）に入れる。「Vulkanのビデオ再生拡張をIntel Xe-LPで実装する。H.264を最初のターゲットとする。」動画プレーヤ（WS122）・ブラウザ（WS121）の土台（VA-API の WS123 は canceled、アプリが Vulkan Video を直接使う）。**別セッションでユーザーと進める。このセッションは割り当てない。ベータ1 では drop 可の努力目標。**
<!-- awesome-plan-current:end -->

## 目標（2026-09-28 ユーザー）

「WSを追加します。OSCデモの必須ではないです。Vulkan Video拡張を追加して、i915のドライバ対応を行います。H.264を最初のターゲットにします。」

- 自前の libvulkan（`userland/desktop/libvulkan`）に Vulkan Video の拡張（`VK_KHR_video_queue`・`VK_KHR_video_decode_queue`・
  `VK_KHR_video_decode_h264`）を足し、i915 の driver（5330、Alder Lake、Gen12）の video の engine（VCS、MFX）で H.264 を decode する。
- 最初の到達点: H.264 の elementary stream（8 bit・4:2:0・progressive、Baseline/Main/High の一般的な形）を decode し、各 frame が
  参照の decoder（host の ffmpeg 等）の出力と一致する。I frame から、次に P・B と DPB。
- encode（`VK_KHR_video_encode_h264`）・H.265・AV1 は後（Future）。

## 設計で決めること（p001）

- **API の面**: video の queue family と capability の報告、`VkVideoSessionKHR`・session parameters（SPS・PPS）、video の format と
  image の用途（decode の出力・DPB）、`vkCmdBeginVideoCodingKHR`・`vkCmdDecodeVideoKHR`、query（decode の結果の status）。
  Vulkan Video では app が bitstream の SPS・PPS・slice header を解析して `StdVideo*` の構造体で渡す。
- **i915 の側**: VCS（video の engine）の立ち上げ（ring・context・submission、今の render の engine の仕組みとの共通化）、Gen12 の MFX の
  AVC の VLD の decode の command（`MFX_PIPE_MODE_SELECT`・`MFX_SURFACE_STATE`・`MFX_PIPE_BUF_ADDR_STATE`・`MFX_IND_OBJ_BASE_ADDR_STATE`・
  `MFX_BSP_BUF_BASE_ADDR_STATE`・`MFX_AVC_IMG_STATE`・`MFX_QM_STATE`・`MFX_AVC_DIRECTMODE_STATE`・`MFX_AVC_SLICE_STATE`・
  `MFD_AVC_BSD_OBJECT` 等）、NV12 の tile の形、firmware（H.264 の decode に HuC が要らないことの確認）。資料は Intel の公開の PRM と Mesa・
  intel-media-driver を参照し、code は写さない（license の方針に従う）。
- **memory**: bitstream の buffer、DPB の image の配置、MV の buffer、row store 等の scratch。
- **試験**: 実機（5330）だけ（Venus・lavapipe は Vulkan Video を持たない見込み）。試験の stream の license（自作の stream を host の ffmpeg・
  x264 で作り、tree には入れない、または自由な license のもの）。frame ごとの hash で参照と比べる。
- **利用者**: 試験の app（例 `vkvideo-probe`）。その後に mview・ブラウザの `<video>`（WS074 の範囲の外）等の利用は別に判断する。

## Phase（案）

| Phase | 目的 | Status | 依存 |
| --- | --- | --- | --- |
| [ws083-p001](phase001/phase.md) | 設計（[design.md](design.md)） | in-progress（P1、第 2 版） | — |
| ws083-p002 | libvulkan の骨組み（拡張・queue family・capability・format・session・parameters・record・sync2 の翻訳・capset の native の語）、host の試験、T1 の QEMU の回帰 | planning | p001、H1・H2・H3・HD1・HD6 |
| ws083-p003 | i915: engine record VCS0、worker の engine ごとの context、遅延の VCS0 の context、hang の封じ込め、video の object・slot 表・kernel の検べ、host の試験 | planning | p001（video の object は H1） |
| [ws083-p004](phase004/phase.md) | MFX AVC の I frame の builder（genxml-video.h、NV12 Tile Y）、genxml の独立の decoder、試験の stream、`vkvideo-probe`（host で build） | in-progress（q857、P2。実装と host 試験済み、§8.2 の QEMU 回帰は T1 待ち） | p003、H4・H5・HD4 |
| ws083-p005 | 実機: VCS の bring-up と I frame の hash（`i915.debug=video`）、HuC 不要の確認 | planning | p004、5330 |
| [ws083-p006a](phase006a/phase.md) | P・B と DPB、scaling list の fall-back、複数 slice の host の試験（vkvideo-probe の DPB と表示順） | in-progress（P2、host の範囲は済み） | p004 |
| ws083-p006b | P・B と DPB の実機の hash | planning | p005、p006a |
| [ws083-p007](phase007/phase.md) | `GRDOM_MEDIA` の engine 単位の reset と VCS の hang の回復（design §9 の p007、HD2 (a) の前提） | in-progress（q876、P2。host の範囲は実装と host 試験済み、実機の人工の hang は 5330 の後） | p005 |
| ws083-p008 | 性能、`i915.debug=video` の門の既定化（p007 の後）、利用者への案内、SAMPLED・TRANSFER_SRC（HD5）、result status query | planning | p006b、p007 |
| ws083-p009 | 全文規約確認と回帰（必須の最終確認） | planning | 全 Phase |
