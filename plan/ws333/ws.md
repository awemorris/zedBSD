<!-- awesome-plan project=zedbsd record=ws333 -->

# WS333: /dev/thermal で統一する電力・熱の枠組み（CPU の idle・DVFS・P/E core、熱・fan）

- Master: [plan/master.md](../master.md)（2026-10-31 作成）。Primary Milestone: MG004。
- Status: incomplete（2026-10-31 Q1: p001 設計済み、p002・p003 を main に、p006 を main に、p005・p007 を進行中）
- 目標: CPU の idle（C-state、MWAIT）・P-state（HWP と、HWP の無い CPU の driver の P-state）・P/E core の相対性能・熱の監視・fan・非常の停止を、1 つの device `/dev/thermal`（と `/sbin/thermal`）の下で統一して設計し、実装する。
- Resume point: p001（統一の設計、P5）と p002（amd64 の idle の MWAIT、P1）を並べて進める。

## ユーザーの決定（要点）

- 2026-10-31 ユーザーから、WS301（CPU の DVFS と P/E core）と WS303（熱と fan）を廃止して統合し、新しい WS にする、という趣旨の指示を受けた。理由は、2 つの検討を始めた時には /dev/thermal という統一の device を想定しておらず、検討が進んでこの形になったので、統一して設計しないといけない、という事。
- 前の 2 つの WS のユーザーの決定（UAPI は /dev/thermal、HAL は変えない、HWP の無い CPU も P-state を driver で、strategy は performance・balanced・powersave・manual、fan は上げるだけ、非常の停止）は、この WS の設計の入力として引き継ぐ（[ws301/ws.md](../ws301/ws.md)・[ws303/ws.md](../ws303/ws.md) とその design.md・user-questions.md）。
- 2026-10-31 ユーザーから、WS291 p022 の CPU の省電力（idle の C-state・P-state）を先に直す、という趣旨の指示を受けた。→ p002 で amd64 の idle（MWAIT）を先に実装し、P-state は p001 の設計の後。
- 関係: 2026-10-31 WS291 p022（i915、5330）で、zedBSD の idle の package の電力が 12.99 W（Linux 3.2〜3.5 W）で、CPU の idle が HLT（C1）だけ・P-state の制御が無い見込みのため、GPU が電力の上限で 800 MHz に抑えられていると分かった（WS299 の結果の「amd64 の idle が C1 だけ・HWP」と同じ件）。CPU の idle（MWAIT の C-state）はこの WS の範囲に入れる。

## Phase

