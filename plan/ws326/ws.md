<!-- awesome-plan project=zedbsd record=ws326 -->

# WS326 libmedia の H.264 の software の decoder（独自の実装）

- Master: [plan/master.md](../master.md)（2026-10-30 作成）。Primary Milestone: MG006。
- Status: incomplete（2026-10-30 Q1 が作成、p001 から）
- 目標: libmedia に、既存の library を使わない H.264 の software の decoder を作る。範囲は progressive の 8 bit 4:2:0、Constrained Baseline・Main・High（interlace（PAFF・MBAFF）・4:2:2・10 bit は範囲の外）。
  Vulkan Video の hardware が無い機械（Pi 5 など）で libavcodec に頼らずに再生する道。libmedia の backend の順は Vulkan Video → この独自の software の decoder → libavcodec（2026-10-30 ユーザー「libmedia の backend の順番は、Vulkan Video --> 独自ソフトウェアdecoder --> libavcodecです。」）。

## ユーザーの決定（2026-10-30、原文）

1. 「H.264デコーダのソフトウェア実装は、どれくらいの規模見積もりになりますか？LWでいうと。」（Q1 の見積もり: 正しく decode できるまで約 60〜80 LW、1080p を CPU で実用の速さにするまで含めて 80〜120 LW）
2. 「libmediaへのH.264デコーダの実装について、Claudeの安いモデルに外注することを検討しています。全体の骨格を、モジュールをわけてファイルを作り、モジュールのインタフェース（定数、構造体、関数）をヘッダで定義しておき、中身はヘッダの関数のコメントで仕様を定義しておいて、モジュールの.cファイルの実装は、実装するLLMに任せる、という構成にしたいです。WSを立てて、phaseを作成し、最初のphaseでこのモジュール設計だけを実施して、実装のフェーズはSonnet 5.5 Highのサブエージェントで実行する、というストラテジにしたいです。」

3. 「WS326ですが、共通部分はH.265でも使えるように、h264_のような名前でなく、汎用にするよう、記録しておいてください。」

## 進め方（上の原文の解釈、Q1）

- p001（設計、Opus）: module に分け、各 module の header（定数・構造体・関数）を書き、各関数の comment に仕様（入力・出力・前提・誤りの扱い・ISO/IEC 14496-10 の節）を書く。.c は空の骨格だけ（関数は未実装の印）。module ごとの host の単体試験の仕様と、全体の適合の試験（JVT の conformance の bitstream と hash の照合）の道も決める。module の依存の順と、実装の Phase の割り振り（1 Phase = Sonnet の 1 回の起動で済む大きさ）を表にする。
- p002 以降（実装、Sonnet 5.5 の high の subagent）: header を変えずに .c を実装し、単体試験を通す。header の不備は直さず Q1 に返す（Q1 か Opus が header を直す）。
- 最後に組み込み（libmedia の backend）、適合の試験、速さ、規約の全文の見直し。
- 決定 3 の解釈: H.265 でも使える部分（例: bit の読み、Exp-Golomb、NAL・RBSP（emulation prevention）、CABAC の算術の engine と状態の遷移の表、frame・plane の確保、DPB・出力の順の共通の部分、試験の道具）は `h264sw` の名前でなく codec に依らない名前の module にする。
  今動いている実装の Phase（header は凍結）を乱さないため、名前の移しは p026（適合の試験 p022 の後、規約の見直し p025 の前）で行い、単体試験と適合の試験が前後で同じであることで確かめる。
  どこまでを共通にするかと名前は p026 の設計（Opus）で決め、[WS329](../ws329/ws.md)（H.264 の encoder）・[WS330](../ws330/ws.md)（H.265 の decoder・encoder）の共通化の設計はその上に足す。

## Q1 の判断（2026-10-30、p001 の問い）

