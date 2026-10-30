<!-- awesome-plan project=zedbsd record=ws331-p007 -->

# ws331-p007: HAL の early console の framebuffer の map を、kernel が外せる形に（amd64 の fb_window を他の platform へ）

Status: in-progress（P2 の作業は済み、判定は Q1）
Parent: [WS331](../ws.md)
Queue: q1720（P2）

## 目的

2026-10-30 ユーザーから、HAL が unmap device できるよう、early console の GOP（boot の framebuffer）の領域を map の list に入れておく形が GHCI の時に PC か VF2 で入っているはずなので確かめ、あれば他の platform でも真似する、という趣旨の指示を受けた。

## Q1 が確かめた事（amd64）

- `src/hal/amd64/space.c` の `fb_window`（状態 NONE・HELD・GIVEN・REMOVED、`fb_window_hold`・`fb_window_give`・`fb_window_remove`、3392 行の辺り）: HAL の early console が boot の framebuffer を専用の窓（ZBL6_FRAMEBUFFER_VIRTUAL_BASE）に map して HELD。kernel が framebuffer のちょうどの範囲を `hal_space_map_device` で求めると、その窓を渡して GIVEN（424 行）。kernel がそれを `hal_space_unmap_device` で外すと REMOVED（462 行）、early console は `pcat_cons_framebuffer_release`（bsp.h:65）で以後描かない。
- kernel の側は pcat の `backend.c` の `drv_pcat_graphics_backend_end`（1330 行の辺り）が、`kern_device_map` で窓を受け取り `kern_device_unmap` で外す。

## 範囲

1. arm64（Raspberry Pi 4・5 の `bsp-rpi-common`、ODROID-C5 の `bsp-s905x5m`）と riscv64（VisionFive 2）の HAL に、同じ形の窓（hold・give・remove と early console の release）を実装する。Pi は今 framebuffer を direct map で描いているので、専用の窓に移す。hal.h は変えない（HAL の実装の補完）。
2. 共通の `boot-fb.c` の end を、pcat の backend と同じく、HAL の窓を `kern_device_map` で受け取って `kern_device_unmap` で外す形にし、外れたら EBUSY を返さない。
3. HAL の stall の報告・致命的な停止の表示は、窓が外れた後は framebuffer に描かない（HVS の plane など、描ける所だけ）。
4. 確かめ: QEMU raspi4b で、GPU の最初の flip に当たる試験（kern_text_end に範囲を渡す）で EBUSY が出ず map が外れる事、login と描き直し。VF2 の実機で起動と /dev/graphics、map が外れる事（log）。Pi 5・ODROID-C5 は build。20 port の build。

## 決めた形（P2、q1720）

- 形は amd64 の fb_window と同じ: HAL の early console が boot の framebuffer を専用の窓で描き（HELD）、kernel_putc の公開の後に framebuffer のちょうどの範囲（base と size、READ|WRITE|NOCACHE）を求める `hal_space_map_device` にその窓を渡し（GIVEN）、その `hal_space_unmap_device` で early console に忘れさせてから窓を外す（REMOVED、全 CPU の TLB を落とす）。hal.h は不変。
- arm64（`src/hal/arm64/space.c`、Pi 4・Pi 5・ODROID-C5 が共有する `bsp-rpi-common/framebuffer.c`）:
  - 窓は system 半分の L0[2]（0xffff010000000000〜、1 GiB）。表は静的（L1・L2 と端の 2 つの L3）で、framebuffer の page だけを map し、隣の memory は入れない。`arm64_space_init` が表の切り替えの前に作り、切り替えの後に `rpi_framebuffer_move` で early console の描き先を窓へ移す（それまでは boot の表の direct map）。
  - memory の型を direct map と揃える（同じ page を型の違う 2 つの map にしない）: framebuffer が RAM なら窓は cached（direct map と同じ。描いた行は今まで通り clean）、RAM の外なら窓は Device-nGnRE（`hal_space_map_device` が device に与える型）で、direct map の split された 2 MiB block のうち RAM を含まない物も同じ時に device block にし、切り替えの前に古い cached の line を clean+invalidate する。
  - 外す時: `rpi_cons_framebuffer_release`（cons.c、output lock の下で `rpi_framebuffer_forget`）→ L0[2] を 0 → `arm64_flush_tlb`（tlbi vmalle1is）。
  - stall の報告・致命的な停止（watch.c）: 手放した後は、cell の painter は描かず（描き先が NULL）、HVS の plane のうち手放した framebuffer に重なる物は飛ばす（`rpi_framebuffer_given_up`）。他の plane には今まで通り描く。
  - 窓が 1 GiB に入らない framebuffer は窓を作らず、今まで通り direct map で描く（渡さない）。
