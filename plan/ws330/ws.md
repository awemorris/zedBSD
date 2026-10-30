<!-- awesome-plan project=zedbsd record=ws330 -->

# WS330 libmedia の H.265 の software の decoder と encoder（独自の実装、標準廉価実装手順）

- Master: [plan/master.md](../master.md)（2026-10-30 作成）。Primary Milestone: MG006。
- Status: planning（2026-10-30 Q1 が作成。実装は [WS329](../ws329/ws.md)（H.264 の software の encoder）の completed の後。2026-10-31 p001 の設計を P5 が先に進めた）
- 目標: libmedia に、既存の library を使わない H.265（HEVC）の software の decoder と encoder を作る。手順は標準廉価実装手順（[docs/agent/spec-module-delegation.md](../../docs/agent/spec-module-delegation.md)）。
  libmedia の backend の 1 つで、順は Vulkan Video → この software の実装 → libavcodec（[WS326](../ws326/ws.md) の決定に合わせる、Q1 の解釈）。H.265 の実装はユーザーが許可済み（2026-10-30「H.265のパテントプールへの支払いは行う方針です。H.265の実装を行っていいです。」）。

## ユーザーの決定（2026-10-30、原文）

「H.265のエンコーダ、デコーダをlibmediaに追加するWSを1つ立てます。実施時期はH.264エンコーダが完了してからです。H.264エンコーダと同様に、共通部分を洗い出してから、設計を行い、そのあとでSonnetに委譲する計画設計にしてくだしあ。」

- 2026-10-31: ユーザーから、不在の間（昼頃まで）は設計だけ進められる WS の設計を進め、libmedia の WS の優先度を上げる、という趣旨の指示を受けた（Q1 経由）。これにより p001（設計）を WS329 の completed の前に、WS326 の header と WS329 の設計を入力にして行った。実装の Phase は WS329 の completed の後のまま。

## 進め方（上の原文の解釈、Q1）

- p001（設計、Opus 5.5）:
  1. H.264 の decoder・encoder（[WS326](../ws326/ws.md)・[WS329](../ws329/ws.md)、WS326 p026 で codec に依らない名前にした module）から、H.265 で使える物を洗い出す（例: bit の読み書き・Exp-Golomb、NAL・RBSP、CABAC の算術の engine、frame・plane、DPB・参照の管理の共通の部分、動きの探索・rate control の枠、試験の道具）。
  2. 足りない共通化の設計（H.264 の既存の試験を壊さない移し方。前後で H.264 の単体試験と適合の試験が同じであることで確かめる）。
  3. H.265 に固有の module（VPS・SPS・PPS・slice segment header、CTU・CU・PU・TU の四分木、intra の 35 mode、変換 4〜32 と transform skip、8・4 tap の補間、merge・AMVP、deblocking、SAO、CABAC の文脈と構文要素、encoder の mode・分割の選び）の header（interface と仕様の comment）と骨格、試験の道（適合の bitstream と hash の照合、decoder と encoder の往復）、実装の Phase の表と各 phase.md。
  4. design-reviewer。
- p002 以降（実装、Sonnet 5.5 high）: 標準廉価実装手順のとおり。decoder を先、encoder を後。
- 範囲（案、p001 で詰める）: Main profile（8 bit 4:2:0 progressive）の decode と encode。Main 10・tile・WPP などは p001 で範囲を示しユーザーに聞く。

## Phase

設計は [design.md](design.md)（決定・H.264 との共通化・module・試験の道・Phase の割り振り）、各 module の仕様は [proposed/](proposed/) の header の comment（p003 で source に置き、その commit を実装の Phase の基点 BASE にする）。ユーザーの判断を待つ点は [user-questions.md](user-questions.md)（Q330-1〜4）、敵対的レビューは [design-review.md](design-review.md)。
型: O = Opus、S = Sonnet 5.5 high（標準廉価実装手順、1 回の起動）、D = conformance-reviewer。p034〜p048 の phase.md は p033 が書く（design D19）。