1. h264sw.h の「節の番号は ITU-T H.264 (08/2021)」の記述は定数の block の comment なら guardrail に合う（file の先頭でない）。
2. 適合の bitstream の cache は main の共有の `/home/awe/zedBSD-claude1/build/sources/h264-conformance/`（git の外、消さない場所）。実装の担当は `H264SW_CONFORMANCE_DIR` にそこを渡す。p001 で取った 3 本は写し済み。
3. profile の範囲: Extended（constraint_set0/1 が 1）や High 10・4:2:2・4:4:4 Predictive の profile_idc でも、中身が 8 bit 4:2:0 の frame なら受ける（A.2）は、ユーザーの「Constrained Baseline・Main・High」を狭めない技術の判断として採る（ユーザーに報告）。
4. 実装の Phase は、依存の無い物から並べて Sonnet 5.5 high の subagent を別々の worktree で動かす（p002・p008・p010 から）。

## Phase

設計は [design.md](design.md)（module・決定・所有・試験の道）、各 module の仕様は `userland/desktop/libmedia/h264sw*.h` の comment。p002〜p020 は Sonnet 5.5 high の 1 回の起動で 1 Phase（header 1〜2 個）、依存の Phase の実装が main に入ってから始める。
p021（最初の全体の試験、module の間の誤りがここで出る）・p022（適合）・p024（速さの設計）は Opus を推す（Q1 の判断）。header の基点は fd1d6f88c（実装の Phase は header を変えない、`BASE=fd1d6f88c sh plan/ws326/tests/check-skeleton.sh`）。

