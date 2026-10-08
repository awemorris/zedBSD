<!-- awesome-plan project=zedbsd record=ws190 -->

# WS190: 文字の欄と Text Editor の指での選択（ダブルタップ・端の drag・コピーの popup）

<!-- awesome-plan-current:start -->
Status: incomplete
Primary Milestone: MG006
Objectives: O2
Parent: [Master](../master.md)
Focused goal: fg019（ベータ2）
Queue: q899（P1、2026-10-08 午後、WS189 の drag の後）
<!-- awesome-plan-current:end -->

## 由来（2026-10-08 ユーザー）

「1行および複数行のテキストフィールドコンポーネントでは、テキストをダブルタップすると、選択モードになり、開始端、終了端をドラッグ操作で決定でき、指を離すと、コピーと切り取りのボタンがポップアップするといいなあ。別実装になるけど、テキストエディタも。今実装しているドラッグ処理のあとに実装しよう。」

## 決定（2026-10-08 ユーザー、クリックの回答）

- popup のボタンは **コピー・切り取り・貼り付け・すべて選択**（選択が無ければコピー・切り取りを出さない、clipboard が空なら貼り付けを出さない、読み取り専用の欄では切り取り・貼り付けを出さない、秘密の欄（パスワード）ではコピー・切り取りを出さない、は Q1 の推しとして設計で確かめる）。

## 既にある物（2026-10-08 Q1 の確認）

- libkeiland の `kl_text_touch`（`kl_text_touch_tap(…, twice)`、`kl_text_touch_drag_begin/drag/end`、`kl_text_touch_draw_handles`、`KL_TEXT_TOUCH_MENU`）。Text Editor（textedit/app.c・main.c）が使っている。libkeiland の欄（kl_field・text area）はまだ使っていない。
- libkeiland の欄の clipboard・undo（KL_VERSION 67、ws177-p013）。

## 範囲

- libkeiland の 1 行の欄（kl_field）と複数行の欄（text area）: ダブルタップで語を選んで選択の mode、開始端・終了端の handle の drag、指を離すと popup。
- Text Editor: 同じ操作（別の実装、kl_text_touch の今の使い方に popup を足す）。
- 試験: host の試験と、AAT（aat-input の touch で）。

## Phase（案）

| Phase | 目的 | Status | 依存 |
| --- | --- | --- | --- |
| [p001](phase001/phase.md) | 設計: kl_field・text area への kl_text_touch の組み込み、popup の部品（位置・画面の端・画面 keyboard との重なり）、Text Editor の popup | in-progress（初版、design-reviewer） | WS189 |
| p002 | libkeiland の欄 | planned | p001 |
| p003 | Text Editor | planned | p001 |
