<!-- awesome-plan project=zedbsd record=ws324-p006a -->

# ws324-p006a H.265 の解析の設計（標準廉価実装手順 §4）と、header を source に置く段 2

Status: in-progress（2026-11-02 P1、q1864-i02。段 1 は Q1 の受け入れ済み（Q-A〜Q-E は全部 A）。段 2 済み: 基点 BASE `1f16db0e6`、確かめは下の「結果（段 2）」。cleared は Q1 の判定を待つ）

- WS: [WS324](../ws.md)。設計: [h265-parse-design.md](../h265-parse-design.md)。header（正本）: [proposed/h265/](../proposed/h265/)。review: [design-review.md](../design-review.md) の review 2。
- 背景: Q324-1（2026-11-01 Q1 の判断）で、H.265 の解析は WS318 p016〜p019 の範囲を標準廉価実装手順で先に作り、libmedia の build と runtime には繋がない。WS330 の Q330-2 の A（解析は 1 つ）により、WS330 の proposed の解析の header を土台にする。
- 仕様: `/home/awe/zedBSD-claude1/build/sources/specs/itu-t-h265-202601.pdf`（ITU-T H.265 (V11) (01/2026)、SHA-256 `78b093480e178a2b99d8cb7499934240f359c50c6d09568f8be651bc1151d4d2`、印刷の頁 N = PDF の頁 N + 18）。

## やる事

段 1（この起動、q1864-i01）:

1. 範囲・仕様の入手・module の分け・header（proposed）・試験の道具と vectors・実装の Phase の分け方と各 phase.md・受け入れを、手順 §4 の形で書く。
2. design-reviewer の敵対的レビューを受け、全件を design-review.md に表で記録し、反映するか理由を付けて見送る。

段 2（Q1 の受け入れの後の別の起動、Q-C）: design §9.1 の 1〜5（header を変えずに source に写す、骨格の .c、試験の道具、確かめ、基点 BASE の記録）。

## 受け入れ

- 段 1: design・proposed・phase006b〜h の phase.md・design-review.md の review 2（全件の反映の表）。Q1 への問いを背景・選択肢・推しつきで書いた（下）。
- 段 2: design §9.1 の 4 の確かめが全部 PASS、BASE の SHA を ws.md・design・各 phase.md に書いた。

## 結果（段 1、2026-11-01 P1）

- 作った物: [h265-parse-design.md](../h265-parse-design.md)、[proposed/h265/](../proposed/h265/) の header 11 個（`h265sw.h`・`h265sw-nal.h`・`h265sw-scaling.h`・`h265sw-vui.h`・`h265sw-rps.h`・`h265sw-ps.h`・`h265sw-slice.h`・`h265sw-poc.h`・`h265-dpb.h`・`h265-stream.h`・`h265-std.h`）、実装の Phase の [phase006b](../phase006b/phase.md)〜[phase006g](../phase006g/phase.md)（Sonnet）と [phase006h](../phase006h/phase.md)（Opus、適合）の phase.md。design-reviewer の review 2 は blocker 2・major 5・minor 13 の 20 件で、全件を反映した（PTL の general の bit 数、AU の境の決め方、EOS の付く絵、VPS の 16 bit、SPS の log2 の上限、p006f の分割など。表は design-review.md）。
- 確かめた事: 11 個の header が単独で 2 回 include して host の cc と clang の `-std=c89 -Wall -Wextra -Werror -Wpedantic` で通る（`vcodec*.h` は今の source の物、Vulkan は host の 1.4.309）。`sizeof` を host の amd64 で測った（design §6）。仕様の PDF の SHA-256 と頁の換算。HEVC_v1 の一覧（147 本）と 3 本の zip の中身、host の ffmpeg 7.1.5 の 2 つの副の oracle の出力の形（design §2.3）。Vulkan の H.265 の std の意味を docs.vulkan.org の本文で読んだ（design §2.2）。
- 読んだ code: libmedia の `vcodec*.h`・`h264.h`・`h264-dpb.h`・`sources.mk`、`vkvideo-probe` の `dpb.h`・`main.c`・`Makefile`、WS330 の proposed の解析の header と design、WS326 の試験の道具、WS322 の design の H.265 の部分。
- 未実施: source への配置・骨格・試験の道具（段 2）。Sonnet の起動。適合の stream の取得（cache は Q-D の後）。
- source は変えていない。HAL・toolchain は関係しない。
- 消す物（Q1 へ）: 無し（一時物は scratchpad だけ）。

