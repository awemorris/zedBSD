<!-- awesome-plan project=zedbsd record=ws329 -->

# WS329 libmedia の H.264 の software の encoder（独自の実装、標準廉価実装手順）

- Master: [plan/master.md](../master.md)（2026-10-30 作成）。Primary Milestone: MG006。
- Status: planning（2026-10-30 Q1 が作成。2026-10-31 p001 の設計を P5 が先に進めた。実装の Phase は [WS326](../ws326/ws.md)（H.264 の software の decoder）の completed の後）
- 目標: libmedia に、既存の library を使わない H.264 の software の encoder を作る。手順は標準廉価実装手順（[docs/agent/spec-module-delegation.md](../../docs/agent/spec-module-delegation.md)）。
  libmedia の encoder の口（[WS318](../ws318/ws.md) の `media_encoder_*`、codec の問い合わせと指定）の backend の 1 つで、順は Vulkan Video の encode → この encoder → libavcodec（[WS326](../ws326/ws.md) の decoder の順の決定に合わせる、Q1 の解釈）。

## ユーザーの決定（2026-10-30、原文）

「標準廉価実装手順で、libmediaに、H.264エンコーダを追加するWSをそれぞれ作ります。H.264デコーダの実装のうち再利用できるところを洗い出して、デコード専用ではなく共通関数にするのを、Opus 5.5が設計段階で行います。そのあと、必要な追加実装を設計済みにしてSonnetに委譲します。H.264デコーダの実装が完了してから取り組みます。」

2026-10-30（WS330 を立てた時）: 「H.265のエンコーダ、デコーダをlibmediaに追加するWSを1つ立てます。…H.264エンコーダと同様に、共通部分を洗い出してから、設計を行い、そのあとでSonnetに委譲する計画設計にしてくだしあ。」（Q1 の解釈: 先の「WSをそれぞれ作ります」は H.264 の encoder がこの WS329、H.265 は WS330）

- 2026-10-31: ユーザーから、不在の間（昼頃まで）は設計だけ進められる WS の設計を進め、libmedia の WS の優先度を上げる、という趣旨の指示を受けた（Q1 経由）。これにより p001（設計）を WS326 の completed の前に、WS326 の凍結済みの header を入力にして行った。実装の Phase は WS326 の completed の後のまま。

## 進め方（上の原文の解釈、Q1）

- p001（設計、Opus 5.5）:
  1. WS326 の decoder の module（h264sw-*）から、encoder でも使える物を洗い出す（例: bit の読み書き、NAL と RBSP の組み立て、SPS・PPS・slice header の構造体、表、変換と逆変換・量子化、intra の予測、動きの補間、deblocking、DPB と参照の list、CAVLC・CABAC の表と文脈）。
  2. それらを decode 専用でない共通の関数・module にする設計（H.265 でも使える物は codec に依らない名前にする。[WS326](../ws326/ws.md) の決定 3 と p026 の上に足す）（名前・header の分け方、decoder の既存の試験を壊さない移し方）。共通化の refactor 自体は p001 の設計に沿って Phase として行い、decoder の単体試験と適合の試験が前後で同じであることで確かめる。
  3. encoder に要る追加の module（bit の書き、CAVLC・CABAC の符号化、mode の選び（intra・inter）、動きの探索、rate control、frame の種類と参照の構造、出力の NAL）の header（interface と仕様の comment）と骨格、試験の道（decoder との往復、適合の確かめ）、実装の Phase の表と各 phase.md。
  4. design-reviewer。
- p002 以降（実装、Sonnet 5.5 high）: 標準廉価実装手順のとおり。
- 範囲（案、p001 で詰める）: decoder と同じ 8 bit 4:2:0 progressive、Constrained Baseline・Main・High の encode。速さ・画質の目標は p001 で示す。

## Phase

設計は [design.md](design.md)（決定・decoder との共通化・module・試験の道・Phase の割り振り）、各 module の仕様は [proposed/](proposed/) の header の comment（p003 で source に置き、その commit を実装の Phase の基点 BASE にする）。ユーザーの判断を待つ点は [user-questions.md](user-questions.md)（Q329-1〜5）、敵対的レビューは [design-review.md](design-review.md)。
型: O = Opus、S = Sonnet 5.5 high（標準廉価実装手順、1 回の起動）、D = conformance-reviewer。

