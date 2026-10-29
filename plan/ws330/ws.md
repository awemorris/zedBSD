<!-- awesome-plan project=zedbsd record=ws330 -->

# WS330 libmedia の H.265 の software の decoder と encoder（独自の実装、標準廉価実装手順）

- Master: [plan/master.md](../master.md)（2026-10-30 作成）。Primary Milestone: MG006。
- Status: planning（2026-10-30 Q1 が作成。[WS329](../ws329/ws.md)（H.264 の software の encoder）の completed の後に始める）
- 目標: libmedia に、既存の library を使わない H.265（HEVC）の software の decoder と encoder を作る。手順は標準廉価実装手順（[docs/agent/spec-module-delegation.md](../../docs/agent/spec-module-delegation.md)）。
  libmedia の backend の 1 つで、順は Vulkan Video → この software の実装 → libavcodec（[WS326](../ws326/ws.md) の決定に合わせる、Q1 の解釈）。H.265 の実装はユーザーが許可済み（2026-10-30「H.265のパテントプールへの支払いは行う方針です。H.265の実装を行っていいです。」）。

## ユーザーの決定（2026-10-30、原文）

「H.265のエンコーダ、デコーダをlibmediaに追加するWSを1つ立てます。実施時期はH.264エンコーダが完了してからです。H.264エンコーダと同様に、共通部分を洗い出してから、設計を行い、そのあとでSonnetに委譲する計画設計にしてくだしあ。」

## 進め方（上の原文の解釈、Q1）

- p001（設計、Opus 5.5）:
  1. H.264 の decoder・encoder（[WS326](../ws326/ws.md)・[WS329](../ws329/ws.md)、WS326 p026 で codec に依らない名前にした module）から、H.265 で使える物を洗い出す（例: bit の読み書き・Exp-Golomb、NAL・RBSP、CABAC の算術の engine、frame・plane、DPB・参照の管理の共通の部分、動きの探索・rate control の枠、試験の道具）。
  2. 足りない共通化の設計（H.264 の既存の試験を壊さない移し方。前後で H.264 の単体試験と適合の試験が同じであることで確かめる）。
  3. H.265 に固有の module（VPS・SPS・PPS・slice segment header、CTU・CU・PU・TU の四分木、intra の 35 mode、変換 4〜32 と transform skip、8・4 tap の補間、merge・AMVP、deblocking、SAO、CABAC の文脈と構文要素、encoder の mode・分割の選び）の header（interface と仕様の comment）と骨格、試験の道（適合の bitstream と hash の照合、decoder と encoder の往復）、実装の Phase の表と各 phase.md。
  4. design-reviewer。
- p002 以降（実装、Sonnet 5.5 high）: 標準廉価実装手順のとおり。decoder を先、encoder を後。
- 範囲（案、p001 で詰める）: Main profile（8 bit 4:2:0 progressive）の decode と encode。Main 10・tile・WPP などは p001 で範囲を示しユーザーに聞く。

## Phase

| Phase | 目的 | 状態 | 依存 |
| --- | --- | --- | --- |
| ws330-p001 | 設計（Opus 5.5）: H.264 との共通部分の洗い出しと共通化の設計、H.265 の decoder・encoder の module・header・骨格・試験の道・実装の Phase の表 | planning | WS329 completed |

## Resume point

- 2026-10-30 Q1: 作成。WS329 の completed の後に p001。