| Phase | 目的 | 状態 | 依存 |
| --- | --- | --- | --- |
| [p001](phase001/phase.md) | 統一の設計: /dev/thermal の UAPI（CPU の idle・P-state・P/E core・熱の zone・fan・strategy・非常の停止）、kernel の枠組みと driver の分け方、HAL の口で足りるか（足りないと主張する時は理由）、WS301・WS303 の設計からの引き継ぎと食い違いの整理、Phase の分け方。design-reviewer を通す | in-progress（P5、q1734） | — |
| [p002](phase002/phase.md) | amd64 の CPU の idle を HLT から MWAIT の C-state に（5330 で idle の電力を下げ、i915 の GPU の電力の枠を空ける） | in-progress（P1、q1735。2026-10-31: MWAIT C6 を実装、5330 で idle 13.00 → 11.70 W、c600 30.6 → 32.6 fps（GT 800 → 850〜900 MHz）。Linux の idle 3.2〜3.5 W にはまだ遠い） | — |
| [p003](phase003/phase.md) | kernel の口の header（include/kern/thermal.h・cpuperf.h、kern_cpu_count・KERN_CPU_MAX）、design-reviewer 1 回 | cleared（P7、q1754-i01。全 20 port の vmunix の build、warning 0） | p001 |
| [p005](phase005/phase.md) | 熱の core: 写し・sensor の push（acpi-thermal と rpi5-fan）・fan・meter・strategy と受け手（世代の 2 周目）・boost・3 つの thread・事象・ioctl の範囲の口・`hw.thermal` を写しから（2 source）。後半（p004 の後）: `/dev/thermal`・`/dev/thermal-inject`・`/sbin/thermal` の熱の表示 | in-progress（P6、q1762-i01。前半を実装: host 試験 13 scenario PASS、全 20 port の vmunix warning 0、QEMU amd64 の login と `sysctl hw.thermal` PASS。後半は p004 の後） | p003（後半は p004） |
| [p006](phase006/phase.md) | cpuperf の枠組み（kernel）: topology・domain・箱と notify の道・provider の口・相対の性能・受け手「cpu」・strategy の写し・直の設定・上限・legacy の governor・idle の統計・WS302 の口 | cleared（2026-10-31 Q1、main b5c8e8f50。P7、q1754-i01・q1759-i01。host 試験 PASS 196 check、全 20 port の vmunix warning 0、QEMU の起動の回帰 amd64（login）・armv7-virt・riscv64-virt・raspi4b PASS。実機は未実施） | p003 |
| [p007](phase007/phase.md) | x86 の decode の module の設計（標準廉価実装手順の設計の Phase）: SDM Vol. 4 の ADL の MSR の確かめ、header・骨格・試験の道具・p008〜p010 の phase.md、design-reviewer（design.md §15） | cleared（2026-10-31 Q1、39f4a5597・f45480c58。run-unit・check-skeleton を Q1 も流して PASS。Q1 の判断: legacy の 0xCE の比の道は SDM に根拠が無いので外す（p011 は HWP の道だけ）。P7、q1759-i01。header の基点 39f4a5597、design-reviewer 1 回 major 5・minor 12 を全部反映、`cpux86.c` は p007 で実装 4140 check。cleared は Q1 の判定の後） | p001 |
| [p008](phase008/phase.md) | module: CPUID の解析（cpux86-cpuid.c） | cleared（2026-10-31 Q1、fae4a0d41。test-cpuid 3590873 checks、全 PASS・check-skeleton PASS、突然変異 44 のうち 43 を検出（残りは同値）） | p007 |
| [p009](phase009/phase.md) | module: HWP の register と HFI の表（cpux86-hwp.c） | cleared（2026-10-31 Q1、9fb61c61e。test-hwp 12804888 checks、全 PASS・check-skeleton PASS、突然変異 24 を検出） | p007 |
| [p010](phase010/phase.md) | module: 0xCE と、allow list・RAPL・C-state の滞在・0xE2（cpux86-pstate.c・cpux86-power.c）。`_PSS`・AMD・PERF_CTL は仕様未取得／使い道が無く外した | cleared（2026-10-31 Q1、35d5d4500。test-pstate 120186・test-power 275202 checks、全 PASS と check-skeleton を Q1 も流して PASS、突然変異 18 を検出） | p007 |
| [p017](phase017/phase.md) | ACPI WMI の層（`_WDG` の解析の module と method・query の口）と AML の harness の `plan/tools/aml-host/` への移しと拡張 | in-progress（P6、q1771-i01。2026-10-31: 実装、WMI の host 試験 9 項目 PASS（5330 の表で SMI の書き 0）、harness の回帰 PASS、全 20 port の vmunix warning 0、QEMU amd64 の login PASS。cleared は Q1 の判定の後） | p001 |
| [p012](phase012/phase.md) | tick の位相を揃える（amd64 の HAL の実装）: TSC-deadline の mode で全 CPU の期限を TSC の周期の倍数に、戻す macro `LAPIC_TICK_TSC_DEADLINE=0` | in-progress（P7、q1768-i01。2026-10-31: vmunix warning 0、QEMU+KVM の起動 PASS（periodic と tsc-deadline の両方、SMP 4）、tick 1000 Hz。5330 の前後の電力と PC の滞在は未実施） | p002 |
