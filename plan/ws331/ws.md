<!-- awesome-plan project=zedbsd record=ws331 -->

# WS331: driver から HAL への直の依存を除き、kernel の口だけを使う形にする

Status: completed（2026-10-31、p006 の規約の全文の見直しの後。判定は Q1）
Primary Milestone: MG001
Parent: [Master](../master.md)
目標: `src/drivers/`・`include/drivers/` の HAL の header の include と `hal_*` の呼び出しを 0 にし、driver は kernel の機能（`kern_*`）だけを使う。規則は [Guardrail](../guardrail.md) の「driver は HAL を知らない」。
Resume point: なし（完了）。

## ユーザーの決定（要点）

- 2026-10-30 ユーザーから、driver から見て HAL は使えず存在も知らない、driver は kernel の機能で map するだけである、driver が HAL を include していれば規約の違反なので drivers の include・src を grep する、という趣旨の指示を受けた。続いて、規則を guardrail に書き、直す WS を立て、まず違反に使われた関数ごとの置き換え先（kernel の関数）の一覧と、kernel に wrapper の無い関数（kernel に足す案）の一覧を出す、という趣旨の指示を受けた。
- 2026-10-30 ユーザーから、p001 の表 A（置き換え先）と表 B（B1〜B8: kern_device_map の WRITECOMBINE・READONLY、MSI の 4 関数、kern_cpu_current、kern_console_early_putc、kern_physaddr_t）の許可を受けた。
- 2026-10-30 p003 で見つかった表 B の漏れについて、ユーザーから、kernel_notify_task に当たる名前は sched 系がよく、kernel_putc はそのままでよい、という趣旨の回答を受けた → kernel_putc は宣言を include/kern/text-display.h に足し、driver の kernel_notify_task は既存の sched_notify_task に替えた。
- 2026-10-30 ユーザーから、rpi4-console.c が HAL の BSP の非公開の関数（rpi_cons_suspend_output・resume_output）まで呼んでいたのは違反であり、Raspberry Pi の console は kernel_putc を入れた時に作り直さなかった事が原因なので今作り直す、という趣旨の指示を受けた（p005）。続いて、/dev/graphics を Pi 4・Pi 5・VisionFive 2・ODROID-C5 にも作り、手本は PC/AT（console は GPU の driver の最初の描画で framebuffer から切り離される）、gpu.c も最初の flip で firmware の framebuffer を手放す、という趣旨の指示を受けた。
- 2026-10-30 ユーザーから、driver の build の macro HAL_ARCH_*・HAL_BOARD_* はなるべく残さない、kernel の header が HAL を include する件は簡単なら分ける、という趣旨の指示を受けた（p008・p009）。
- 2026-10-30 ユーザーから、pc98・pcat の legacy の VRAM・VGA の窓は straight map の中にあるので、HAL からの handoff や kernel の API を足さず、driver が基底 0x80000000 を自分の中で定義して使う、という趣旨の指示を受けた（p010）。
- 2026-10-30 ユーザーから、include/hal/atomic.h の契約を macro だけにし（ENTER の port の直列化は wrapper の macro の中の block で hal_atomic_enter・leave を宣言して呼ぶ、hal_atomic_cookie_t は消すか define にして undef、kernel の atomic.h は HAL の macro で inline 関数を定義した後に undef）、arch の hal_atomic_uint_try_acquire を include/hal/arch/atomic/<arch>.h に分ける、という趣旨の指示を受けた（p010・p011）。

## 結果（2026-10-31）

