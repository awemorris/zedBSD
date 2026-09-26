<!-- awesome-plan project=zedbsd record=ws069p001 -->

# ws069-p001: 設計

Phase ID: `ws069-p001`
Parent: [WS069](../ws.md)
Status: cleared（q472-i01、2026-09-26）
Phase disposition: normal
Queue: q472-i01
承認: 2026-09-26 ユーザーの自律実行の指示（「WaylandコンポジタのX11機能」「GLX拡張も実装しておいてください」）

成果: [design.md](../design.md)（Xzed の Wayland backend、rootful → rootless → GLX の順、GLX は DRI3/Present に当たる buffer の受け渡し）。
方式の選択はエージェントの技術判断の範囲（既存の Xzed を使う。外部の X server の移植はしない）。GLX の desktop GL の範囲は WS068 の
GLES の方式の判断と合わせて p004 で決める。
