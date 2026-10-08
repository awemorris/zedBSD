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
| [ws083-p001](phase001/phase.md) | 設計（[design.md](design.md)） | in-progress → Q1 の判定待ち（第 3.1 版、review 3 回と人の判断は済み、2026-10-08 q897 の照合の review も blocking 無し） | — |
| [ws083-p002](phase002/phase.md) | libvulkan の骨組み（拡張・queue family・capability・format・session・parameters・record・sync2 の翻訳・capset の native の語）、host の試験 | in-progress（P1 q833 段 1〜3 の実装と host 試験済み、§8.2 の QEMU 回帰は T1-371 PASS） | p001、H1・H2・H3・HD1・HD6（決定済み） |
| [ws083-p003a](phase003a/phase.md) | i915: engine record VCS0、worker の engine ごとの context、遅延の VCS0 の context、hang の封じ込め | in-progress（P1 q833 実装と host 試験済み、実機は p005） | p001 |
| [ws083-p003b](phase003b/phase.md) | 実行器の video の module、capset の native の語と family 1、video の submit の骨組み | cleared（2026-10-07 Q1） | p002、p003a |
| [ws083-p004](phase004/phase.md) | MFX AVC の I frame の builder（genxml-video.h、NV12 Tile Y）、genxml の独立の decoder、試験の stream、`vkvideo-probe` | in-progress（q857、P2。host 試験済み、§8.2 の QEMU 回帰は T1-371 PASS（Q1 判定）、実機は p005） | p003b、H4・H5・HD4 |
| ws083-p005 | 実機: VCS の bring-up と I frame の hash（`i915.debug=video`）、HuC 不要の確認 | planning → T1-435（未実行、5330 の passthrough。2026-10-08 午後 ユーザー「5330はつけっぱなしですので、Videoのテストで使ってよいです」） | p004、5330 |
| [ws083-p006a](phase006a/phase.md) | P・B と DPB、scaling list の fall-back、複数 slice の host の試験（vkvideo-probe の DPB と表示順） | in-progress（P2、host の範囲は済み） | p004 |
| ws083-p006b | P・B と DPB の実機の hash | planning → T1-435 の C（未実行、p005 と同じ回） | p005、p006a |
| [ws083-p007](phase007/phase.md) | `GRDOM_MEDIA` の engine 単位の reset と VCS の hang の回復 | in-progress（q876、P2。host の範囲と review の R-S2・S3・S5 は済み（q897）。実機の人工の hang の道具は未作成、R-S4 は実機で） | p005 |
| ws083-p008 | 性能、`i915.debug=video` の門の既定化（p007 の後）、利用者への案内、SAMPLED・TRANSFER_SRC（HD5: 要らない）、result status query | in-progress（q897 で host の分: result status query と docs/reference/vulkan-video.md は済み。性能と `i915.debug=video` の既定化は実機の後） | p006b、p007（host の分は無し） |
| ws083-p009 | 全文規約確認と回帰（必須の最終確認） | planning（ベータ3、2026-10-08 ユーザー） | 全 Phase |

## 設計と実装の照合の review（2026-10-08 q897、design-reviewer、HEAD 40766787a、読むだけ）

blocking 無し。MFX の命令列（順・opcode・長さ・全 field）・slice の開始と長さ・zig-zag と default の表・wire の並び・NV12 の pitch と Y offset・GRDOM 0x20 は Mesa の genxml・ANV と一致。指摘と対応（2026-10-08 Q1 の判断 (a): R-S1・S2・S3・S5・S6 を p005 の前に q897 で直す、R-S4 は実機で）:

| ID | 内容 | 所在 | 案 |
| --- | --- | --- | --- |
| R-S1 | bitstream の Upper Bound が 4 KiB に整列していない（PRM は bits 47:12 の見込み、未確認）。ANV は 0 を書く | render/video.c（`bitstream_end`）、video-mfx.c | 4 KiB に切り上げるか ANV と同じ 0、U18 は実機で |
| R-S2 | video が止まった後の EIO（何も走らせない）を hang と見て無関係な session を quarantine | worker.c（`video_dead` で EIO）、render/video.c（EIO → quarantine） | 止まっている時は別の errno、quarantine は wait の ETIMEDOUT・EIO だけ |
| R-S3 | quarantine の session が新しい video session を作り decode を続けられる（hang の上限 3 を 1 つの app が使い切れる） | render/video.c の session create・`drv_i915_video_submit` | quarantined なら submit は DEVICE_LOST、create は INITIALIZATION_FAILED |
| R-S4 | ready-to-reset が来ないと engine reset を諦める（R1） | reset.c | VCS0 だけ handshake を省いた 2 回目の GDRST 0x20、実機で |
| R-S5 | VCS も RCS 用の 10 秒の timeout、worker 1 本なので desktop が止まる | worker.c | VCS0 は 1 秒程度 |
| R-S6 | 試験の空白: libvulkan の native の経路（context.c の 176 byte、`physical_load_video`、D3、device.c の依存）は host 試験が無い／D17 の 1〜4・7〜9 の境界試験が無い／Tile Y の試験は同じ関数で往復／golden は同じ判断（D21・upper bound・MOCS）から | plan/ws083/tests | native の経路の stub の host 試験、D17 の境界試験、実機で 64x64 の tile 0 の生の bytes を ffmpeg の plane と比べる手順 |
| R-M1〜M9 | setup の slot が参照と同じでも拒まない、4 byte の start code の直後の次の slice で BSD の長さ 0、skew＋開始の 29 bit の切り捨てを検べない、1 submit に session 5 つ以上で DEVICE_LOST、`i915_video_write` の ENOSPC が DEVICE_LOST、skip の log の static の数え上げ（32 回で無言）、parameters NULL の decode が skip（D18 が筋）、design §3.2 の event・ExecuteCommands と実装の違い、§6.2 の flush と 1 decode ごとの run | render/video.c ほか | 各々の案は review の原文（Q1 への報告）に |

対応（q897、2026-10-08 午後 P2、host・build）:
- R-S1（p004 の直し）: `render/video.c` の upper bound を buffer の終わりの page の終わりに切り上げ（memory は page で bind されるので同じ memory の中）。試験: roundtrip で 4000 byte の buffer の `MFX_IND_OBJ_BASE_ADDR_STATE` の upper bound が page の終わり。
- R-S2（p007 の直し）: worker は video が止まった後の VCS0 の request を `ECANCELED`（何も走らせない）で返し、`i915_video_run` は ECANCELED を quarantine にせず EIO（submit は DEVICE_LOST）。試験: roundtrip（quarantine にならない、log）、vcs-worker（止まった後の run は ECANCELED）。
- R-S3（p007 の直し）: quarantine の session は video の submit が DEVICE_LOST（log）、video session の create が INITIALIZATION_FAILED。試験: roundtrip（hang の後、engine が戻っても submit・create を拒む）。render の側の quarantine の門は WS083 の外（未対応、Q1 へ）。
- R-S5（p007 の直し）: VCS0 の request の timeout を 1 秒（`I915_WORKER_VIDEO_TIMEOUT_MS`）、RCS は 10 秒のまま。hang の log の ms は使った timeout。
- R-S6: (1) 新 `run-host-libvulkan-native.sh`: `host-libvulkan-native.c`（instance.c・device.c を取り込み、physical_load_queues・physical_load_video・D3 の濾し・拡張の列挙・device_validate の依存）と `host-libvulkan-capset.c`（context.c の 176 byte の native の語、open と ioctl を stand-in に）。(2) D17 の境界: 値の検べを `drv_i915_video_mfx_check_sets`（video-mfx.c へ移した、動作は同じ）にして `host-mfx-avc.c` の `case_set_bounds` で各値の最後に通る値と最初に断る値、roundtrip で item 3（image より大きい picture）・7（page に揃わない picture）・5（buffer を越える range）。item 8 の slice の長さと 9 の 256 を越える slice は wire の stream を変える必要があり未。(3) Tile Y の生の bytes の比べは実機の手順（T1-435 に足す文を Q1 へ）。(4) golden が同じ判断から作られている点は記録のみ。
- 他の WS の runner: `plan/ws101/tests/host/run.sh` の executor の list に `forget`（BUG-260 で増えた file、無いと link できない、main の時点で壊れていた）を足した。
- 確認: run-host-libvulkan-native・-status・-video、run-host-video-roundtrip、run-host-mfx-avc、run-host-vcs-worker、run-host-boot-video、ws031 run-vk-host-tests、ws075 run-host-layered、ws101 host/run.sh すべて PASS。`make -j16 BUILD=build/p2-k ZEDBSD_CONFIG=config/ci/config-amd64.mk ZEDBSD_USER_PROGRAMS="libvulkan vkvideo-probe" build/p2-k/vmunix build/p2-k/dynamic/libvulkan.so build/p2-k/bin/vkvideo-probe` warning 0。実機は未。

残件（2026-10-08 Q1 の指示で記録）: render 側の quarantine の門。hang した session（quarantined）でも render の stream の入口（`render/command.c` の `i915_command_render`）は quarantine を見ず、graphics の family の submit・新しい resource は続けられる（design §6.1 の「quarantine はその session の新しい resource・job を拒む」に届いていない）。R-S3 は video の submit と video session の create だけを拒む。直すなら WS031（i915 の Vulkan 実行器）の側の別 Queue。