## 結果（段 2、2026-11-02 P1、q1864-i02）

- 基点 BASE: **`1f16db0e6`**（commit 1f16db0e619a9cd23c74f7f36a8729efdba4ecb3、branch agent/p1-ws324、main 8f91a0d32 から）。ws.md・design §8.1.1・p006b〜h の phase.md の「確かめの command」に書いた（この記録は直後の plan の commit）。
- 作った物（BASE の commit）:
  - `userland/desktop/libmedia/` に header 11 個（`h265sw.h`・`h265sw-nal.h`・`-scaling.h`・`-vui.h`・`-rps.h`・`-ps.h`・`-slice.h`・`-poc.h`・`h265-dpb.h`・`h265-stream.h`・`h265-std.h`）。`plan/ws324/proposed/h265/` の物と `cmp` で同じ byte。
  - 骨格の .c 10 個（50 関数。int は `return ENOSYS;`、void は何もしない）。header の宣言の comment の最初の文を定義の comment にし、仕様は header の comment にあると書いた。`sources.mk`・`Makefile` には足していない（D3、Q324-1）。
  - `plan/ws324/tests/run-unit.sh`・`check-skeleton.sh`・`unit/h265write.h`（約 1 900 行、7.3 と Annex E の syntax の表から書いた書き手、`h265w_` の接頭辞）・`unit/test-h265write.c`（自己試験）。道具の口は design §8.1.1。
- 確かめ（command と最後の行）:
  - `sh plan/ws324/tests/check-skeleton.sh` → `check-skeleton: PASS`（host の cc 14.2 と clang 19 で header 11 個と .c 10 個、zedBSD の clang で x86_64・aarch64・riscv64 の syntax、layering、宣言 50 の定義、nm の名前、libmedia の build に無い事、vcodec*.h の 77ed48ac4 からの不変）。`BASE=1f16db0e6 sh plan/ws324/tests/check-skeleton.sh` → `check-skeleton: PASS`。zedBSD の `h265-std` は tree に `vk_video/vulkan_video_codec_h265std_decode.h` が無いので飛ばした（p002 の後に自動で入る）。
  - `sh plan/ws324/tests/run-unit.sh all` → `test-h265write: PASS (51 checks)`、`run-unit: PASS`。
  - 骨格で FAIL する形: scratchpad に置いた probe（各 module の関数を 1 つずつ header の言う結果で呼ぶ 13 checks）を `H265_UNIT_DIR=<scratchpad> sh plan/ws324/tests/run-unit.sh probe h265write` で流し、全 module が host の Vulkan の header で build・link でき、`test-probe: FAIL (12 of 13)`（残りの 1 は常に真の check）、無い試験の名前は `FAIL (no ...)`、最後の行 `run-unit: FAIL` を確かめた。
  - 書き手をわざと 8 通りに壊した（use_delta_flag の条件、fixed_pic_rate_within_cvs の推定、escape の 0x03 の境、collocated_from_l0 の推定、range extension の flag の順、slice_segment_address の bit 数、PTL の 43 bit、重みの flag）。escape の 1 通りを最初の試験が見逃したので、00 00 02 と 00 00 03 の escape の check を足して塞ぎ、全部 FAIL を確かめて `cmp` で戻した。
  - H.264 の非退行: `BASE=77ed48ac4 sh plan/tools/h264sw/check-skeleton.sh` → `check-skeleton: PASS`。`sh plan/tools/h264sw/run-unit.sh all` → 24 試験の全部が PASS、`run-unit: PASS`（変えた file は h264sw・vcodec の glob に入らないので、前と同じ物を流した事になる）。
  - `git diff --check` → 出力無し。
- 適合の stream: `H265SW_CONFORMANCE_DIR=/home/awe/zedBSD-claude1/build/sources/h265-conformance sh plan/tools/h265/conformance/fetch.sh` を流している（結果は次の commit で書く）。
- 自分で決めた点:
  - check-skeleton に design §8.1 の 5 項目へ 3 つ足した: zedBSD の clang（3 つの arch）での syntax、宣言の全部の定義と nm の名前空間、`sources.mk`・`Makefile` が h265 を名指さない事。BASE の不変の対象に道具（run-unit.sh・check-skeleton.sh・h265write.h・test-h265write.c・h264sw の check.h・bitwriter.h）を入れた（WS335 の形。足し・消しも見る）。
  - `run-unit.sh` に `H265_UNIT_DIR`（道具の自己確かめ用）を足した。PASS の行で終わらない試験を FAIL にした。
  - `h265write.h` は導出をしない書き手にした（予測の set の NumDeltaPocs[ RefRpsIdx ]、slice header の文脈は試験が渡す）。値の field は全部 32 bit で範囲の外も書ける。数が配列を超える時は数だけ書き、entry は配列の分まで。
  - 骨格の判定の関数（`h265sw_nal_is_*` など）も設計 §9.1 のとおり ENOSYS を返す（骨格では真に見える）。
