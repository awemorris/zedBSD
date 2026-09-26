<!-- awesome-plan project=zedbsd record=ws035p073 -->

# ws035-p073: `userland/base/zwl` を `userland/base/zdesktop`（`/bin/zdesktop`）へ改名

Phase ID: `ws035-p073`
Parent: [WS035](../ws.md)
Status: in-progress（q486-i01）
Phase disposition: normal
Queue: q486-i01
承認: 2026-09-27 ユーザー「userland/base/zwlをuserland/base/zdesktopにリネームし、以後はzdesktopと呼んでください。」、
範囲はユーザーの回答「外側だけ」（C の識別子と log の接頭辞は変えない）

## 範囲

1. `git mv userland/base/zwl userland/base/zdesktop`。program は `/bin/zdesktop`（Makefile の target、package の名 `zdesktop`）。
2. 参照の更新: build の config（`platform/*`、`config/`、`plan/*/tests/config-*.mk`）、`zdesktop-x11`、App Home の中の自分の名、
   他の program の source の注釈と文字列（mview・wltest・zdesktop-terminal・libwayland 等）、現行の試験と道具
   （`plan/ws014/tests`・`plan/ws031/tests`・`plan/ws035/tests`・`plan/ws068/tests`・`plan/ws069/tests`・`plan/ws035/demo`・`plan/tools`）の
   起動の path と process の名（`zwl` → `zdesktop`。`ps` の一致は `zdesktop-terminal`・`zdesktop-x11` と混ざらないよう完全一致）、
   現行の文書（master の Tools、ws035・ws068・ws069 の ws.md と design）。
3. 変えないもの: C の識別子（`zwl_*`、`struct zwl_server` 等）、log の接頭辞（`ZWL ...`）、完了した Phase の記録と `plan/history/`。

## 受け入れ

1. build（`plan/ws035/tests/build-zdesktop-image.sh`）が warning 0 で `/bin/zdesktop` を入れ、`/bin/zwl` が無い。現行の source・試験・
   道具に `zwl` の path・process の名の参照が残らない（`git grep` で確認。C の識別子と log の接頭辞は除く）。
2. Venus の回帰: zdesktop-p070（App Home と X11）、x11-p005、egl-p008、zdesktop-p059 か p064 の 1 つ。
3. i915 実機の run（`CAPTURE=zdesktop-x11 ZDESKTOP_APP=home`）で zdesktop が起動し desktop が描かれる。boot test。
