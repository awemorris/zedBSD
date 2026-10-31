<!-- awesome-plan project=zedbsd record=ws334-p003 -->

# ws334-p003 設計 B: libkeiland の Win32 と apps の module（標準廉価実装手順）

- 親: [WS334](../ws.md)、設計 [design.md](../design.md) の §4・§6.4・§6.7・§6.8・§8（T1〜T3）。
- 状態: cleared（2026-11-01 02:43 Q1 の判定、main 5ea437eb7）
- 依存: p002。

## 目的

libkeiland の Win32 の backend と `apps/browser-win32/` のうち、header で約束が決まり host か Wine で単体試験できる物を module にし（鍵の表・custom frame の幾何・UTF-16 と入力の写し・CF_HTML・platform-win32・ICO）、header・.c の骨格・試験の道具の追加・実装の phase.md（p014〜p019）と、統合の Phase（p028・p029・p031〜p035）の phase.md を書く。仕様の file（scan code の表・HID・evdev の値・CF_HTML・ICO）を取得して SHA-256 を記録し、置き場を Q1 に頼む。

## module の分け（この Phase の分）

| module | header（約束） | .c | Phase | 試験 |
| --- | --- | --- | --- | --- |
| 鍵の表 | `libkeiland/win32/keys-table.h` | `keys-table.c` | p014 | test-keys-table |
| custom frame の幾何 | `libkeiland/win32/frame.h` | `frame.c` | p015 | test-frame |
| platform の Windows | `libkeiland/platform/platform.h`（`struct keiui_wake` に `event`、`keiui_wake_event` を足した） | `platform/platform-win32.c` | p016 | test-platform-win32（win32） |
| UTF-16 と入力の写し | `libkeiland/win32/wide.h`・`input.h` | `wide.c`・`input.c` | p017 | test-wide・test-input |
| CF_HTML | `libkeiland/win32/cf-html.h` | `cf-html.c` | p018 | test-cf-html |
| ICO の writer | `apps/browser-win32/tools/ico.h` | `ico.c` | p019 | test-ico |

- どれも Windows の header を使わない pure な module（platform-win32 を除く）で、host と Wine の両方で試す。
- UI の部分（kl_app・kl_window・window procedure・題の帯の controls・IMM32・clipboard・DPI・TSF など）は module にせず、統合の Phase（p028・p029・p033）に手順と受け入れを書いた（手順の §1: UI は普通の Phase）。

## 見つけた事

- libkeiland の private の `process.h` が CRT の `<process.h>`（winpthreads の `pthread.h` が include する）を隠す。libkeiland の directory を include の path に入れない（libkeiland の file は tree の上からの path で include する）。p028 に書いた。
- libmedia の `bits.h` は mingw の BITS の `bits.h` と名前が同じだが、使う header（windows.h など）は include しないので問題にならない（記録だけ）。

## 経過

- 2026-11-01 02:11 header 7・骨格 7・modules.sh の追加・phase.md（p014〜p019・p028）を commit 99af11a7f。`check-skeleton: PASS`（header 19、source 22、make の 2 つ）。
- 2026-11-01 仕様の file 7 つを Q1 が `build/sources/specs/ws334/` に置いた（SHA-256 一致）。p014 に hid-input.c（GPL-2.0）から値だけを取る約束を書いた。
- 2026-11-01 02:14 phase.md（p029・p031〜p035）。
