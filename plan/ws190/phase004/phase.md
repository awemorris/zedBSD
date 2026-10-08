<!-- awesome-plan project=zedbsd record=ws190-p004 -->
# ws190-p004: 規約の全文の見直し（WS190 の C コード）

Status: cleared（2026-10-09 Q1 の判定: P3（Haiku）の 12a2891e6、build と host 試験 PASS。Q1 が text-area.c の誤った注釈 2 つ（Up・Down を「scroll」と書いた）を直した）
Disposition: normal
Parent: [WS190](../ws.md)
Queue: q902（P3、2026-10-09）

## 範囲

WS190 の phase002・phase003 が変えた C コード（libkeiland の text-bar.c・text-select.c・ui.c・field.c・text-area.c・text-input.c、textedit の app.c・main.c・textedit.h）。

## 検証（P3、2026-10-09）

### style-check と修正

- 初期実行: 9 件の blank-after-brace 違反を検出（field.c:787、text-area.c:218・234・581・585、ui.c:676・2920、main.c:633・654）。
  - 全て修正: 閉じ括弧の後に blank line を追加。

- 修正後の 2 次実行: 9 件の paragraph-comment 違反を検出（field.c:788、text-area.c:219・236・584・589、ui.c:677・2922、main.c:634・656）。
  - 全て修正: 各 semantic paragraph に適切な comment を追加。

- 修正後の 3 次実行（style-check 実行）: 違反なし。

### build と試験

- build 対象: `make -j16 ZEDBSD_CONFIG=plan/ws190/tests/config-amd64-aat-touch.mk BUILD=build/p3-ws190 build/p3-ws190/dynamic/libkeiland.so build/p3-ws190/bin/textedit build/p3-ws190/bin/kuidemo`。
  - ビルド成功（既存の成果物から変わらず）、warning 0（clang -Werror）。

- host 試験: `sh plan/ws190/tests/run-host.sh`。
  - host-text-bar: 47/47 PASS（plain・2 回実行で同じ結果）。
  - host-touch-select: 44/44 PASS（plain・2 回実行で同じ結果）。
  - ws190-host: PASS（ASan/UBSan）。
  - 全ての試験が passed。behavior 変更なし。

- `git diff --check`: 違反なし（trailing whitespace なし）。

## 直した規則

- **blank-after-brace** (section 5): 閉じ括弧で semantic paragraph が終わるので、その後に blank line を置く。
  - 修正個所: 9 箇所（field.c、text-area.c、ui.c、main.c）。

- **paragraph-comment** (section 5): 全ての semantic paragraph に前置 comment を置く。一つの sentence で何をするかを述べる。
  - 修正個所: 9 箇所（field.c、text-area.c、ui.c、main.c）。

## 他の WS のコード

phase002・phase003 で変更された他の WS のコード（calendar/main.c、mailer/main.c、phone/main.c、notes/box.c、libkeiland/clipboard.c）は review 対象外（所有者の WS で直す）。

## 結果

- 全ての WS190 own のファイル: style-check pass、build warning 0、host 試験 pass。
- behavior 変更なし。
- clearance criteria 達成。
