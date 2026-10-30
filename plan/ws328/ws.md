<!-- awesome-plan project=zedbsd record=ws328 -->

# WS328 libbrowser のベクトル描画（path の塗りと線、CSS の角丸・gradient など）と、それを使った SVG の描画

- Master: [plan/master.md](../master.md)（2026-10-30 作成）。Primary Milestone: MG006。
- Status: planning（2026-10-30 Q1 が作成、担当は未割当。WS074（Browser、Codex のセッション）と file が重なるので、始める前に WS074 と調整する）
- 目標: libbrowser の描画（`userland/desktop/libbrowser/paint/`）にベクトル描画を足し、それで CSS の描画（角丸・gradient・影など）と、libbrowser の中の SVG の描画を作る。既存の library は使わない（libbrowser は単体の library）。

## ユーザーの決定（2026-10-30、原文）

「libbrowserはCSS描画のためのベクトル描画を持っていると思います。持っていないなら、内部に実装するのがいいと思います。このベクトル描画機能を使って、libbrowser内にSVG描画を実装するのがいいと思います。WebP, AVIFは、あとでlibwebp-compatのような実装をしたいと思いますが、H.264デコーダが安いAIモデルで実装できるかの実験結果を待ちたいと思います。」

- 2026-10-30 ユーザー「WS328は、Vulkanで描画する実装にします。」→ path の塗り・線・gradient などの描画は Vulkan（GPU）で行う（CPU の参照の描画との関係は設計で示す）。

## 今の状態（Q1 の調べ、2026-10-30）

- 描画の display list（`paint/paint.h`）の種類は PAINT_RECT・PAINT_TEXT・PAINT_CLIP・PAINT_UNCLIP・PAINT_IMAGE だけ。path・曲線・gradient・角丸の描画は無い（comment は「Rounded corners, background images, opacity and shadows come later」）。CPU の参照の描画（software.c）と Vulkan の描画（vulkan.c）の 2 つが同じ display list を描く。
- SVG: DOM・HTML の解析（html/foreign.c）、inline の `<svg>` の大きさ（layout/replaced.c の viewBox の比）、JS の bind（svg-length.c・svg-text.c）はあるが、図形の描画は無い。画像としての SVG（`<img src=*.svg>`）も無い。

## 方針（Q1 の案）

- display list に path の item（move・line・二次と三次の Bézier・arc、塗りの規則 nonzero と evenodd、線の幅・join・cap・dash、変換の行列、単色と線形・放射の gradient、anti-alias）を足し、CPU の参照（coverage の計算）と Vulkan（GPU、例えば stencil して覆う形か compute の coverage）で同じ結果を描く（画素の比べの試験）。
- CSS: border-radius、gradient の background、box-shadow を path の item で。
- SVG: rect・circle・ellipse・line・polyline・polygon・path、fill・stroke、transform、viewBox、g、use、defs の gradient、clip-path の順に。`<img>` の SVG も。
- WebP・AVIF（libwebp-compat の類）は [WS326](../ws326/ws.md)（H.264 の software の decoder を安い model で作れるかの実験）の結果を待つ。

## Phase（2026-10-30 Q1 が標準廉価実装手順の形に）

ユーザーの指示（2026-10-30、原文）「WS328は標準廉価実装手順を適用してphaseを書いてください。」
→ 手順は [docs/agent/spec-module-delegation.md](../../docs/agent/spec-module-delegation.md)。設計の担当は Opus 5.5、実装の担当は Sonnet 5.5 high を Phase ごとに別の worktree で。
基づく標準: SVG 2（W3C）、CSS Backgrounds and Borders 3（border-radius・box-shadow）、CSS Images 3（gradient）、Compositing and Blending 1。WPT の該当の試験を照合に使う。
module と実装の Phase の正確な分け方は p001 が決める（下の p002〜 は Q1 の見込みで、p001 の表で置き換える）。

| Phase | 目的 | 担当 | 状態 | 依存 |
| --- | --- | --- | --- | --- |
| ws328-p001 | 設計（Opus）: WS074 との file の分け方の調整、module の分け方、各 header（定数・構造体・関数と仕様の comment、標準の節）の凍結、.c の骨格、試験の道具（CPU の参照の描画と Vulkan の描画の画素の比べ、独立の参照の rasterizer か WPT の reftest、単体試験の run-unit と check-skeleton）、実装の Phase の表と各 phase.md、design-reviewer | Opus | planned | WS074 と調整 |
| ws328-p002〜 | 実装（Sonnet）、見込み: path の幾何（Bézier・arc の平坦化、変換）／線の展開（join・cap・dash）／塗りの coverage（nonzero・evenodd、CPU の参照）／gradient（線形・放射・conic）／影の blur／Vulkan の path の描画（stencil か compute の coverage）／display list の path の item と software.c・vulkan.c の受け／CSS（border-radius・gradient の background・box-shadow）／SVG の図形と style（rect〜path、fill・stroke、transform、viewBox、g・use・defs）／SVG の gradient と clip-path／`<img>` の SVG | Sonnet | planning | p001 |
| ws328-p0xx | 統合: Browser の上の描画、WPT の該当の試験、QEMU の画面 | Opus | planning | 実装の Phase |
| ws328-p0xx | 規約の全文の見直し | Sonnet | planning | 統合 |

## Resume point

- 2026-10-30 Q1: 作成。WS074 と調整してから始める。

- 2026-10-30 Q1: ユーザーの指示で Phase を標準廉価実装手順の形にした。p001 の設計（Opus）から、着手の時期は未定（WS074 と調整）。
