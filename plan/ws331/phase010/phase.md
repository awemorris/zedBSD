<!-- awesome-plan project=zedbsd record=ws331-p010 -->

# ws331-p010: atomic.h の HAL の分離と、pc98・pcat の legacy の窓の番地

Status: in-progress
Parent: [WS331](../ws.md)
Queue: q1723（P2）

## 目的と範囲（ユーザーの決定の要点は ws.md）

1. `include/hal/atomic.h`: 契約を「macro だけ」にし、その旨を comment に書く。ENTER の port の直列化は、macro の中の statement expression の block で `hal_atomic_enter`・`hal_atomic_leave` を宣言して呼ぶ（file の scope には宣言を出さない）。`hal_atomic_cookie_t` は消し、`uintptr_t` で書く（HAL の 6 つの atomic.c の定義も合わせる。定義と macro の中の宣言の型が食い違わないよう、HAL の中の header で宣言を確かめる）。`hal/arch.h`・`hal/types.h` の 2 行の include を外し、「hal.h の中で include される事を期待し、HAL_ATOMIC_STYLE を与える側が単体で include する使い方も認める」と comment に書く。hal.h の宣言・他の契約は変えない（ユーザーの承認の範囲）。
2. `include/kern/atomic.h`: build の設定（platform の mk の -D）から `HAL_ATOMIC_STYLE` を一時的に定義して `hal/atomic.h` を include し、inline 関数を定義した後に `hal_atomic_*` の macro と `HAL_ATOMIC_STYLE` を undef する。`hal/hal.h` は include しない。
3. pc98・pcat の legacy の VRAM・VGA の窓: driver の中に straight map の基底 0x80000000 を 1 つ定義し、各番地を「基底 + 物理番地」で書く（pcat の font.c、pc98 の text.c・display-glyph.c）。display-gdc.c の基底の無い番地（0xa8000 ほか）も直す。HAL の定数は参照しない。
4. 確かめ: 20 port の build（warning 0、ENTER の port と NATIVE の port の両方）、grep で driver の TU に HAL の宣言が出ない事（kern/atomic.h の前処理の出力で確かめる）、pcat・pc98 の build と pcat の QEMU の起動、amd64・rpi4・m68k の QEMU の起動。
