<!-- awesome-plan project=zedbsd record=ws083 -->

# WS083: Vulkan Video の拡張と i915 の対応（最初の目標 H.264 の decode）

<!-- awesome-plan-current:start -->
Status: incomplete（host で作れる範囲は p002〜p004・p006a・p007 まで実装と host 試験済み、実機（5330）待ち）
Primary Milestone: MG006
Related Milestones: MG002
Objectives: O1
Parent: [Master](../master.md)
Queue: q897（P2、2026-10-08 午後: 設計と実装の照合の review、p008 の host の分）
Resume point: 2026-10-08 午後 P2（q897）: 人の判断 H1〜H5・HD1〜HD6 は 2026-10-07 に全部決定（master.md の 2026-10-07 の行）、design は第 3.1 版（review 3 回、§15〜§17）。p002・p003a は実装と host 試験済み、p003b cleared、p004 は T1-371（QEMU §8.2）PASS、p006a・p007 は host の範囲が済み。残りは実機（p005 の VCS の bring-up と I frame の hash、p006b の P・B の hash、p007 の人工の hang からの回復; 5330 の UAT の後に Q1 が T1 へ）と p008 の host の分（result status query、利用者への案内）。p009（全文規約の見直し）はベータ3（2026-10-08 ユーザー「コーディング規約による整形はベータ3でやります」）。
2026-10-02 user: fg019（ベータ1、10/17）に入れる。「Vulkanのビデオ再生拡張をIntel Xe-LPで実装する。H.264を最初のターゲットとする。」動画プレーヤ（WS122）・ブラウザ（WS121）の土台（VA-API の WS123 は canceled、アプリが Vulkan Video を直接使う）。2026-10-07 ユーザーの回答で、p001・p002 と host で作れる所までエージェントが進める（10-02 の「別セッション」の指示を置き換え）。
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
| [ws083-p001](phase001/phase.md) | 設計（[design.md](design.md)） | in-progress（第 3.1 版、review 3 回と人の判断は済み、Q1 の ACK で cleared の判定） | — |
| [ws083-p002](phase002/phase.md) | libvulkan の骨組み（拡張・queue family・capability・format・session・parameters・record・sync2 の翻訳・capset の native の語）、host の試験 | in-progress（P1 q833 段 1〜3 の実装と host 試験済み、§8.2 の QEMU 回帰は T1-371 PASS） | p001、H1・H2・H3・HD1・HD6（決定済み） |
| [ws083-p003a](phase003a/phase.md) | i915: engine record VCS0、worker の engine ごとの context、遅延の VCS0 の context、hang の封じ込め | in-progress（P1 q833 実装と host 試験済み、実機は p005） | p001 |
| [ws083-p003b](phase003b/phase.md) | 実行器の video の module、capset の native の語と family 1、video の submit の骨組み | cleared（2026-10-07 Q1） | p002、p003a |
| [ws083-p004](phase004/phase.md) | MFX AVC の I frame の builder（genxml-video.h、NV12 Tile Y）、genxml の独立の decoder、試験の stream、`vkvideo-probe` | in-progress（q857、P2。host 試験済み、§8.2 の QEMU 回帰は T1-371 PASS（Q1 判定）、実機は p005） | p003b、H4・H5・HD4 |
| ws083-p005 | 実機: VCS の bring-up と I frame の hash（`i915.debug=video`）、HuC 不要の確認 | planning（5330 の UAT の後、Q1 が T1 へ） | p004、5330 |
| [ws083-p006a](phase006a/phase.md) | P・B と DPB、scaling list の fall-back、複数 slice の host の試験（vkvideo-probe の DPB と表示順） | in-progress（P2、host の範囲は済み） | p004 |
| ws083-p006b | P・B と DPB の実機の hash | planning（5330） | p005、p006a |
| [ws083-p007](phase007/phase.md) | `GRDOM_MEDIA` の engine 単位の reset と VCS の hang の回復 | in-progress（q876、P2。host の範囲は済み、実機の人工の hang は 5330 の後、hang を起こす試験の道具は未作成） | p005 |
| ws083-p008 | 性能、`i915.debug=video` の門の既定化（p007 の後）、利用者への案内、SAMPLED・TRANSFER_SRC（HD5: 要らない）、result status query | planning（q897 で host の分: result status query と案内） | p006b、p007（host の分は無し） |
| ws083-p009 | 全文規約確認と回帰（必須の最終確認） | planning（ベータ3、2026-10-08 ユーザー） | 全 Phase |
