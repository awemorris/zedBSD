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

2026-10-30 の範囲の追加（Q1 から、ユーザーの指示の趣旨）:
- ユーザーから、Pi 4 にも /dev/graphics を作り、HAL に依存せずに描き、その上で kernel_putc を登録する、という趣旨の指示を受けた。続いて、/dev/graphics を Pi 5・VisionFive 2・ODROID-C5 にも作り、手本は sun4u でなく PC/AT（sun4u は保守されていない）、console は GPU の driver の最初の描画で framebuffer から切り離される PC/AT の方式にする、という趣旨の指示を受けた。
- ユーザーから、GPU の枠組みの gpu.c も最初の flip で firmware の framebuffer を手放す形（kern_text_end に boot の framebuffer の範囲を渡す）にする、という趣旨の指示を受けた。

## 決めた形

- PC/AT の役割分け（text 層・graphics の backend・kernel_putc は kern_text_kernel_putc・GPU の最初の flip で kern_text_end）を、Pi 4・Pi 5・ODROID-C5・VisionFive 2 で共有する generic の driver にした。pcat・pc98 は自前の /dev/graphics の front end を持つが、新しい 4 board は front end の写しを増やさず generic の `src/drivers/generic/graphics-device.c` を使う（pcat の code は変えていない）。
  - `src/drivers/generic/boot-fb.c`・`include/drivers/generic/boot-fb.h`: board が kern_boot_framebuffer で読んだ画面を kern_device_map で map して描き、/dev/graphics に FIRMWARE の優先度で backend を登録、end で map を外す。arm64・riscv64 の kern_device_map は RAM の範囲に cached の view を返すので、描いた行を kern_dcache_clean_range で掃き出す。HAL が自分の view（stall・fatal の報告）を持ち続けるので、end は画面が ranges にあれば EBUSY。
  - `src/drivers/generic/fb-text.c`・`include/drivers/generic/fb-text.h`: pcat の text.c と同じ形の text 層（属性付きの cell、cursor、spinlock、suspend で描かず resume で消して描き直す、suspend の間の文字は捨てる、reveal_fatal、end）。board は serial の送信と fatal の時の画面の取り戻しを渡す。
  - board の glue: `rpi4-console.c`（Pi 4・Pi 5・ODROID-C5。UART の map・送受信・入力の thread、Pi の pixel の順の値の読み替え、ODROID-C5 は alpha の byte を立てる）、`src/kern/platform/visionfive2.c`（ns16550 の UART を serial の送信に、PowerVR の fatal の取り戻しを reveal_fatal に。vf2_text_end・console_ended は generic の end に置き換えた）。
  - kern/platform の rpi4・rpi5・odroid-c5・visionfive2 の kern_platform_debug_write は kernel_putc の公開後は書かない（書くと二重に出る）。
- HAL（hal.h は不変）: `watch.c` は描く先を HVS の有無と plane の数だけで決める。`bsp-rpi-common/cons.c` の rpi_cons_suspend/resume_output・rpi_cons_output_suspended・output_begin/end・output_suspended を消し、cons.h を git rm（Q1 の OK）。
- gpu.c: 最初の flip で、pcat・rpi4・rpi5・odroidc5・riscv64・sun4u の `.framebuffer` を順に引き、見つかった範囲を kern_text_end に渡す（無ければ NULL・0）。失敗は log して続ける。
- build: Makefile の ZEDBSD_GRAPHICS_BACKEND に rpi4・rpi5・odroid-c5・visionfive2、`platform/arm64/vmunix.mk`・`platform/riscv64/vmunix.mk` に boot-fb・fb-text・vgafont（と graphics-device）、`config/ci/config-{rpi5,odroid-c5,visionfive2}.mk` の CONFIG_DRIVER_GRAPHICS を y。
- pc98: `pc98-lgy98.c` は kern_io_in8/out8/in16/out16、`pc98/graphics/backend.c` の GDC の START・STOP は driver の中の gdc_command（kern_io）。どちらも HAL の include なし。

## 結果（2026-10-30）

- commit: 84e3e2698（Pi、Q1 が main に merge 429fef1d1）、609d97bd5（VF2、cons.h の git rm）、fde6f9ece（gpu.c）、1d950e03c（pc98 の 2 か所）。
- build: `plan/ws331/tests/build-vmunix-all.sh build/p005d` で 20 port の vmunix が全部 rc=0・warning 0（fde6f9ece）。pc98 は 1d950e03c の後も rc=0・warning 0（build/p005e）。
- QEMU raspi4b（boot-test.sh、画面の PNG を読んだ）:
  - 試験の image `plan/ws331/tests/config-rpi4-text.mk`（`-DKERN_TEST_TEXT_SUSPEND_SECONDS=20`: fb-text.c の試験の thread が kern_text_suspend の後に画面を青で塗り "GRAPHICS OWNER" を描き、30 秒後に kern_text_resume）: login PASS、画面の読み取りで "GRAPHICS OWNER" を見た後、login: の画面に描き直された（resume の後の PNG を見て青が無く grid が戻った事を確認）。途中の PNG は boot-test が残さない。
  - serial の build `plan/ws331/tests/config-rpi4-serial.mk`（CONFIG_RPI4_SERIAL_CONSOLE=y）: `plan/tools/guest/rpi4-serial.sh` で serial の login・`uname -a`・`echo` が通り、boot-test の画面の login も PASS。
- grep: rpi4-console.c・boot-fb・fb-text・pc98-lgy98.c・pc98/graphics/backend.c に `hal/`・`hal_` は無い。残るのは build の macro `HAL_BOARD_*`（未決）。

## 未実施・残り

- /dev/graphics の ioctl を通した enter・leave（guest の program が要る。suspend・resume は kernel の試験の thread で確かめた）。
- amd64 の Venus（virtio-gpu）で flip の後も表示と serial が続く事（`plan/ws290/tests/text-end-qemu.sh core` が合うが、Venus の renderer がこの host に無い）と、pcat の end が範囲を受けて map を外せる事の QEMU の確かめ。
- Pi 5・ODROID-C5・VisionFive 2 の実機（build だけ）。
- WS290 の `plan/ws290/tests/text-end-boards-host-test.sh` が compile できなくなった（stub が VF2 の console_ended と旧 rpi4-console の形を参照）。WS290 の file なので直していない（Q1 に報告済み）。
- 副作用: kernel_putc の公開の後は kernel の log が hal_putc を通らず、Pi の HAL の stall の報告の「log の最後の行」は HAL 自身の行だけになる（HAL の口が要るので行っていない）。HVS がある Pi では、console の状態でも stall の報告が HVS の plane に描かれ、長い stop では firmware の display list を一枚の plane の list に替える。
- pc98 の JIS の表の git mv と HAL の display.c・display.h の git rm は Q1 の OK 待ち（一覧を送った）。

## 再開点

Q1 の OK を受けて pc98 の git mv・git rm と vmunix.mk の path を直し、pc98 の build。その後 phase の判定は Q1。

## 確かめ

- Pi 4: QEMU raspi4b の起動（boot-test.sh で login）と、graphics の後に console に戻った時の描き直し（AAT か画面の PNG）。Pi 5・ODROID-C5 は build（warning 0）。pc98 は build。
- grep で driver に `hal/`・`hal_`・`HAL_` が残らない事（build の macro HAL_ARCH_*・HAL_BOARD_* はユーザーの決定待ち）。
