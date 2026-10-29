<!-- awesome-plan project=zedbsd record=ws329 -->

# WS329 libmedia の H.264 の software の encoder（独自の実装、標準廉価実装手順）

- Master: [plan/master.md](../master.md)（2026-10-30 作成）。Primary Milestone: MG006。
- Status: planning（2026-10-30 Q1 が作成。[WS326](../ws326/ws.md)（H.264 の software の decoder）の completed の後に始める）
- 目標: libmedia に、既存の library を使わない H.264 の software の encoder を作る。手順は標準廉価実装手順（[docs/agent/spec-module-delegation.md](../../docs/agent/spec-module-delegation.md)）。
  libmedia の encoder の口（[WS318](../ws318/ws.md) の `media_encoder_*`、codec の問い合わせと指定）の backend の 1 つで、順は Vulkan Video の encode → この encoder → libavcodec（[WS326](../ws326/ws.md) の decoder の順の決定に合わせる、Q1 の解釈）。

## ユーザーの決定（2026-10-30、原文）

「標準廉価実装手順で、libmediaに、H.264エンコーダを追加するWSをそれぞれ作ります。H.264デコーダの実装のうち再利用できるところを洗い出して、デコード専用ではなく共通関数にするのを、Opus 5.5が設計段階で行います。そのあと、必要な追加実装を設計済みにしてSonnetに委譲します。H.264デコーダの実装が完了してから取り組みます。」

## 進め方（上の原文の解釈、Q1）

- p001（設計、Opus 5.5）:
  1. WS326 の decoder の module（h264sw-*）から、encoder でも使える物を洗い出す（例: bit の読み書き、NAL と RBSP の組み立て、SPS・PPS・slice header の構造体、表、変換と逆変換・量子化、intra の予測、動きの補間、deblocking、DPB と参照の list、CAVLC・CABAC の表と文脈）。
  2. それらを decode 専用でない共通の関数・module にする設計（名前・header の分け方、decoder の既存の試験を壊さない移し方）。共通化の refactor 自体は p001 の設計に沿って Phase として行い、decoder の単体試験と適合の試験が前後で同じであることで確かめる。
  3. encoder に要る追加の module（bit の書き、CAVLC・CABAC の符号化、mode の選び（intra・inter）、動きの探索、rate control、frame の種類と参照の構造、出力の NAL）の header（interface と仕様の comment）と骨格、試験の道（decoder との往復、適合の確かめ）、実装の Phase の表と各 phase.md。
  4. design-reviewer。
- p002 以降（実装、Sonnet 5.5 high）: 標準廉価実装手順のとおり。
- 範囲（案、p001 で詰める）: decoder と同じ 8 bit 4:2:0 progressive、Constrained Baseline・Main・High の encode。速さ・画質の目標は p001 で示す。

## Phase

| Phase | 目的 | 状態 | 依存 |
| --- | --- | --- | --- |
| ws329-p001 | 設計（Opus 5.5）: decoder との共通化の洗い出しと設計、encoder の module・header・骨格・試験の道・実装の Phase の表 | planning | WS326 completed |

## Resume point

- 2026-10-30 Q1: 作成。WS326 の completed の後に p001。
