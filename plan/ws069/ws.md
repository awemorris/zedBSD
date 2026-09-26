<!-- awesome-plan project=zedbsd record=ws069 -->

# WS069: Wayland デスクトップ（zwl）で X11 の app を動かす（Xzed の Wayland backend、GLX）

<!-- awesome-plan-current:start -->
Status: incomplete
Primary Milestone: MG006
Related Milestones: MG002
Objectives: O1, O2
Parent: [Master](../master.md)
Queue: q472
Resume point: p002（rootful の Wayland backend）
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
3. GLX の試験 app（glxinfo 相当、clear と三角形）が動く（WS068 の GLES の後）。
4. i915 実機、規約・回帰。

## 依存

- WS068（EGL/GLES）: GLX（p004）の前提。GLES の方式はユーザーの判断待ち（WS068 design.md §4）。
- Xzed の text は `/dev/graphics` の glyph（レガシー）。Wayland backend では libtruetype の等幅 font を使う（2026-09-26 ユーザー
  「/dev/graphicsのフォントは使わないでください。それはレガシー用です。」）。

## Phase 一覧

| Phase | 内容 | Status | 依存 |
| --- | --- | --- | --- |
| [ws069-p001](phase001/phase.md) | 設計（[design.md](design.md)） | cleared（q472-i01、2026-09-26） | — |
| [ws069-p002](phase002/phase.md) | rootful の Wayland backend（窓・wl_shm・入力・libtruetype の glyph） | in-progress（q473-i01） | p001 |
| ws069-p003 | rootless（X の top-level ごとの Wayland の窓） | planning | p002 |
| ws069-p004 | GLX 拡張と libGL（EGL/GLES の上） | planning | p003、WS068-p003 以降 |
| ws069-p005 | i915 実機、規約の全文との照合と回帰（最後） | planning | 全 Phase |