| Phase | 目的 | 状態 | 依存 |
| --- | --- | --- | --- |
| [ws326-p001](phase001/phase.md) | module の設計: file の骨格・header の interface と仕様の comment・単体と適合の試験の仕様・実装の Phase の割り振り（Opus） | cleared（2026-10-30 Q1。header と骨格 fd1d6f88c を main に merge 済み、以後の実装の Phase の BASE） | — |
| [ws326-p002](phase002/phase.md) | bit reader と NAL: h264sw-bits、h264sw-nal（試験 test-bits、test-nal） | cleared（2026-10-30 Q1、main 711b1ba4c。test-bits 169・test-nal 95 checks） | p001 |
| [ws326-p003](phase003/phase.md) | 共有の表と parameter set: h264sw-tables、h264sw-ps（試験 test-tables、test-ps） | cleared（2026-10-30 Q1、main a29ef7eaa。test-tables 846・test-ps 41887 checks） | p002 |
| [ws326-p004](phase004/phase.md) | slice header: h264sw-slice（試験 test-slice） | cleared（2026-10-30 Q1、main a10770ab8。test-slice 141694 checks） | p003 |
| [ws326-p005](phase005/phase.md) | frame と POC: h264sw-frame、h264sw-poc（試験 test-frame、test-poc） | cleared（2026-10-30 Q1、main 405626548。test-frame 284・test-poc 382 checks、突然変異 17 のうち 16 を検出（残りは観測できない padding）） | p003、p004 |
| [ws326-p006](phase006/phase.md) | DPB（marking・gap・出力の順）: h264sw-dpb（試験 test-dpb） | cleared（2026-10-30 Q1、main 4213dfb23。test-dpb 129447 checks、突然変異 24 で残りは観測できない 1 つ） | p005 |
| [ws326-p007](phase007/phase.md) | reference picture list: h264sw-reflist（試験 test-reflist） | cleared（2026-10-30 Q1、main 5d43ad00d。test-reflist 278758 checks、突然変異 23 で残りは観測できない 2 つ。B の POC type 0 の非存在の frame の扱いは header と仕様が食い違い、p021 の前に header を直す） | p006 |
| [ws326-p008](phase008/phase.md) | macroblock の型と neighbour: h264sw-mb（試験 test-mb） | cleared（2026-10-30 Q1、main bc2cf3321。test-mb 1938 checks） | p001 |
| [ws326-p009](phase009/phase.md) | CAVLC: h264sw-cavlc（試験 test-cavlc） | cleared（2026-10-30 Q1、main 7a918c446。test-cavlc 87868 checks。Kraft の和は仕様の表どおり 1 未満の列がある（phase.md の記述の誤り、Q1 が Sonnet の値を採った）） | p002、p008 |
| [ws326-p010](phase010/phase.md) | CABAC の context の初期値の表: h264sw-cabac-init（試験 test-cabac_init） | cleared（2026-10-30 Q1、main c294565dd。1840 対の値を仕様の PDF と独立の抜き出しで照合、4159 checks） | p001 |
| [ws326-p011](phase011/phase.md) | CABAC の engine: h264sw-cabac（試験 test-cabac） | cleared（2026-10-30 Q1、main 8627fc588。test-cabac 195654 checks、突然変異 5 つで FAIL を確認） | p002、p010 |
| [ws326-p012](phase012/phase.md) | CABAC の syntax element: h264sw-cabac-se（試験 test-cabac_se） | cleared（2026-10-30 Q1、main 00ecfcc80。test-cabac_se 168241 checks、突然変異 15 のうち 14 を検出（残り 1 は等価）） | p008、p011 |
| [ws326-p013](phase013/phase.md) | 変換と逆量子化: h264sw-transform（試験 test-transform） | cleared（2026-10-30 Q1、main 4c2e27dd5。test-transform 33158 checks） | p003 |
| [ws326-p014](phase014/phase.md) | intra 予測: h264sw-intra（試験 test-intra） | cleared（2026-10-30 Q1、main dfd380ae5。test-intra 593 checks） | p008 |
| [ws326-p015](phase015/phase.md) | 補間と重み: h264sw-interp、h264sw-weight（試験 test-interp、test-weight） | in-progress（q1748） | p007 |
| [ws326-p016](phase016/phase.md) | MV の予測と P_Skip: h264sw-mvpred（試験 test-mvpred） | cleared（2026-10-30 Q1、main 4f596bbc6。test-mvpred 83 checks） | p008 |
| [ws326-p017](phase017/phase.md) | B の direct: h264sw-direct（試験 test-direct） | in-progress（q1749） | p007、p008、p016 |
| [ws326-p018](phase018/phase.md) | slice data と macroblock の syntax: h264sw-macroblock（試験 test-macroblock） | cleared（2026-10-30 Q1、main 3195bbd68。test-macroblock 1107981 checks、突然変異 29 で残りは同値 1・CABAC の residual は p021・p022 で） | p002、p008、p009、p012 |
| [ws326-p019](phase019/phase.md) | macroblock の再構成: h264sw-recon（試験 test-recon） | planned | p013〜p018 |
| [ws326-p020](phase020/phase.md) | deblocking: h264sw-deblock（試験 test-deblock） | cleared（2026-10-30 Q1、main 9fa35fc76。test-deblock 3313 checks、突然変異約 35 で残りは観測できない 1 つ） | p003、p008 |
| [ws326-p021](phase021/phase.md) | decoder の全体の流れと最初の全体の試験: h264sw-decoder（試験 —） | planned | p002〜p020 |
| [ws326-p022](phase022/phase.md) | JVT の適合の試験: —（試験 —） | planned | p021 |
| [ws326-p023](phase023/phase.md) | libmedia の backend と backend の切り替え: h264sw-backend（試験 —） | planned | p021（p022 を推す） |
| [ws326-p024](phase024/phase.md) | 速さの設計と最初の最適化: —（試験 —） | planned | p022、p023 |
| ws326-p026 | H.265 でも使える部分を codec に依らない名前の module へ移す（設計 Opus、移しは Sonnet 可、試験は全単体と適合の前後の一致） | planned | p022 |
| [ws326-p025](phase025/phase.md) | 規約の全文の見直し: —（試験 —） | planned | p023・p026（と速さの Phase） |

## Resume point

- 2026-10-30 Q1: 作成。P1 が q1695 で p001。
- 2026-10-30 P1（q1695-i01）: p001 の成果を branch agent/p1-ws326 に（header と骨格 fd1d6f88c、plan は次の commit）。design-reviewer の 23 件を反映（design.md §9）。次: Q1 が受け入れて main に merge、p002 から（p008・p010 は p001 だけに依存、並べられる）。
- 2026-10-30 Q1: ユーザーの決定 3（共通部分を汎用の名前に）を記録し、p026 を足した。p020 cleared（main 9fa35fc76）。
- 2026-10-31 Q1: ユーザーから、libmedia の実装の WS を昼までに終えるよう優先度を上げる、という趣旨の指示を受けた。p015（q1748）・p017（q1749）を Sonnet 2 人で同時に起動。