- driver の HAL への直の依存は 0: `grep -rn '#include *[<"]hal/' src/drivers include/drivers` は 0 行、`hal_` の名前・`HAL_` の macro も driver の source に 0 行（2026-10-31、p006 の tree）。driver の前処理の出力でも file scope の `hal_` の宣言は 0（p011、20 port）。残る `hal_` は kernel の atomic.h の inline 関数の中の block の宣言と局所変数だけ（HAL の macro の契約の形）。
- kernel に足した口（表 B、p002）: `KERN_DEVICE_WRITECOMBINE`・`KERN_DEVICE_READONLY`、`kern_irq_alloc_msi`・`attach_msi`・`detach_msi_sync`・`free_msi`、`kern_cpu_current`、`kern_console_early_putc`、`kern_physaddr_t`。HAL の API（include/hal/hal.h）は変えていない。
- 表 A の置き換え（p003）、i915 の map と MSI（p004）、Raspberry Pi・ODROID-C5・VisionFive 2 の console と /dev/graphics を generic の `src/drivers/generic/boot-fb.c`・`fb-text.c` に作り直し、gpu.c の最初の flip で boot の framebuffer を手放す形（p005）、HAL の early console の framebuffer の窓を kernel が外せる形に（p007）、Pi の共通化と ODROID-C5 の分離（p008）、kernel の header から HAL を分ける（p009）、atomic.h の HAL の分離と pc98・pcat の legacy の窓の基底を driver に（p010）、arch の spinlock の取得を HAL の atomic.h の macro の契約へ（p011）、規約の全文の見直し（p006）。
- p006（規約の全文の見直し、P3、q1737）: WS331 の commit が変えた C（223 file）を 3 つの組で見直し、書き方だけを直した: hal の組（7 file）・kern の組（12 file と sleep.c の段落の分け）・drivers の組（30 file、db1fd7749。条件の中の呼び出しを変数に、意味の無い comment の書き直し、成功の return で終わる形、source の中の開発の管理情報の削除。P3 が差分を全部読み、動作の不変を確かめ、usb-uas-disk.c の unbraced な if と本体の間に comment が入っていた 1 か所を直した）。sun4u・sparcv9 の file は変えていない。
- 確かめ（2026-10-31、db1fd7749 の tree）:
  - build: `plan/tools/ports/build-vmunix-all.sh` と同じ script で 20 port（amd64・intelmac・pcat・pc98・rpi4・rpi5・odroid-c5・visionfive2・zybo・armv7-virt・riscv64-virt・riscv32-virt・m68k-virt・ppc32-mac99・ppc64-pseries・mips32-malta・mips64-malta・mips32-rg350・sun4u・x68k）の vmunix が全部 rc=0・warning 0。試験の macro を定義した 2 つ（`plan/tools/console-end/config-rpi4-end.mk`・`config-visionfive2-end.mk`。boot-fb.c・fb-text.c の `KERN_TEST_TEXT_*` の block を compile する）も rc=0・warning 0。`git diff --check` は空。
  - QEMU（boot-test.sh が画面の PNG を読む。console・serial の log では判定していない）: armv7-virt PASS（`ARMV7 HAL READY AEABI PASS ATOMIC PASS PAGING PASS TIMER TICK`、CPU 1 ONLINE、root が無いので `VFS initialization failed (12)` まで）、riscv64-virt PASS（`RISCV64 HAL READY`、SMP 4 ONLINE、同じく VFS まで）。amd64 uefi-usb の login PASS（image は `plan/tools/boot-images/config-amd64-boot.mk`）、raspi4b の login PASS（`plan/tools/boot-images/config-rpi4-boot.mk`、SD の mmcblk0 から root を mount し init から login: まで）。2 つの disk-image は rc=0、warning は OpenSSL の package の source の 15 だけで zedBSD の source は 0。PNG は P3 が目で見た。
  - build の規則の欠け（Q1 の指示で q1737 の範囲に足した、6993b1736・9bf6aa286）: arm64 の dynamic libc の libm と float（softfloat・compiler-runtime・softfloat128・compiler-runtime128・float-parse）の object の規則に sysroot の stamp の前提が無く、worktree に arm64 の sysroot が無いと -j16 で sysroot の header より先に compile されて落ちた。riscv64・armv7 と同じく stamp を前提に足した（amd64・pcat・pc98 の libm は tree の中の header で compile するので不要）。確かめ: まだ無い sysroot の path（ZEDBSD_SYSROOT_ARM64・ZEDBSD_TARGET_SYSROOT の上書き）で `dynamic/libc.so` を -j16 で build し、sysroot が先に作られ rc=0・warning 0。disk-image そのものは、sysroot が無ければ Makefile の `zedbsd-target-toolchain-ready` が「先に make toolchain」として止める作り（変えていない）。
  - 前の Phase の確かめ（各 Phase の記録の要約）: 20 port の build は p005・p007〜p011 の各時点で rc=0・warning 0。QEMU の起動は amd64（uefi-usb の login）・pcat（bios-ide の login）・raspi4b（login）・m68k-virt・riscv64-virt（VFS まで）。QEMU raspi4b の console の終わり（`end-rpi4-qemu.sh`: graphics の owner の画面 → login → 描き直した login、gdbstub で end の後の HAL の窓が外れ kernel が動き続ける）PASS（p007〜p009）。serial の build の raspi4b で serial の login（p005・p008）。VF2 の実機で end が窓の map を外した事を SSH の dmesg で確かめた（p007）。host 試験は ws290・ws206・ws215・ws048・ws014・ws220・ws221・kernel-lock・ipv6-frag の 20 余りが PASS（p010・p011）。

## 制限・移管

