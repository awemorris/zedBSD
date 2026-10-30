<!-- awesome-plan project=zedbsd record=ws331 -->

# WS331: driver から HAL への直の依存を除き、kernel の口だけを使う形にする

Status: incomplete
Primary Milestone: MG001
Parent: [Master](../master.md)
目標: `src/drivers/`・`include/drivers/` の HAL の header の include と `hal_*` の呼び出しを 0 にし、driver は kernel の機能（`kern_*`）だけを使う。規則は [Guardrail](../guardrail.md) の「driver は HAL を知らない」。
Resume point: p001 の一覧（表 A・表 B・例外・Phase の案・未決）を P2 が phase001/phase.md に書いた（q1713）。Q1 がユーザーに表 B と未決を見せ、決定の後に p002〜 を立てる。

## ユーザーの決定（原文、2026-10-30）

- 「ドライバから見て、HALは使えません。存在を知りません。カーネルの機能を使ってマッピングするのみです。」
- 「ドライバがHALをincludeしているところがあれば、規約違反ですので、driversをinclude/srcともにgrepしてみてください。」
- 「了解です。ガードレールに記載してください。これを修正するWSを立ててください。P2で実行してください。HALのマップ関数には、カーネル側に相当する関数があるはずです。まずは違反利用された関数が、どの関数に置き換えられるか、一覧を作成してください。そして、ラッパーがない場合、カーネルに関数追加しますので、その一覧をください。」
- 解釈（Q1）: (1) 規則を guardrail に書く（済み）。(2) 最初の成果は 2 つの一覧: 違反に使われた `hal_*` の関数ごとに、置き換える kernel の関数（既存の wrapper）の一覧と、kernel に wrapper が無い関数の一覧（kernel に足す関数の案）。ユーザーが見て、kernel に足す関数を決める。(3) driver の書き換えは一覧の後の Phase で（一覧の後に Phase を立てる）。HAL の API は変えない。

- 2026-10-30 ユーザー（原文）「表A,Bを許可します」 → p001 の表 A（置き換え先）と表 B（B1〜B8: kern_device_map の WRITECOMBINE・READONLY、MSI の 4 関数 kern_irq_alloc_msi・attach_msi・detach_msi_sync・free_msi、kern_cpu_current、kern_console_early_putc、kern_physaddr_t）を許可。MSI は表 B の関数を足す形（別の案は採らない、Q1 の解釈）。未決のまま: rpi4 の console の 2 関数、build の macro HAL_ARCH_*、kernel の header が HAL の header を include する件。
- 2026-10-30 p003 で見つかった表 B の漏れ（kernel_putc・kernel_notify_task、hal.h だけに宣言のある kernel の名前）へのユーザーの回答（原文）「kern_task_notifyはなんか変。sched系の名前にするのがいいのでは？putcはOK。」 → kernel_putc は既存の宣言を include/kern/text-display.h に足す。kernel_notify_task は新しい関数を足さず、既存の sched_notify_task（include/kern/sched.h）に driver の呼び出しを替える（Q1 の解釈）。
- 2026-10-30 ユーザーから、rpi4-console.c が呼ぶ rpi_cons_suspend_output・rpi_cons_resume_output（HAL の Raspberry Pi の BSP の中の非公開の関数）について、HAL の呼び出しに加えて非公開の関数まで呼んでいたのは違反であり、resume に当たる処理は driver の中に同等の関数を定義して使えばよい、という趣旨の指示を受けた。→ Q1 の解釈: suspend は hal.h の既存の約束（kernel が kernel_putc を公開したら HAL の early console は描かない）で済ませ、BSP がその約束を守っていなければ HAL の実装（src/hal/）を直す（hal.h は不変）。resume（残した文字の描き直し）は driver の中に実装する。p005 で行う。
- 2026-10-30 ユーザーから、Raspberry Pi の console は kernel_putc を入れた時に作り直さなかった事が原因なので、今ここで作り直す、という趣旨の指示を受けた。→ p005 で rpi4-console.c（Pi 4・Pi 5・ODROID-C5）を sun4u-console.c と同じ形に作り直す: driver が firmware の framebuffer を kernel の口で map し、自分の文字の表と font で描き、serial の時は UART の送信も driver で行い、kernel_putc を公開する。graphics が画面を持つ間は描かず、返された時に driver が描き直す。HAL の watch.c の描く先の判断は HVS の状態だけで決める。cons.c の rpi_cons_suspend/resume_output は使われなくなる。
- 2026-10-30 ユーザーから、driver の build の macro HAL_ARCH_*・HAL_BOARD_* はなるべく残さない、kernel の header が HAL を include する件は簡単なら分ける、という趣旨の指示を受け、Q1 の調べ（16 か所、13 header）を受けて p008・p009 の形になった。
- 2026-10-30 ユーザーから、pc98・pcat の legacy の VRAM・VGA の窓は straight map の範囲の中にあるので、framebuffer のように HAL から handoff したり straight map の基底を取る kernel の API を足したりはせず、platform driver は基底 0x80000000 を知っている前提で作り、その基底は driver の中で定義し直して HAL の定数を参照しない、という趣旨の指示を受けた。
- 2026-10-30 ユーザーから、include/hal/atomic.h の契約を「macro だけ」にし、ENTER の port の直列化は wrapper の macro の中の scope で hal_atomic_enter・leave を block の中で宣言して呼ぶ形にし、hal_atomic_cookie_t は消すか define にして include 元で undef し、arch.h・types.h の 2 行の include は外す（hal.h の中で include される事を期待し、kernel の atomic.h が単体で include する使い方も認める）、kernel の atomic.h は HAL の macro で inline 関数を定義した後に HAL の macro を undef する、という趣旨の指示を受けた。HAL_ATOMIC_STYLE は kernel の atomic.h が build の設定から一時的に定義して使い、undef する（Q1 の補い）。