| Phase | 目的 | 型 | 状態 | 依存 |
| --- | --- | --- | --- | --- |
| ws330-p001 | 設計（Opus 5.5）: H.264 との共通部分の洗い出しと共通化の設計、H.265 の decoder の module・header・試験の道・実装の Phase の表、encoder の地図と公開の header | O | in-progress（q1763 P5。設計と review の反映の後、Q1 の受け入れと Q330-1〜4 の回答待ち） | — |
| [ws330-p002](phase002/phase.md) | decoder 側の共通化（vcodec.h・vcodec-frame・vcodec-output、WS326 p026 の残り） | O | planned | WS329 completed、WS326 p026 |
| [ws330-p003](phase003/phase.md) | decoder の header と骨格を source に、試験の道具、BASE | O | planned | p002 |
| [ws330-p004](phase004/phase.md) | h265sw-nal、h265sw-sei | S | planned | p003 |
| [ws330-p005](phase005/phase.md) | h265sw-scan、h265sw-pic | S | planned | p003 |
| [ws330-p006](phase006/phase.md) | h265sw-scaling | S | planned | p005 |
| [ws330-p007](phase007/phase.md) | h265sw-rps | S | planned | p003 |
| [ws330-p008](phase008/phase.md) | h265sw-vui | S | planned | p003 |
| [ws330-p009](phase009/phase.md) | h265sw-ps | S | planned | p006、p007、p008 |
| [ws330-p010](phase010/phase.md) | h265sw-slice | S | planned | p004、p009 |
| [ws330-p011](phase011/phase.md) | h265sw-poc | S | planned | p010 |
| [ws330-p012](phase012/phase.md) | h265sw-dpb | S | planned | p011、p002 |
| [ws330-p013](phase013/phase.md) | h265sw-reflist | S | planned | p012 |
| [ws330-p014](phase014/phase.md) | h265sw-cabac-init | S | planned | p003 |
| [ws330-p015](phase015/phase.md) | h265sw-cabac-se | S | planned | p014 |
| [ws330-p016](phase016/phase.md) | h265sw-residual | S | planned | p015、p005 |
| [ws330-p017](phase017/phase.md) | h265sw-intra | S | planned | p003 |
| [ws330-p018](phase018/phase.md) | h265sw-transform | S | planned | p003 |
| [ws330-p019](phase019/phase.md) | h265sw-interp（補間と重み） | S | planned | p003 |
| [ws330-p020](phase020/phase.md) | h265sw-tmvp | S | planned | p005、p002 |
| [ws330-p021](phase021/phase.md) | h265sw-merge | S | planned | p020 |
| [ws330-p022](phase022/phase.md) | h265sw-amvp | S | planned | p020 |
| [ws330-p023](phase023/phase.md) | h265sw-ctu（CTU の syntax） | S | planned | p009、p010、p015、p016、p017 |
| [ws330-p024](phase024/phase.md) | h265sw-recon（CU の再構成） | S | planned | p013、p018、p019、p021、p022、p023 |
| [ws330-p025](phase025/phase.md) | h265sw-deblock | S | planned | p005 |
| [ws330-p026](phase026/phase.md) | h265sw-sao | S | planned | p005 |
| [ws330-p027](phase027/phase.md) | h265sw-slicedata（WPP・tile・dependent） | S | planned | p023、p024 |
| [ws330-p028](phase028/phase.md) | h265sw-decoder の全体、host の道具、最初の全体の試験 | O | planned | p004〜p027 |
| [ws330-p029](phase029/phase.md) | 適合の試験（HEVC_v1） | O | planned | p028 |
| [ws330-p030](phase030/phase.md) | libmedia の backend（共通の backend の核） | O | planned | p029、WS318 completed |
| [ws330-p031](phase031/phase.md) | decoder の速さ | O | planned | p029、p030 |
| [ws330-p032](phase032/phase.md) | encoder 側の共通化（WS329 の encoder を移す） | O | planned | p029、WS329 completed |
| [ws330-p033](phase033/phase.md) | encoder の header の設計・骨格・p034〜p048 の phase.md・design-reviewer | O | planned | p032 |
| ws330-p034 | h265enc-ps | S | planning（p033 で phase.md） | p033 |
| ws330-p035 | h265enc-slice | S | planning | p034 |
| ws330-p036 | h265enc-cabac | S | planning | p033 |
| ws330-p037 | h265enc-residual | S | planning | p036 |
| ws330-p038 | h265enc-quant | S | planning | p033 |
| ws330-p039 | h265enc-refplanes | S | planning | p033 |
| ws330-p040 | h265enc-intra | S | planning | p038 |
| ws330-p041 | h265enc-ctuwrite | S | planning | p035、p037 |
| ws330-p042 | h265enc-cu | O | planning | p040、p041 |
| ws330-p043 | h265enc-mode | S | planning | p039、p042 |
| ws330-p044 | h265enc-sao、h265enc-gop | S | planning | p042 |
| ws330-p045 | h265enc-encoder の全体、roundtrip | O | planning | p034〜p044 |
| ws330-p046 | 品質と適合の行列 | O | planning | p045 |
| ws330-p047 | libmedia の encoder の backend | O | planning | p045、WS318 p012a |
| ws330-p048 | encoder の速さ（WPP の thread） | O | planning | p046、p047 |
| ws330-p049 | 規約の全文の見直し | D | planning | p031、p046〜p048 |

## Resume point

- 2026-10-30 Q1: 作成。WS329 の completed の後に p001。
- 2026-10-31 P5（q1763-i01）: p001 の設計を branch agent/p5-ws330 に（design.md・proposed/ の header 36 個（common 6 を含む、cc と clang の c89 pedantic で単独 compile を確かめた）・user-questions.md（Q330-1〜4）・phase002〜033 の phase.md・tests/conformance の list・fetch.sh・sha256・design-review.md）。source は変えていない。次: Q1 が受け入れて merge、Q330-1〜4 の回答、design §14 の Q1 への注意（仕様の PDF の写し、WS326 p026 と WS329 p003 への入力、WS318 の記録）。WS329 completed の後に p002。
