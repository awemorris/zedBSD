<!-- awesome-plan project=zedbsd record=ws334-p003 -->

# ws334-p003 設計 B: libkeiland の Win32 と apps の module（標準廉価実装手順）

- 親: [WS334](../ws.md)、設計 [design.md](../design.md) の §4・§6.4・§6.7・§6.8・§8（T1〜T3）。
- 状態: planned（2026-11-01 p002 で作成）。
- 依存: p002。

## 目的

libkeiland の Win32 の backend と `apps/browser-win32/` のうち、header で約束が決まり host か Wine で単体試験できる物を module にし（鍵の表・custom frame の幾何・UTF-16 と入力の写し・CF_HTML・platform-win32・ICO）、header・.c の骨格・試験の道具の追加・実装の phase.md（p014〜p019）と、統合の Phase（p028・p029・p031〜p035）の phase.md を書く。仕様の file（scan code の表・HID・evdev の値・CF_HTML・ICO）を取得して SHA-256 を記録し、置き場を Q1 に頼む。
