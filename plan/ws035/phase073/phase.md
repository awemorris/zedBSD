<!-- awesome-plan project=zedbsd record=ws035p073 -->

# ws035-p073: `userland/base/zwl` を `userland/base/zdesktop`（`/bin/zdesktop`）へ改名

Phase ID: `ws035-p073`
Parent: [WS035](../ws.md)
Status: cleared（q486-i01、2026-09-27）
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

## 結果（2026-09-27、q486-i01）

- `userland/base/zwl` → `userland/base/zdesktop`（git mv）。package と program は `zdesktop`（`/bin/zdesktop`）。usage の名と
  Vulkan の application の名も `zdesktop`。内部の header `zwl.h` は名を変えない（libzdesktop の公開 header `<zdesktop.h>` と混ざるため）。
- build: `platform/amd64/vmunix.mk`（`$(BUILD)/bin/zdesktop`、`DYNAMIC_ZDESKTOP_OBJS`）、`ZEDBSD_USER_PROGRAMS` の `zwl` を
  `zdesktop` に（`config/ci/config-amd64.mk`、現行の試験の config 5 つ）。
- 試験・道具: `/bin/zwl` → `/bin/zdesktop`、実機の rc の service `zwl` → `zdesktop`（file の名も）、`run-zwl.sh` → `run-zdesktop.sh`、
  log `/var/log/zwl.log` → `/var/log/zdesktop.log`、`ps` の一致 `[z]wl` → `[z]desktop( |$)`（`zdesktop-terminal`・`zdesktop-x11` と
  混ざらない）。他の program の注釈、README。
- 変えなかったもの: C の識別子 `zwl_*`、log の接頭辞 `ZWL`、試験の shell 変数（`frames_zwl`）、完了した Phase の記録と履歴、
  以前の文書（WS014 の記録、results、bug ticket の観察）。

## 検証

- build（`plan/ws035/tests/build-zdesktop-image.sh`）warning 0。rootfs に `/bin/zdesktop` があり `/bin/zwl` が無い。
  現行の source・試験・道具に `zwl` の path・process の名の参照が無い（`git grep`、識別子と log の接頭辞を除く）。
  変えた C の file の規約の指摘の数は変わらない（legacy の数のまま）。
- Venus（QEMU）: zdesktop-p070・x11-p005・egl-p008・zdesktop-p064 PASS。
- i915 実機（capture）: zdesktop が起動し desktop・Gears・X terminal・デスクトップ 2 と 1 が PASS
  （`build/ws035-p073-hw/sheet.png`）。gears_turns は FAIL（既知の BUG-057、ws069-p010）。実機の LCD の目視は未実施。
- boot test PASS（`build/ws035-p073-boot/login.png`）。

## 追記（2026-09-27、ws035-p074 で判明）

build の変数の改名（`DYNAMIC_ZWL_OBJS` → `DYNAMIC_ZDESKTOP_OBJS`）が libzdesktop の既存の変数と同じ名になり、libzdesktop.so の link が
compositor の object で行われる誤りを入れていた（p073 の build は libzdesktop.so が最新だったので通っていた）。p074 で
`DYNAMIC_ZDESKTOP_PROGRAM_OBJS` に直した。