## 調べの起点（2026-10-30 Q1 の grep、main f29e49c3c）

- include: `hal/hal.h` 38 file、`hal/i386/i386.h`（pc98-lgy98.c）、`hal/i386/bsp-pc98/display.h`（pc98/graphics/backend.c）、`hal/arm64/bsp-rpi-common/cons.h`（rpi4-console.c）。`include/drivers/` は 0。
- 呼び出しの多い関数: hal_space_map_device 15・hal_space_unmap_device 13・hal_get_arch_handoff 10・hal_irq_free_msi 9・hal_putc 8・hal_rtc_read_counter 7・hal_irq_alloc_msi 7・hal_cpu_current 6、ほかに hal_io_*・hal_mmio_*・hal_irq_enable/disable/send_eoi/attach_msi/detach_msi_sync。

## Phase

| Phase | 目的 | 状態 | 依存 |
| --- | --- | --- | --- |
| [p001](phase001/phase.md) | 違反の全数と、関数ごとの置き換え先（既存の kern_*）の一覧、wrapper の無い関数（kernel に足す案）の一覧 | cleared（2026-10-30 ユーザーが表 A・B を許可） | — |
| [p002](phase002/phase.md) | kernel に表 B を足す（driver は変えない） | cleared（2026-10-30 Q1） | p001 |
| [p003](phase003/phase.md) | 表 A の機械的な置き換えと include だけの file（sparcv9 の 2 file は別の commit） | cleared（2026-10-30 Q1） | p002 |
| [p004](phase004/phase.md) | i915 の map（WRITECOMBINE・READONLY）と MSI（B2〜B5）、試験の代役 | cleared（2026-10-30 Q1） | p002 |
| [p005](phase005/phase.md) | rpi4 の console の作り直し（kernel_putc を公開）と pc98 の 3 か所。追加: Pi 5・ODROID-C5・VF2 の /dev/graphics（generic の boot-fb・fb-text）、gpu.c の最初の flip で boot の framebuffer を手放す | in-progress（P2、q1719。84e3e2698〜977f43204、20 port の build、raspi4b の起動・描き直し・serial、WS290 の host 試験、VF2 の実機の起動（画面・serial の目視は未確かめ）は済み、Venus は未実施、判定は Q1） | p003 |
| [p007](phase007/phase.md) | HAL の early console の framebuffer の窓を、kernel が外せる形に（amd64 の fb_window を arm64・riscv64 へ） | cleared（2026-10-30 Q1） | p005 |
| [p008](phase008/phase.md) | Pi の共通化（drivers rpi-common、HAL bsp-rpi-common）、ODROID-C5 の分離（drivers s905x5m、HAL bsp-s905x5m-common と bsp-odroidc5）、残りの HAL_ARCH_* | in-progress（P2、q1721。74d6760d2〜ae0b1b031、20 port の build、driver の HAL_ の grep 0、raspi4b の login・描き直し・end・serial は済み、Pi 5・ODROID-C5 は build だけ、判定は Q1） | p007 |
| [p009](phase009/phase.md) | kernel の header から HAL を分ける（kernel の整数の型、_private.h） | in-progress（P2、q1721。22e324fc9・e5e316da5、20 port の build、amd64・rpi4・m68k-virt・riscv64-virt の QEMU、atomic.h は判断待ち、判定は Q1） | p008 |
| [p010](phase010/phase.md) | atomic.h の HAL の分離（HAL の atomic.h を macro だけの契約に）、pc98・pcat の legacy の窓の番地を driver の基底で | in-progress（P2、q1723） | p009 |
| p006 | 規約の全文の見直し | 未作成 | p003〜p005 |
