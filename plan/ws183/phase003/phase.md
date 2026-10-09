<!-- awesome-plan project=zedbsd record=ws183-p003 -->

# ws183-p003: 規約の全文の見直し（WS183 が変えた C）

Status: planned（2026-10-09 Q1、P4 の Sonnet 5.5 low の試しに割当）
Parent: [WS183](../ws.md)

## 範囲

WS183（p001・p002）が変えた次の file の、WS183 が触った部分を [coding-style.md](../../coding-style.md) の全文と照らし、書き方だけを直す（動作は変えない）。

- src/drivers/gpio/intel-gpio.c
- src/drivers/i2c/i2c-hid.c
- userland/desktop/wayland/touchpad.c
- userland/desktop/wayland/touchpad.h

## 受け入れ

- 違反を一覧にし、直した物と直さない物（理由つき）を記録する。
- 直した後、該当の kernel・userland の build が warning 0、touchpad の host 試験が通る。
- 動作の変更が無い（差分を読んで確かめる）。
