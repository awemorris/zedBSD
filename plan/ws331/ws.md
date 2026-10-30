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

## 調べの起点（2026-10-30 Q1 の grep、main f29e49c3c）

- include: `hal/hal.h` 38 file、`hal/i386/i386.h`（pc98-lgy98.c）、`hal/i386/bsp-pc98/display.h`（pc98/graphics/backend.c）、`hal/arm64/bsp-rpi-common/cons.h`（rpi4-console.c）。`include/drivers/` は 0。
- 呼び出しの多い関数: hal_space_map_device 15・hal_space_unmap_device 13・hal_get_arch_handoff 10・hal_irq_free_msi 9・hal_putc 8・hal_rtc_read_counter 7・hal_irq_alloc_msi 7・hal_cpu_current 6、ほかに hal_io_*・hal_mmio_*・hal_irq_enable/disable/send_eoi/attach_msi/detach_msi_sync。

## Phase

| Phase | 目的 | 状態 | 依存 |
| --- | --- | --- | --- |
| [p001](phase001/phase.md) | 違反の全数と、関数ごとの置き換え先（既存の kern_*）の一覧、wrapper の無い関数（kernel に足す案）の一覧 | in-progress（P2、q1713） | — |
| p002〜 | 一覧をユーザーが見た後に立てる（kernel の関数の追加、driver の書き換え、規約の全文の見直し） | — | p001 |