| Phase | 目的 | 型 | 状態 | 依存 |
| --- | --- | --- | --- | --- |
| ws329-p001 | 設計（Opus 5.5）: decoder との共通化の洗い出しと設計、encoder の module・header の案・試験の道・実装の Phase の表 | O | in-progress（q1758 P5。設計と review（29 件）の反映は済み、Q1 の受け入れと Q329-1〜5 の回答待ち） | — |
| [ws329-p002](phase002/phase.md) | 共通化の refactor（CAVLC の表・CABAC の context の選び・context の初期化・表 A-1、decoder の振る舞いは不変） | O | planned | WS326 completed（p026 を含む） |
| [ws329-p003](phase003/phase.md) | encoder の header と骨格を source に、run-unit・check-skeleton・試験の土台 fixture.h、BASE | O | planned | p002 |
| [ws329-p004](phase004/phase.md) | vcodec-bitwriter、vcodec-nalwriter | S | planned | p003 |
| [ws329-p005](phase005/phase.md) | vcodec-cabac-enc | S | planned | p004 |
| [ws329-p006](phase006/phase.md) | h264enc-ps（SPS・PPS・VUI・level の選び） | S | planned | p004 |
| [ws329-p007](phase007/phase.md) | h264enc-slice（slice header・cabac_zero_word・filler） | S | planned | p004、p006 |
| [ws329-p008](phase008/phase.md) | h264enc-quant（前向きの変換と量子化） | S | planned | p003 |
| [ws329-p009](phase009/phase.md) | h264enc-cavlc | S | planned | p004 |
| [ws329-p010](phase010/phase.md) | h264enc-cabac-se（MB の構文要素） | S | planned | p005 |
| [ws329-p011](phase011/phase.md) | h264enc-cabac-res（residual） | S | planned | p010 |
| [ws329-p012](phase012/phase.md) | h264enc-cost、h264enc-source | S | planned | p003 |
| [ws329-p013](phase013/phase.md) | h264enc-refplanes（1/2 画素の面） | S | planned | p003 |
| [ws329-p014](phase014/phase.md) | h264enc-me（動きの探索） | S | planned | p012、p013 |
| [ws329-p015](phase015/phase.md) | h264enc-intra（intra の選び） | S | planned | p008、p012 |
| [ws329-p016](phase016/phase.md) | h264enc-mbwrite（slice_data・macroblock_layer の書き） | S | planned | p007、p009、p011 |
| [ws329-p017](phase017/phase.md) | h264enc-mb（MB の符号化、2 回の再構成） | O | planned | p008、p015、p016 |
| [ws329-p018](phase018/phase.md) | h264enc-mode（preset、I・P の決定） | S | planned | p014、p017 |
| [ws329-p019](phase019/phase.md) | h264enc-mode-b（B の決定） | S | planned | p018 |
| [ws329-p020](phase020/phase.md) | h264enc-ratecontrol | S | planned | p003 |
| [ws329-p021](phase021/phase.md) | h264enc-gop | S | planned | p003 |
| [ws329-p022](phase022/phase.md) | h264enc-encoder の全体、host の道具と roundtrip、最初の全体の試験 | O | planned | p004〜p021 |
| [ws329-p023](phase023/phase.md) | 品質と適合の行列（全組、BD-rate） | O | planned | p022 |
| [ws329-p024](phase024/phase.md) | libmedia の backend（WS318 の encoder の ops） | O | planned | p022、WS318 p012a |
| [ws329-p025](phase025/phase.md) | 速さ（近道、slice の thread、実機の計測） | O | planned | p023、WS318 p008 |
| [ws329-p026](phase026/phase.md) | 規約の全文の見直し | D | planned | p023〜p025 |

## Resume point

- 2026-10-30 Q1: 作成。WS326 の completed の後に p001。

- 2026-10-31 Q1（WS318 の設計からの引き継ぎ）: 依存に WS318 p012a（encoder の口）を足す。p001 の設計は WS326 の完了を待たずに q1758（P5）で先に進める。
- 2026-10-31 P5（q1758-i01）: p001 の設計を branch agent/p5-ws329 に（design.md・proposed/ の header 26 個（shared 3 を含む、cc と clang の c89 pedantic で単独 compile を確かめた）・user-questions.md（Q329-1〜5）・phase002〜026 の phase.md・design-review.md）。design-reviewer の 29 件（blocker 0・major 10・minor 19）を反映。source は変えていない。次: Q1 が受け入れて merge、Q329-1〜5 の回答、WS326 p026 に `vcodec-` の名前を渡す（design §14）。WS326 completed の後に p002。
- 2026-10-31 Q1（WS326 p026・WS330 の設計からの引き継ぎ）: header の基点は 92ca2d0e8 に移った。p003 で `h264sw_bits` → `vcodec_bitreader`、表 → `vcodec_cabac_*`、`h264sw_cabac_pcm` など → `vcodec_cabac_*(&c.engine)`、`h264sw_nal_find` → `vcodec_nal_find` に置き換える。comment の「H.265 9.3.4.3」は「9.3.5」。frame・DPB の共通化は WS330 p002 で WS329 の試験も前後一致で確かめる。
