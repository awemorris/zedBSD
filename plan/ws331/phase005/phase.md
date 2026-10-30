<!-- awesome-plan project=zedbsd record=ws331-p005 -->

# ws331-p005: Raspberry Pi の console の作り直しと pc98 の例外の候補

Status: in-progress
Parent: [WS331](../ws.md)
Queue: q1719（P2）

## 目的と範囲

2026-10-30 ユーザーから、kernel_putc を入れた時に Raspberry Pi の console を作り直さなかったのが原因なので、今作り直す、という趣旨の指示を受けた。

1. `src/drivers/platform/rpi4/rpi4-console.c`（Pi 4・Pi 5・ODROID-C5 が使う）を `sun4u-console.c` と同じ形にする: firmware の framebuffer を kern_boot_framebuffer と kern_device_map で map、自分の文字の表と 8x16 の font（pcat の vgafont と同じ物）で描く、serial の build では UART（Pi の PL011、ODROID-C5 の Amlogic UART）の送信も driver で行う、自分の出力の関数を kernel_putc として公開する（hal_putc を呼ばない）。graphics が画面を持つ間は描かず、返された時に描き直し、end の後は描かない。driver は `hal/arm64/bsp-rpi-common/cons.h` を include しない。
2. `src/hal/arm64/watch.c` の stall の報告の描く先を、HVS の状態（plane の有無）だけで決める（HAL の実装の中、hal.h は不変）。使われなくなる `rpi_cons_suspend_output`・`rpi_cons_resume_output`・`rpi_cons_output_suspended` を整理する。
3. p001 §4 の pc98 の 3 か所（pc98-lgy98 を kern_io へ、graphics の start・stop を driver へ、JIS の表を driver へ。git mv は Q1 の OK の後）。

- 2026-10-30 ユーザーから、Pi 4 にも /dev/graphics を作り HAL に依存せず描画し、その上で kernel_putc を登録する、という趣旨の指示を受けた。→ 1 は sun4u の形でなく pcat の形にする: Raspberry Pi の graphics の backend を generic の graphics-device（src/drivers/generic/graphics-device.c）に登録して /dev/graphics を出し、その上の text の console が kern_text で描き kernel_putc を公開する。
- 2026-10-30 ユーザーから次の趣旨の指示を受けた: /dev/graphics は Pi 4 だけでなく Pi 5・VisionFive 2・ODROID-C5 にも作る。手本は保守されていない sun4u でなく PC/AT にする。console は GPU の driver の最初の描画で framebuffer から切り離す PC/AT の方式（kern_text_end、src/drivers/gpu/gpu.c の最初の flip）にし、GPU の最初の描画で firmware の framebuffer を完全に手放す既存の実装（kern_text_end に ranges を渡す形）にも合わせる。

## 確かめ

- Pi 4: QEMU raspi4b の起動（boot-test.sh で login）と、graphics の後に console に戻った時の描き直し（AAT か画面の PNG）。Pi 5・ODROID-C5 は build（warning 0）。pc98 は build。
- grep で driver に `hal/`・`hal_`・`HAL_` が残らない事（build の macro HAL_ARCH_*・HAL_BOARD_* はユーザーの決定待ち）。
