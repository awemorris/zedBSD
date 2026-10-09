<!-- awesome-plan project=zedbsd record=ws200 -->

# WS200: Settings の Users の頁のパスワード変更のウィザードと認証方式の選択

<!-- awesome-plan-current:start -->
Status: planned
Primary Milestone: MG006
Related Milestones: —
Objectives: O2
Parent: [Master](../master.md)
Queue: q922（P1、2026-10-10、WS199 の後）
Target: **ベータ2**（2026-10-10 ユーザー、クリック「ベータ2 に入れる」）
Resume point: p001 から。
<!-- awesome-plan-current:end -->

## 由来（2026-10-10 ユーザーの UAT）

「SettingsのUsersのページには、パスワード変更ボタンをつけて、ウィザード式にする。」
「SettingsのUsersのページには、認証方式のボタンをつけて、Password, PIN, Security Keyにチェックをつけて変更できる。それ以外のログイン方法は、greeter/lock画面では使えなくなる。コンソールのログインではパスワードだけ使える。」

## 依存（2026-10-10 Q1、WS199 review-2 N15）

WS199 i01 の popup の部品（userland/desktop/settings/dialog.c）を使う。account の設定は /etc/passkey の 1 行 `<name>:<uid>:options:methods=..:key-pin=0|1:key-touch=0|1` を WS199 と共有し、各々自分の field だけを変えて書き戻す。

## 目標

- Users の頁に「Change Password」の button、ウィザード（今の password → 新しい password を 2 回 → 完了、処理中は操作できない表示）。
- Users の頁に「Sign-in Methods」の button: Password・PIN・Security Key の checkbox。外した方式は greeter と lock の画面で出さず受けない。console の login・su・sudo・SSH は今どおり password だけ（docs/architecture/security.md の規則）。
- 少なくとも 1 つは残す（全部外せない）。PIN・Security Key は登録が無ければ選べない。
- 選択の保存は sessiond・/sbin/passkey の側（root の持つ設定、/etc/passkey 等）で、Settings は特権を持たない。

## 完了の条件

- QEMU の AAT（password の変更、方式の選択で greeter・lock の選択肢が変わる）、PNG をユーザーに。
- 5330 の UAT。

## Phase

| Phase | 目的 | Status |
| --- | --- | --- |
| p001 | 今の Users の頁・passwd の経路・greeter と lock の方式の選択（ws172-p007）の調べ、設計、実装、host 試験 | planned |
| p002 | T1 の AAT と 5330 の UAT | planning |
