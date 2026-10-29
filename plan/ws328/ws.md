<!-- awesome-plan project=zedbsd record=ws328 -->

# WS328 libbrowser のベクトル描画（path の塗りと線、CSS の角丸・gradient など）と、それを使った SVG の描画

- Master: [plan/master.md](../master.md)（2026-10-30 作成）。Primary Milestone: MG006。
- Status: planning（2026-10-30 Q1 が作成、担当は未割当。WS074（Browser、Codex のセッション）と file が重なるので、始める前に WS074 と調整する）
- 目標: libbrowser の描画（`userland/desktop/libbrowser/paint/`）にベクトル描画を足し、それで CSS の描画（角丸・gradient・影など）と、libbrowser の中の SVG の描画を作る。既存の library は使わない（libbrowser は単体の library）。

## ユーザーの決定（2026-10-30、原文）

「libbrowserはCSS描画のためのベクトル描画を持っていると思います。持っていないなら、内部に実装するのがいいと思います。このベクトル描画機能を使って、libbrowser内にSVG描画を実装するのがいいと思います。WebP, AVIFは、あとでlibwebp-compatのような実装をしたいと思いますが、H.264デコーダが安いAIモデルで実装できるかの実験結果を待ちたいと思います。」

## 今の状態（Q1 の調べ、2026-10-30）

- 描画の display list（`paint/paint.h`）の種類は PAINT_RECT・PAINT_TEXT・PAINT_CLIP・PAINT_UNCLIP・PAINT_IMAGE だけ。path・曲線・gradient・角丸の描画は無い（comment は「Rounded corners, background images, opacity and shadows come later」）。CPU の参照の描画（software.c）と Vulkan の描画（vulkan.c）の 2 つが同じ display list を描く。
- SVG: DOM・HTML の解析（html/foreign.c）、inline の `<svg>` の大きさ（layout/replaced.c の viewBox の比）、JS の bind（svg-length.c・svg-text.c）はあるが、図形の描画は無い。画像としての SVG（`<img src=*.svg>`）も無い。

## 方針（Q1 の案）

- display list に path の item（move・line・二次と三次の Bézier・arc、塗りの規則 nonzero と evenodd、線の幅・join・cap・dash、変換の行列、単色と線形・放射の gradient、anti-alias）を足し、CPU の参照（coverage の計算）と Vulkan（GPU、例えば stencil して覆う形か compute の coverage）で同じ結果を描く（画素の比べの試験）。
- CSS: border-radius、gradient の background、box-shadow を path の item で。
- SVG: rect・circle・ellipse・line・polyline・polygon・path、fill・stroke、transform、viewBox、g、use、defs の gradient、clip-path の順に。`<img>` の SVG も。
- WebP・AVIF（libwebp-compat の類）は [WS326](../ws326/ws.md)（H.264 の software の decoder を安い model で作れるかの実験）の結果を待つ。

## Phase

| Phase | 目的 | 状態 | 依存 |
| --- | --- | --- | --- |
| ws328-p001 | 設計: path の item と CPU・GPU の描画、CSS と SVG への当て方、試験（WPT の該当の試験と画素の比べ）、WS074 との調整 | planning | — |

## Resume point

- 2026-10-30 Q1: 作成。WS074 と調整してから始める。
