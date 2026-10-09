<!-- awesome-plan project=zedbsd record=ws183-p003 -->

# ws183-p003: 規約の全文の見直し（WS183 が変えた C）

Status: cleared 候補（2026-10-09 P4。見直し・build・host 試験まで済み。Q1 の判定待ち）
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

## 見直しの結果（2026-10-09 P4）

見た範囲: bde49e9c9（p001 の i2c-hid・intel-gpio）、6ff14654c の group_first 周り、c98a659ec（p002 の touchpad.c・.h）の WS183 の行。

### 直した（書き方だけ。動作は変えない）

| file | 場所 | 規則 | 直し |
| --- | --- | --- | --- |
| i2c-hid.c | resource_visitor の IRQ の条件 | 3 clause を 1 行 | 3 行に分け、`return 0` に理由の comment |
| i2c-hid.c | worker の `line` | 名前（error code は意味のある名） | `irq_error` に改名 |
| i2c-hid.c | worker の `kern_logf`（IRQ 経路） | 呼び出しの comment | comment を足した |
| i2c-hid.c | irq_take の trigger・polarity・set_mode | `if` に目的の comment と空行、1 paragraph 1 comment | 3 paragraph に分け、それぞれ comment |
| i2c-hid.c | wait_irq の内側の `while` | loop の直前の comment | 足した。中の comment（「The time to look came.」）が code と合わないので書き直した |
| i2c-hid.c | wait_irq の `any = true;` | paragraph の境界 | 空行と comment |
| i2c-hid.c | wait_irq の `empty++; if (any) empty = 0;` | `if` の comment・空行 | `if (any) { empty = 0; } else { empty++; }`（結果は同じ）に comment |
| touchpad.c | kwl_touchpad_release_all の tap・held の reset | paragraph の comment・空行 | 足した |
| touchpad.c | touch_end の SECOND の reset | comment | 足した |
| touchpad.c | tap_drag_begin の held の reset | 空行・comment | 足した |

### 直さない（違反でない、または WS183 の外）

- intel-gpio.c の WS183 の行（group_first、group_field の `count`・`index` の検査、GROUP_FIELDS_TGL の comment）: 規約に合っている。直しなし。
- intel-gpio.c drv_intel_gpio_pad_find の `if (error == 0) error = group_field(...)` の連なり（src/drivers/gpio/intel-gpio.c 約 305 行）: 以前からの書き方で WS183 の行でない。所有者の WS の見直しで扱う。
- touchpad.c / touchpad.h の p002 の他の行: 規約に合っている。

## 実行した確認

- `make -j16 ZEDBSD_CONFIG=plan/ws035/tests/config-amd64-zdesktop.mk BUILD=build/p4-ws183 vmunix`: rc 0、warning 0（i2c-hid.o を compile し直した）。
- `make -j16 ZEDBSD_CONFIG=plan/ws035/tests/config-amd64-zdesktop.mk BUILD=build/p4-ws183 build/p4-ws183/bin/wayland`: rc 0、warning 0（touchpad.o を compile し直した）。
- `sh plan/ws159/tests/run-host-touchpad.sh build/tmp/p4t/t`: `host-touchpad: ok (32 checks)`。
- `sh plan/ws142/tests/run-host-gesture.sh build/tmp/p4t/g`: `host-gesture: ok (82 checks)`。
- `CC=clang sh plan/ws159/tests/run-host-intel-gpio.sh`: ok（nomode=1 117 checks、tgl 11 checks）。出力の tail だけ見た。
- `python3 plan/tools/style-check.py`（4 file）: 違反 0。`git diff --check`: 0。
- 未実施: i2c-hid の host 試験は無い（p001 のとおり）。keiland-linux の build、QEMU・実機。差分は comment・空行・改名・`if/else` の形だけで、動作は変えていない（`empty` の更新は同じ結果）。
