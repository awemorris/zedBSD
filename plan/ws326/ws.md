<!-- awesome-plan project=zedbsd record=ws326 -->

# WS326 libmedia の H.264 の software の decoder（独自の実装）

- Master: [plan/master.md](../master.md)（2026-10-30 作成）。Primary Milestone: MG006。
- Status: incomplete（2026-10-30 Q1 が作成、p001 から）
- 目標: libmedia に、既存の library を使わない H.264 の software の decoder を作る。範囲は progressive の 8 bit 4:2:0、Constrained Baseline・Main・High（interlace（PAFF・MBAFF）・4:2:2・10 bit は範囲の外）。
  Vulkan Video の hardware が無い機械（Pi 5 など）で libavcodec に頼らずに再生する道。libmedia の backend の順は Vulkan Video → この独自の software の decoder → libavcodec（2026-10-30 ユーザー「libmedia の backend の順番は、Vulkan Video --> 独自ソフトウェアdecoder --> libavcodecです。」）。

## ユーザーの決定（2026-10-30、原文）

1. 「H.264デコーダのソフトウェア実装は、どれくらいの規模見積もりになりますか？LWでいうと。」（Q1 の見積もり: 正しく decode できるまで約 60〜80 LW、1080p を CPU で実用の速さにするまで含めて 80〜120 LW）
2. 「libmediaへのH.264デコーダの実装について、Claudeの安いモデルに外注することを検討しています。全体の骨格を、モジュールをわけてファイルを作り、モジュールのインタフェース（定数、構造体、関数）をヘッダで定義しておき、中身はヘッダの関数のコメントで仕様を定義しておいて、モジュールの.cファイルの実装は、実装するLLMに任せる、という構成にしたいです。WSを立てて、phaseを作成し、最初のphaseでこのモジュール設計だけを実施して、実装のフェーズはSonnet 5.5 Highのサブエージェントで実行する、というストラテジにしたいです。」

## 進め方（上の原文の解釈、Q1）

- p001（設計、Opus）: module に分け、各 module の header（定数・構造体・関数）を書き、各関数の comment に仕様（入力・出力・前提・誤りの扱い・ISO/IEC 14496-10 の節）を書く。.c は空の骨格だけ（関数は未実装の印）。module ごとの host の単体試験の仕様と、全体の適合の試験（JVT の conformance の bitstream と hash の照合）の道も決める。module の依存の順と、実装の Phase の割り振り（1 Phase = Sonnet の 1 回の起動で済む大きさ）を表にする。
- p002 以降（実装、Sonnet 5.5 の high の subagent）: header を変えずに .c を実装し、単体試験を通す。header の不備は直さず Q1 に返す（Q1 か Opus が header を直す）。
- 最後に組み込み（libmedia の backend）、適合の試験、速さ、規約の全文の見直し。

## Phase

| Phase | 目的 | 状態 | 依存 |
| --- | --- | --- | --- |
| ws326-p001 | module の設計: file の骨格・header の interface と仕様の comment・単体と適合の試験の仕様・実装の Phase の割り振り（Opus） | in-progress | — |
| ws326-p002〜 | module の実装（Sonnet 5.5 high、p001 の表どおり） | planning | p001 |

## Resume point

- 2026-10-30 Q1: 作成。P1 が q1695 で p001。
