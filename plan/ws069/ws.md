<!-- awesome-plan project=zedbsd record=ws069 -->

# WS069: Wayland デスクトップ（zwl）で X11 の app を動かす（Xzed の Wayland backend、GLX）

<!-- awesome-plan-current:start -->
Status: incomplete
Primary Milestone: MG006
Related Milestones: MG002
Objectives: O1, O2
Parent: [Master](../master.md)
Queue: none
Resume point: p007（BUG-057 の段の特定）、次に p006（i915 実機、規約と回帰）
<!-- awesome-plan-current:end -->

## 目標

2026-09-26 ユーザー: 「ホーム画面、EGL/GLES、WaylandコンポジタのX11機能など、デスクトップ関連の作業を優先しつつ…」
「WaylandコンポジタのX11サーバ機能については、GLX拡張も実装しておいてください。」

zdesktop（zwl、Wiseman Mode）の上で X11 の app（zterm・zshell 等、外部の Xlib の app）を動かす。Xwayland に当たるものを、
既存の小さな X server（`userland/X11/xzed`、Xzed）の Wayland backend として作る。GLX 拡張も実装する。

## 方式（[design.md](design.md)）

1. rootful: Xzed が 1 つの Wayland の窓（X の screen 全体）を持つ（Xephyr の形）。入力はその窓の wl_pointer・wl_keyboard。
2. rootless: X の top-level の窓それぞれが Wayland の窓（xdg_toplevel）になる（Xwayland の形）。Wiseman が装飾する。
3. GLX: GLX 拡張と client の `libGL`。direct rendering は EGL/GLES（WS068）の上で、描いた画像を X の窓へ渡す（DRI3/Present に当たる）。

## 受け入れ

1. Venus の zdesktop で Xzed の窓に X の app（zterm）が描かれ、keyboard と pointer で操作できる（rootful）。
2. rootless で X の app の窓が Wiseman の窓になる。
3. GLX の試験 app（glxinfo 相当、clear と三角形）が動く（p004）。固定機能の GL 1.x の app（gears）が動く（p005）。
4. i915 実機、規約・回帰。

## 依存

- WS068（EGL/GLES）: GLX（p004）の前提。GLES の方式はユーザーの判断待ち（WS068 design.md §4）。
- Xzed の text は `/dev/graphics` の glyph（レガシー）。Wayland backend では libtruetype の等幅 font を使う（2026-09-26 ユーザー
  「/dev/graphicsのフォントは使わないでください。それはレガシー用です。」）。

## Phase 一覧

| Phase | 内容 | Status | 依存 |
| --- | --- | --- | --- |
| [ws069-p001](phase001/phase.md) | 設計（[design.md](design.md)） | cleared（q472-i01、2026-09-26） | — |
| [ws069-p002](phase002/phase.md) | rootful の Wayland backend（窓・wl_shm・入力・libtruetype の glyph） | cleared（q473-i01、2026-09-26。Venus の zwl で zterm） | p001 |
| [ws069-p003](phase003/phase.md) | rootless（X の top-level ごとの Wayland の窓） | cleared（q474-i01、2026-09-26。Venus で zterm の窓） | p002 |
| [ws069-p004](phase004/phase.md) | GLX の核（Xzed の GLX 拡張、libGL の GLX、pbuffer に描いて X の窓へ） | cleared（q477-i01、2026-09-26。Venus の rootless で glxtest の窓） | p003、WS068-p008・p010 |
| [ws069-p005](phase005/phase.md) | 固定機能の GL 1.x（glBegin/glEnd、行列、光源、display list）と gears | cleared（q478-i01、2026-09-26。Venus で zgears） | p004 |
| [ws069-p007](phase007/phase.md) | i915 実機での GLX の間欠の止まり（BUG-057）の段の特定 | in-progress（q485-i01） | p005、WS068-p006 |
| ws069-p006 | i915 実機、規約の全文との照合と回帰（最後） | planning | 全 Phase（p007 を含む） |