- riscv64（`src/hal/riscv64/space.c`、VisionFive 2）: 窓は root 448（0xfffffff000000000〜、1 GiB）。root の上半分は init の後に封じるので、`riscv64_space_init` が二段目の表（端は page の表、`riscv64_window_fill_pages`）を作って root に繋ぐ。外す時は `riscv64_cons_detach_screen`（cons_lock の下で `riscv64_fbcons_detach`）→ 二段目の表の entry を全部 0 → `riscv64_tlb_flush_everywhere`（root の entry は空の表を指したまま）。board の口を 2 つ足した（`bsp.h`: `riscv64_board_screen`・`riscv64_board_screen_window`。VF2 は loader の画面と、flush の address の付け替え、virt は無し）。窓を持つ時は `map_hal_devices` が画面を device window に map しない。riscv64 の memory の型は PMA が決めるので型の問題は無い。riscv64 の HAL は kernel_putc の公開の後は画面に描かない（stall の報告の描画は無い）。
- `src/drivers/generic/boot-fb.c` の end: 自分の map を外し、次に `kern_device_map(画面の base, 画面の size, UNCACHED)` で early console の窓を受け取って `kern_device_unmap` で外す（pcat の backend と同じ。受け取れなければ普通の map が返り、同じく外れる）。外れたら EBUSY を返さない。外せなかった時だけ、範囲に重なれば EBUSY（外せない map）か外す失敗の error。`include/drivers/generic/boot-fb.h` の説明も直した。driver は HAL を知らない（kern_* だけ）。
- 試験の口: `KERN_TEST_TEXT_END_SECONDS`（boot-fb.c、試験の build だけ）で、画面の map の後の指定の秒に画面の範囲で `kern_text_end` を呼ぶ thread（GPU の最初の flip と同じ範囲）。結果・受け取った map の address を volatile の変数に残し、log に 1 行書く。

## 結果（2026-10-30）

- commit: 6888c59b7（基点 627bd83ff）。記録の commit は別（この節）。
- build: `plan/ws331/tests/build-vmunix-all.sh build/p007c` で 20 port の vmunix が rc=0・warning 0（pcat は最初の並列の run で `build/i386/sysroot` の header が見つからず rc=2、pcat だけを流し直して rc=0・warning 0。同じ sysroot を使う pc98 との並列の時の競合と見ている、確かめてはいない）。rpi4・rpi5・odroid-c5・visionfive2・riscv64-virt は build/p007b でも rc=0・warning 0。
- host 試験: `plan/ws290/tests/text-end-boards-host-test.sh` PASS（rpi4 の scene を「画面の上で 0、自分の map と画面ちょうどの map の 2 つが外れる」に、vf2_ranges を「外せない map（EBUSY）なら画面の上で EBUSY」に直し、scene 15「外す失敗の error を返す」を足した。15 scene × 3 mode）、`text-end-host-test.sh` PASS、`ghci-host-test.sh` PASS。
- QEMU raspi4b（`plan/ws331/tests/end-rpi4-qemu.sh build/p007-end`、image は `config-rpi4-end.mk`: suspend 20 秒・30 秒後に resume、map の 90 秒後に画面の範囲で end。gdbstub 127.0.0.1:2221、QMP。console・serial の log では判定していない）: PASS。
  - 画面（boot-test.py で PNG を読む）: "GRAPHICS OWNER"、login、resume の後に再び login（描き直し）。PNG は build/tmp/end-rpi4.O4JYeg/（login-1.png が owner の青い画面、login-after.png が描き直した login の画面、end.png が end の後。P2 が目で見た: end の後の画面は end の前と同じ文字のまま）。
  - gdbstub: end の戻り値 0、HAL の窓の state=3（REMOVED）、L0[2]=0、窓の address 0xffff010000100000 は "Cannot access memory"、HAL の pixels=NULL・pixels_given_up=1、scheduler_ticks は 5 秒で 0x16de3→0x181ed（kernel は動き続けた）。
  - この image は `fb_end_test_handed`（受け取った map の address の記録）を足す前の source で作った（他は 6888c59b7 と同じ）。
- VF2 の実機（`plan/ws331/tests/end-vf2.sh`、kernel は `config-visionfive2-end.mk`、VF2_PLUG=14C19FA80F86）: PASS。板の kernel（cksum 2825517684）は既にあった /boot/boot0/vmunix.p2old（同じ cksum）を使った。試験の kernel で起動し、SSH の dmesg に `boot framebuffer: the timed end returned 0, the screen's mapping at 0xfffffff000000000 removed`（窓の address を受け取って外した）、`visionfive2: framebuffer 1920x1080 pitch 7680 format 2 at fe000000 size 7e9000`、powervr の render-node=ok、`/dev/graphics` あり。その後に元の kernel を戻して cksum 2825517684 を照合、入れ直して SSH が上がり `/dev/graphics` が無い（元の kernel）事を確かめた。KC705・zedbsd.cfg には触っていない。

## 未実施・残り

- Pi 5・ODROID-C5 の実機（build だけ）。Pi 4 の実機（QEMU だけ。実機の firmware の framebuffer は RAM の外のはずで、QEMU と同じ Device の側の道を通る、未確かめ）。
- arm64 の RAM の中の framebuffer（窓が cached の側）は、この試験の環境では通っていない（QEMU raspi4b の framebuffer は RAM の外）。ODROID-C5 の U-Boot の画面がこの側の見込み、未確かめ。
- VF2 の実機の画面の目視（HDMI の capture が無い）。end の後の画面の変化も見ていない（log と SSH だけ）。
- arm64・riscv64 の窓の host 試験（amd64 の hal-fbwindow-host-test に当たる物）は作っていない。
- stall の報告・致命的な停止が手放した後に framebuffer を描かない事は、code の読みだけ（実際に stall・fatal を起こしてはいない）。
- 実際の GPU の最初の flip（gpu.c）からの end は通していない（試験の thread で同じ範囲を渡した）。
- 判定は Q1。

## 再開点

範囲 1〜4 の作業は済み。残りは上の未実施。
