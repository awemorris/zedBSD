<!-- awesome-plan project=zedbsd record=ws044p001 -->

# ws044-p001: rpi4 の console の font を PC/AT の 8x16 へ

Phase ID: `ws044-p001`
Parent: [WS044](../ws.md)
Status: cleared（q377-i01、2026-09-24）
Queue: q377（q377-i01）

## 目的

rpi4 の framebuffer の console は `src/hal/arm64/bsp-rpi4/framebuffer.c` の手書きの 5x7 font（英大文字・数字・少しの記号だけ）で
描いている。これを PC/AT の console と同じ 8x16 の VGA font（`src/drivers/platform/pcat/graphics/vgafont.c`、public domain）の
複製に置き換える。HAL は driver の source を参照しないので、font の table を bsp-rpi4 に複製する（ユーザー指示「複製して使って」）。

## 承認

HAL の差分（bsp-rpi4 への font の追加と、`rpi4_framebuffer_cell` の描画の置換。`include/hal/hal.h` は不変）は、
2026-09-24 にユーザーが承認した（「承認、すぐ進める」）。

## 検証

- rpi4 の kernel と disk image の build（warning 0）。
- QEMU raspi4b で起動し、framebuffer を撮って login prompt が 8x16 の font で読めること。
  `plan/tools/boot-test.py` は PC/AT の font で cell を照合するので、同じ font なら画面の文字を読める。

## 結果（q377-i01、2026-09-24）

- `src/hal/arm64/bsp-rpi4/font.c`・`font.h`（新規）: PC/AT の 8x16 VGA font の複製（table は
  `src/drivers/platform/pcat/graphics/vgafont.c` と byte 単位で同一）。header は `<hal/types.h>` だけを読む。
- `src/hal/arm64/bsp-rpi4/framebuffer.c`: 手書きの 5x7 の table（数字・英大文字・記号の一部）と `glyph_row()` を消し、
  `rpi4_framebuffer_cell()` を font の 16 行を描く形に書き直した（左端の pixel が上位 bit）。
- `platform/arm64/vmunix.mk`: HAL の source に `font.c` を足した。
- `plan/tools/boot-test.py`: 80x25 の grid を画面の中央に置く console（rpi4 の 640x480）を読めるよう、中央寄せの原点も試す。
  `plan/tools/boot-test.sh`: `BOOT_MODE=raspi4b`（QEMU raspi4b に vmunix と DTB を直接渡し、image を SD にする）。

| 検証 | 結果 |
| --- | --- |
| rpi4 の kernel と disk image の build（`make BUILD=build/rpi4-font ZEDBSD_CONFIG=config/ci/config-rpi4.mk disk-image`） | 成功、warning 0 |
| `BOOT_MODE=raspi4b plan/tools/boot-test.sh build/rpi4-font/hdd-image.img` | PASS（画面の login prompt を 8x16 の font で読んだ。[login.png](login.png)） |
| 実機 | 未実施（ユーザーが確認する） |
