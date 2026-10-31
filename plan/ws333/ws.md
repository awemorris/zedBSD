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
| [p011](phase011/phase.md) | x86 の CPU の driver の統合: 関門（hypervisor・CPUID の bit・allow list・既定 n・`cpuperf=off`）、MSR の小さな file、probe、HWP の enable の順と既に 1 の道、Highest の MHz の測り、HFI の page、RAPL の meter・滞在の統計、provider の登録、起動の parameter の key（legacy の 0xCE の道は作らない、Q1） | in-progress（P7、q1770-i01。2026-10-31: host 試験 PASS（y 13 場面・n 3 場面、模擬の MSR の #GP 0）、amd64 の vmunix 既定と y の warning 0、全 20 port の vmunix warning 0、QEMU+KVM の y と既定の image で login と「関門で止まり MSR の道の provider が無い」を PASS（HMP）。2026-11-01 q1788 P1 の 5330: HWP の start・HFI・RAPL の meter 4 本・strategy 3 つ・`cpuperf=off` が動いた（試験だけの diag の patch で）。cleared は Q1 の判定） | p006、p008〜p010、p005 の前半 |
| [p012](phase012/phase.md) | tick の位相を揃える（amd64 の HAL の実装）: TSC-deadline の mode で全 CPU の期限を TSC の周期の倍数に、戻す macro `LAPIC_TICK_TSC_DEADLINE=0` | in-progress（P7、q1768-i01。2026-10-31: vmunix warning 0、QEMU+KVM の起動 PASS（periodic と tsc-deadline の両方、SMP 4）、tick 1000 Hz。2026-11-01 q1788 P1 の 5330: idle の package 10.08 W（periodic）→ 6.44 W（TSC-deadline）、PC2〜PC7 は両方 0%） | p002 |
| [p016](phase016/phase.md) | 見張りと非常の停止（signal の disposition、init の SIGUSR2・上書きの禁止、REBOOT・HALT の hook、SLEEP の排他と `kern_thermal_resume`、停止の道の flag、sync の助け、`drv_acpi_poweroff_emergency`、水準の検査、記録の file） | cleared（2026-10-31 Q1、af8aaa5f6・6de483e15・f857f4647、main a57216e81。host 試験 22 場面、QEMU で温度の上がる SSDT から ACPI S5 の電源断まで。init の ACK と注入の ioctl は p005 後半へ持ち越し。`thermal.emergency=off` は作らない（ユーザーに聞く）） | p005 |
| [p017](phase017/phase.md) | ACPI WMI の層（`_WDG` の解析の module と method・query の口）と AML の harness の `plan/tools/aml-host/` への移しと拡張 | cleared（2026-10-31 Q1、caff3950b〜93fb7eadd、main 809dfe329。WMI の試験 9 項目・SMI の書き 0・20 port の build・QEMU の起動。Q1 の判断: WMI の attach の試し直しは p018 の Dell の provider が自分の poll から行う） | p001 |
| [p021](phase021/phase.md) | passive（見張り → poll の thread → 受け手の `limit`、5 秒ごとに 100‰、3 °C 下で戻す） | cleared（2026-10-31 Q1、main a57216e81。host 試験。cpuperf の受け手との実機の結合は p014） | p006、p016 |
| [p019](phase019/phase.md) | sessiond の THERMAL の要求（strategy・boost、root か wheel） | in-progress（P4、q1777-i01。2026-10-31: 要求の口・権限の規則・答えの行と host 試験 PASS 113 checks。kernel への呼びは `/dev/thermal` の UAPI（p004）まで stub（ERROR unavailable）、保存（Q333-1 B）と QEMU は持ち越し） | p005（kernel への呼びは p004） |
| [p018](phase018/phase.md) | Dell の provider（DDV の fan、BIOS の熱の mode の受け手、SMM の boost の確かめ、WMI の attach の試し直し）と 5330 の熱の試験 | in-progress（P7、q1776-i01。2026-10-31: host の harness まで実装、`plan/ws333/tests/dell/run.sh` 30 項目 PASS（5330 の表の SMI の数: attach・fan の読み 0、mode の変更 1、同じ mode 0、boost 3）、amd64 と全 20 port の vmunix warning 0、QEMU amd64 の login（Dell の WMI が無い道）PASS。boost は `CONFIG_DELL_SMM_BOOST`（既定 n）。2026-11-01 q1788 P1 の 5330: attach・4 つの mode の set（7 ms、error 0）・Debian の quiet の読み・起動の `\ODV0` が通った。boost は rpm が上がらず既定 n のまま） | p005、p017 |