- 未実施: Sonnet の Phase の起動（Q1）。p002 の前の zedBSD での `h265-std` の確かめ。
- 消す物（Q1 へ）: worktree の `build/tmp/h265-unit.run.*`・`build/tmp/h265-skeleton.run.*`・`build/tmp/h264sw-unit.run.*`・`build/tmp/h264sw-skeleton.run.*`（使い捨ての出力）。scratchpad の一時物は session の物。

## Q1 の判断を待つ点

前提: WS324 は RTX 2070（NVIDIA の Turing、amd64 の PC 10.0.10.3）の video の hardware を zedBSD の Vulkan Video として出す WS。H.265（HEVC: H.264 の後継の動画の符号化の規格）の decode では、hardware は bitstream の全部を解かず、app の側が parameter set（VPS・SPS・PPS: 動画全体・sequence・picture の設定の塊）と、絵ごとの参照の情報（POC: 表示の順の番号、RPS: どの過去の絵を参照に残すかの組、DPB: 復号した絵を参照と表示のために置いておく buffer）を Vulkan の決まった構造体（StdVideoH265）で渡す。この Phase はその解析の module の設計で、実装は安い model（Sonnet）に任せる手順（標準廉価実装手順）を使う。ユーザーの判断が要る点は無く、以下は Q1 の技術の判断。

- **Q-A**（推し A）: 解析の header を WS330（H.265 の software の decoder の WS）の proposed から採って足す（design D1）。A: 採る（Q330-2 の A「解析は 1 つ」に沿い、WS330 の Sonnet の 7 Phase が確かめだけになる）。B: Vulkan 用に別の解析を作る（同じ仕様の節を 2 回実装し 2 回照合）。
- **Q-B**（推し A）: Phase を design §9 の形にする（Sonnet 6 つ p006b〜g、適合 p006h、probe p006i）。ws.md の案の p006b〜d・p006e の名前を置き換え、p007 の依存を p006i にする。A: 6 つ（1 起動に header 1〜2 の手順の規則に合う。std と stream は review で 1 回に大きすぎると指摘され分けた）。B: 3 つに詰める（1 起動に 4 header 以上、WS326 の教訓に反する）。
- **Q-C**（推し A）: 段 2（header を source に置き骨格と道具を作り基点を決める）を P1 が p006a の 2 回目の起動で行う。A: そうする。B: 別の番号の Phase にする。
- **Q-D**（推し A）: WS330 の適合の一覧と取得の script（`plan/tools/h265/conformance/`）を共有の `plan/tools/h265/conformance/` へ移し plan/tools.md に登録、cache `/home/awe/zedBSD-claude1/build/sources/h265-conformance/` を作る（Q1 の作業）。A: 移す。B: WS330 の path をそのまま使う（WS330 の完了で消える）。
- **Q-E**（推し A）: Vulkan の `NumDeltaPocsOfRefRpsIdx` の値の約束（design D8: slice header が予測の set を持つ時の NumDeltaPocs[RefRpsIdx]、他は 0）。Vulkan の本文は「`short_term_ref_pic_set_sps_flag` が 1 の時」と書くが、その時 RefRpsIdx は定まらず、値の用途（hardware が slice header の予測の set を読む）とも合わない。使い手は zedBSD の driver（nvrtx・Wave511）だけ。A: 約束を採る。B: 本文の字句どおり。

## 他の WS の記録（Q1 へ、design §10）

- WS330: proposed の解析の 7 header は WS324 の source の物が正になる。WS330 p003・p004・p006〜p011 の扱いの直し（確かめだけ）、`h265sw_rps_read` の引数の追加、D5 で外した物の追加は WS330 p003。
- WS318: p016〜p019 は WS324 p006a〜h で済む（Q324-1 の記録に足す）。
- WS322: p006 は `h265-std` の約束（D7〜D9）を入力にする。