- 未実施の確かめ: VF2 の実機の画面と serial の目視（HDMI の capture・serial が無い。Q1 がユーザーの拾える試験の一覧に置く）。amd64 の Venus（virtio-gpu）で flip の後も表示と serial が続く事（host に Venus の renderer が無い）。Pi 5・ODROID-C5 の実機（build だけ）。Pi 4 の実機（QEMU だけ）。pc98 の起動（build と host 試験の PC-98 の場面だけ）。/dev/graphics の ioctl を通した enter・leave（guest の program が要る）。実際の GPU の最初の flip からの end（試験の thread で同じ範囲を渡した）。arm64 の RAM の中の framebuffer（窓が cached の側）の道。stall・fatal が手放した後の framebuffer を描かない事（code の読みだけ）。odroid-c5 の fdt の host 試験（p006 の時は DTB の取得が HTTP 429）。148 個の host 試験の全数は流していない。`run-gpu-build-selection-test.py` は直したが流していない（TemporaryDirectory で消すため、Q1）。
- 副作用: kernel_putc の公開の後、Pi の HAL の stall の報告の「log の最後の行」は HAL 自身の行だけになる（HAL の口が要るので行っていない）。p004 で、HAL の WC|NOCACHE・WC|DEVICE の拒否の確かめが driver の試験（i915 の ktest-sync.c）から無くなった（HAL の側に試験の置き場が無い）。LTO を使わない m68k-virt・x68k は spinlock の取得に HAL の out-of-line の関数への call が 1 段（元から）。
- 規約の見直しの範囲の外: WS331 が触っていない古い code の違反は直していない（例: pci-uhci.c・pci-xhci.c の unbraced な `else if` と本体の間の comment、usb.c の `Checks the hal atomic ... result.` の型どおりの comment、pc98 の display-*.c の graphics の層を指す `hal` の語）。
- 試験の移し先（今後も回帰に使う）: `plan/tools/ports/build-vmunix-all.sh`（20 port の vmunix の build）、`plan/tools/boot-images/`（text の login だけの amd64・pcat・rpi4・sun4u の image の config）、`plan/tools/console-end/`（generic の console の suspend・resume・end の QEMU raspi4b と VF2 の実機の試験と、その config）。

## Phase

| Phase | 内容 | 最終の状態 |
| --- | --- | --- |
| ws331-p001 | 違反の全数、関数ごとの置き換え先（既存の kern_*）の一覧（表 A）、kernel に足す関数の案（表 B） | cleared（2026-10-30、ユーザーが表 A・B を許可） |
| ws331-p002 | kernel に表 B を足す（driver は変えない） | cleared（2026-10-30 Q1） |
| ws331-p003 | 表 A の機械的な置き換えと include だけの file（sparcv9 の 2 file は別の commit） | cleared（2026-10-30 Q1） |
| ws331-p004 | i915 の map（WRITECOMBINE・READONLY）と MSI、試験の代役 | cleared（2026-10-30 Q1） |
| ws331-p005 | Raspberry Pi の console の作り直し（kernel_putc を公開）、Pi 5・ODROID-C5・VF2 の /dev/graphics（generic の boot-fb・fb-text）、gpu.c の最初の flip で boot の framebuffer を手放す、pc98 の 3 か所 | cleared（2026-10-31 Q1。VF2 の画面の目視と amd64 の Venus での flip の確かめは未実施、制限・移管へ） |
| ws331-p006 | 規約の全文の見直し（WS331 が変えた C、223 file） | cleared（2026-10-31 Q1） |
| ws331-p007 | HAL の early console の framebuffer の窓を kernel が外せる形に（amd64 の fb_window を arm64・riscv64 へ） | cleared（2026-10-30 Q1） |
| ws331-p008 | Pi の共通化（drivers rpi-common、HAL bsp-rpi-common）、ODROID-C5 の分離、残りの HAL_ARCH_* | cleared（2026-10-30 Q1） |
| ws331-p009 | kernel の header から HAL を分ける（kernel の整数の型、_private.h） | cleared（2026-10-30 Q1） |
| ws331-p010 | atomic.h の HAL の分離（HAL の atomic.h を macro だけの契約に）、pc98・pcat の legacy の窓の基底を driver に | cleared（2026-10-30 Q1） |
| ws331-p011 | arch の spinlock の取得と待ちの合図を HAL の atomic.h の macro の契約へ（include/hal/arch/atomic/<arch>.h） | cleared（2026-10-30 Q1） |

Phase の directory とこの WS だけの試験は、完了の規則（AGENTS.md §5）で削除した（git の履歴に残る、2026-10-31 Q1 の OK）。今後も使う試験は上の移し先へ移した。
