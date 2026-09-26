<!-- awesome-plan project=zedbsd record=ws068p001 -->

# ws068-p001: 設計（library の構成、EGL、GLES の方式の比較）

Phase ID: `ws068-p001`
Parent: [WS068](../ws.md)
Status: cleared（q470-i01、2026-09-26。GLES の方式の選択はユーザーの判断として残す）
Phase disposition: normal
Queue: q470-i01
承認: 2026-09-26 ユーザーの自律実行の指示「ホーム画面、EGL/GLES、WaylandコンポジタのX11機能など、デスクトップ関連の作業を優先しつつ、幅広く残っている作業を実施してください。…私が止めるまで自走を続けてほしい」

## 成果

[design.md](../design.md): library の構成（`libEGL`・`libGLESv2`・`libwayland-egl`、非公開の interface、Khronos の header の出典）、
EGL の設計（platform: Wayland・display 直接・surfaceless、config、window surface と swapchain、pbuffer、context、error）、
GLES の方式の比較（A 自前 compiler、B glslang、C Zink、D ANGLE）とエージェントの推奨（B）、試験、GLX との関係、libvulkan への要求。

## 残る判断（ユーザー）

- **GLES の方式（A・B・C・D）**。p003（GLES 2.0 の最小）の前提。推奨は B（外部の C++ の package として glslang を取り込む）。
- GLX（desktop GL）の方式は X11 の WS で決める（§6）。

EGL（p002）は方式によらず要るので、この設計のまま進める。
