<!-- awesome-plan project=zedbsd record=ws333 -->

# WS333: /dev/thermal で統一する電力・熱の枠組み（CPU の idle・DVFS・P/E core、熱・fan）

- Master: [plan/master.md](../master.md)（2026-10-31 作成）。Primary Milestone: MG004。
- Status: completed（2026-11-13 ユーザーの判定。全 Phase が cleared か canceled。結果: 5330 の idle の package 4.34〜4.40 W（前 5.30〜5.53 W）・PC2 86〜88 %・GT RC6 99.9 %、MWAIT < HLT、HWP の strategy と MHz の読み、E-core の module の domain、Pi 5 の fan、Dell の provider、/dev/thermal の UAPI と /sbin/thermal、TCO watchdog。外に移した物: S0 idle は [BUG-424](../bugs/BUG-424.md)（VMD の suspend の後に 5330 の電力を再計測、2026-11-13 ユーザー）、100 Hz での ms の待ちの丸めと i915 の worker・job の期限の構造は下の「Q1 の記録」）
- 目標: CPU の idle（C-state、MWAIT）・P-state（HWP と、HWP の無い CPU の driver の P-state）・P/E core の相対性能・熱の監視・fan・非常の停止を、1 つの device `/dev/thermal`（と `/sbin/thermal`）の下で統一して設計し、実装する。
- **前提（2026-11-12）**: build system の作り直しの後、WS333 の作業と試験は `CONFIG_KERN_THERMAL=y`・`CONFIG_DRIVER_X86_POWER_CONTROL=y` で build する（ユーザー）。既定の値は扱わない。作り直しの前の build・既定の記述は過去の物で、従わない。
- Resume point: 完了（2026-11-13）。続きは BUG-424（VMD の S0 idle と 5330 の再計測）。

## ユーザーの決定（要点）

- 2026-11-13 ユーザーから、次の趣旨の指示を受けた: tick の周期は arch ごとに選べる値で、今の amd64 の 1000 Hz はたまたま固定になっているだけ。一時的に 100 Hz に下げる（Q1 が `include/hal/arch/amd64.h` を直した）。tickless idle の設計（p023）は立てない。
- 2026-11-13 ユーザーから、次の Makefile・mk の差分の承認を受けた: (1) `mk/platform-intelmid64/defaults.mk` の `CONFIG_DRIVER_DELL_SMM_BOOST` を n に（Dell の boost は熱の枠組みに依り、intelmid64 は `CONFIG_KERN_THERMAL` n で link できなかった）、`plan/tools/aml-host/Makefile` の macro を `CONFIG_DRIVER_DELL_SMM_BOOST` に（Q1 が当てた）。(2) `mk/platform-x86_64/vmunix.mk` の KERN_SOURCES に `src/drivers/platform/pcat/intel-tco.c` の 1 行（p028、P3 が当てる）。
- 2026-11-13 ユーザーから、次の趣旨の決定を受けた: (1) CPU の domain は物理的に同じ clock で動く CPU の組にする。Alder Lake の E-core は module（4 つで 1 つ）を見込み、5330 の測定で確かめて直す（p025）。(2) CPU の MHz は読み手の権限によらず新しい値を返し、費用は共有の sample で抑える。`READ_SAMPLE` と書き込みの制限は外す（p026）。(3) affinity の無い thread は自由に CPU を移る。留まるのは affinity のある thread だけ（BUG-422）。

- 2026-11-12 ユーザーから、次の趣旨の決定を受けた: `/dev/thermal` は desktop の group の利用者が操作できるようにし（video の group と GPU と同じ形）、sessiond を通さない。sessiond に委ねるのは認証だけ。→ 新しい group `desktop`、`/dev/thermal` は root:desktop 0664、設定の ioctl は書き込みで開いた fd に許す、`/etc/thermal.conf` は root:desktop 0664、sessiond の THERMAL の要求（p019）は取り下げ、`/sbin/thermal` は機種の選びで入れる（全 image ではない）。
- 2026-11-12 ユーザーから、p004 の UAPI の案を承認し P1 で進める、という趣旨の指示を受けた（電力の読みは推し A と Q1 が解釈）。

- 2026-11-01 09:05 ユーザーから、設計の問い Q333-1〜7 を全部推しの案で決める、という趣旨の答えを受けた（Q333-4 の 0〜100 の向きは「どれも 100 = その物が多い」で確定）。

- 2026-10-31 ユーザーから、WS301（CPU の DVFS と P/E core）と WS303（熱と fan）を廃止して統合し、新しい WS にする、という趣旨の指示を受けた。理由は、2 つの検討を始めた時には /dev/thermal という統一の device を想定しておらず、検討が進んでこの形になったので、統一して設計しないといけない、という事。
- 前の 2 つの WS のユーザーの決定（UAPI は /dev/thermal、HAL は変えない、HWP の無い CPU も P-state を driver で、strategy は performance・balanced・powersave・manual、fan は上げるだけ、非常の停止）は、この WS の設計の入力として引き継ぐ（[ws301/ws.md](../ws301/ws.md)・[ws303/ws.md](../ws303/ws.md) とその design.md・user-questions.md）。
- 2026-10-31 ユーザーから、WS291 p022 の CPU の省電力（idle の C-state・P-state）を先に直す、という趣旨の指示を受けた。→ p002 で amd64 の idle（MWAIT）を先に実装し、P-state は p001 の設計の後。
- 関係: 2026-10-31 WS291 p022（i915、5330）で、zedBSD の idle の package の電力が 12.99 W（Linux 3.2〜3.5 W）で、CPU の idle が HLT（C1）だけ・P-state の制御が無い見込みのため、GPU が電力の上限で 800 MHz に抑えられていると分かった（WS299 の結果の「amd64 の idle が C1 だけ・HWP」と同じ件）。CPU の idle（MWAIT の C-state）はこの WS の範囲に入れる。

## Phase

| Phase | 目的 | 状態 | 依存 |
| --- | --- | --- | --- |
| [p001](phase001/phase.md) | 統一の設計: /dev/thermal の UAPI（CPU の idle・P-state・P/E core・熱の zone・fan・strategy・非常の停止）、kernel の枠組みと driver の分け方、HAL の口で足りるか（足りないと主張する時は理由）、WS301・WS303 の設計からの引き継ぎと食い違いの整理、Phase の分け方。design-reviewer を通す | cleared（2026-11-12 Q1 判定: 第 3 版で main の実装と新しい build に合わせ、未登録の Phase を立てた。材料は下の「判定の材料」） | — |
| [p002](phase002/phase.md) | amd64 の CPU の idle を HLT から MWAIT の C-state に（5330 で idle の電力を下げ、i915 の GPU の電力の枠を空ける） | cleared（2026-11-12 Q1 判定: 5330 で idle 13.00 → 11.70 W、MWAIT の路の直の観察は p014） | — |
| [p003](phase003/phase.md) | kernel の口の header（include/kern/thermal.h・cpuperf.h、kern_cpu_count・KERN_CPU_MAX）、design-reviewer 1 回 | cleared（P7、q1754-i01。全 20 port の vmunix の build、warning 0） | p001 |
| [p004](phase004/phase.md) | `/dev/thermal` の UAPI の全文の案（ioctl の番号と struct・事象・注入・`/sbin/thermal` の使い方）、design-reviewer、**ユーザーの review** | cleared（2026-11-12 Q1 判定: ユーザーが UAPI の案を承認、電力の読みは推し A） | p001 |
| [p005](phase005/phase.md) | 熱の core: 写し・sensor の push（acpi-thermal と rpi5-fan）・fan・meter・strategy と受け手（世代の 2 周目）・boost・3 つの thread・事象・ioctl の範囲の口・`hw.thermal` を写しから（2 source）。後半（p004 の後）: `/dev/thermal`・`/dev/thermal-inject`・`/sbin/thermal` の熱の表示 | cleared（2026-11-12 Q1 判定: core の host 試験・全 platform の build・QEMU の回帰を満たした。riscv64 virt は新しい build に無く取り下げ） | p003（後半は p004） |
| [p006](phase006/phase.md) | cpuperf の枠組み（kernel）: topology・domain・箱と notify の道・provider の口・相対の性能・受け手「cpu」・strategy の写し・直の設定・上限・legacy の governor・idle の統計・WS302 の口 | cleared（2026-10-31 Q1、main b5c8e8f50。P7、q1754-i01・q1759-i01。host 試験 PASS 196 check、全 20 port の vmunix warning 0、QEMU の起動の回帰 amd64（login）・armv7-virt・riscv64-virt・raspi4b PASS。実機は未実施） | p003 |
| [p007](phase007/phase.md) | x86 の decode の module の設計（標準廉価実装手順の設計の Phase）: SDM Vol. 4 の ADL の MSR の確かめ、header・骨格・試験の道具・p008〜p010 の phase.md、design-reviewer（design.md §15） | cleared（2026-10-31 Q1、39f4a5597・f45480c58。run-unit・check-skeleton を Q1 も流して PASS。Q1 の判断: legacy の 0xCE の比の道は SDM に根拠が無いので外す（p011 は HWP の道だけ）。P7、q1759-i01。header の基点 39f4a5597、design-reviewer 1 回 major 5・minor 12 を全部反映、`cpux86.c` は p007 で実装 4140 check。cleared は Q1 の判定の後） | p001 |
| [p008](phase008/phase.md) | module: CPUID の解析（cpux86-cpuid.c） | cleared（2026-10-31 Q1、fae4a0d41。test-cpuid 3590873 checks、全 PASS・check-skeleton PASS、突然変異 44 のうち 43 を検出（残りは同値）） | p007 |
| [p009](phase009/phase.md) | module: HWP の register と HFI の表（cpux86-hwp.c） | cleared（2026-10-31 Q1、9fb61c61e。test-hwp 12804888 checks、全 PASS・check-skeleton PASS、突然変異 24 を検出） | p007 |
| [p010](phase010/phase.md) | module: 0xCE と、allow list・RAPL・C-state の滞在・0xE2（cpux86-pstate.c・cpux86-power.c）。`_PSS`・AMD・PERF_CTL は仕様未取得／使い道が無く外した | cleared（2026-10-31 Q1、35d5d4500。test-pstate 120186・test-power 275202 checks、全 PASS と check-skeleton を Q1 も流して PASS、突然変異 18 を検出） | p007 |
| [p011](phase011/phase.md) | x86 の CPU の driver の統合: 関門（hypervisor・CPUID の bit・allow list・既定 n・`cpuperf=off`）、MSR の小さな file、probe、HWP の enable の順と既に 1 の道、Highest の MHz の測り、HFI の page、RAPL の meter・滞在の統計、provider の登録、起動の parameter の key（legacy の 0xCE の道は作らない、Q1） | cleared（2026-11-12 Q1 判定: 受け入れは済み、build の前提は ws.md の「前提」） | p006、p008〜p010、p005 の前半 |
| [p012](phase012/phase.md) | tick の位相を揃える（amd64 の HAL の実装）: TSC-deadline の mode で全 CPU の期限を TSC の周期の倍数に、戻す macro `LAPIC_TICK_TSC_DEADLINE=0` | cleared（2026-11-12 Q1 判定: 5330 で idle の package 10.08 → 6.44 W、PC の滞在 0% と lock の取り合いは p014） | p002 |
| [p013](phase013/phase.md) | CPU の ioctl・`/sbin/thermal` の CPU と電力の表示・strategy の保存と rc の restore（Q333-1 B） | cleared（2026-11-12 Q1 判定: CPU の ioctl の host 試験、MSR の道の有無の build、QEMU の起動の restore。riscv64 は取り下げ） | p004、p005 後半、p006 |
| [p014](phase014/phase.md) | 5330 の CPU の受け入れ（design.md §8.4 の 1〜4、1 本の script）と §4.3.3 段 4 の数。p002・p011・p012・p018 の実機の残りを集める | cleared（2026-11-13 Q1。S0 idle は BUG-424 へ） | p011、p012、p013 |
| [p015](phase015/phase.md) | amd64 の既定で MSR の道を有効に（Q333-7 A。今の mk の既定との食い違いは design.md §16.2） | canceled（2026-11-12: 既定の値は WS333 の扱いの外、作業は y で build する） | p014、mk の案の答え |
| [p016](phase016/phase.md) | 見張りと非常の停止（signal の disposition、init の SIGUSR2・上書きの禁止、REBOOT・HALT の hook、SLEEP の排他と `kern_thermal_resume`、停止の道の flag、sync の助け、`drv_acpi_poweroff_emergency`、水準の検査、記録の file） | cleared（2026-10-31 Q1、af8aaa5f6・6de483e15・f857f4647、main a57216e81。host 試験 22 場面、QEMU で温度の上がる SSDT から ACPI S5 の電源断まで。init の ACK と注入の ioctl は p005 後半へ持ち越し。`thermal.emergency=off` は作らない（ユーザーに聞く）） | p005 |
| [p017](phase017/phase.md) | ACPI WMI の層（`_WDG` の解析の module と method・query の口）と AML の harness の `plan/tools/aml-host/` への移しと拡張 | cleared（2026-10-31 Q1、caff3950b〜93fb7eadd、main 809dfe329。WMI の試験 9 項目・SMI の書き 0・20 port の build・QEMU の起動。Q1 の判断: WMI の attach の試し直しは p018 の Dell の provider が自分の poll から行う） | p001 |
| [p021](phase021/phase.md) | passive（見張り → poll の thread → 受け手の `limit`、5 秒ごとに 100‰、3 °C 下で戻す） | cleared（2026-10-31 Q1、main a57216e81。host 試験。cpuperf の受け手との実機の結合は p014） | p006、p016 |
| [p019](phase019/phase.md) | sessiond の THERMAL の要求（strategy・boost、root か wheel） | canceled（2026-11-12 ユーザー: /dev/thermal は desktop の group が直に操作、sessiond は認証だけ） | p005（kernel への呼びは p004） |
| [p020](phase020/phase.md) | Pi 5 の provider（fan の level・rpm、受け手、boost） | cleared（2026-11-12 Q1 判定: Pi 5 の実機で fan の rpm・level、strategy で trip の組が変わる、boost で level が上がる、を確かめた。host 試験 19838 check。QEMU raspi4b は rpi4 の firmware の notice の欠けで未実施） | p005、p016 |
| [p018](phase018/phase.md) | Dell の provider（DDV の fan、BIOS の熱の mode の受け手、SMM の boost の確かめ、WMI の attach の試し直し）と 5330 の熱の試験 | cleared（2026-11-12 Q1 判定: 受け入れは満たした、boost は規則どおり無し） | p005、p017 |
| [p022](phase022/phase.md) | sysctl の道具と Keiland の monitor を `/dev/thermal` に、`hw.thermal` を外す | cleared（2026-11-12 Q1 判定: hw.thermal を外し、/sbin/thermal と monitor を /dev/thermal で QEMU で確かめた） | p005 後半、p013 |
| [p024](phase024/phase.md) | 規約の全文の見直しと文書（`docs/architecture/power-management.md`） | cleared（2026-11-13 Q1、P1 q2038。main に merge） | p005〜p022 |
| [p025](phase025/phase.md) | p014 の食い違いの解析と直し（HWP の読みの古さ・E-core の MHz の換算・manual の上限・E-core の domain・package の C-state 0%）。考える Phase、実機は最後に 1 回 | cleared（2026-11-13 Q1、P1 q2022・q2023。読み方と測りの thread の直し、E-core を module の domain に。package の C-state 0% は p027） | p014 |
| [p026](phase026/phase.md) | CPU の MHz を読み手の権限によらず新しい値で返す（`READ_SAMPLE` と書き込みの制限を外し、約 1 秒の共有の sample） | cleared（2026-11-13 Q1、P1 q2027。全 CPU を 100 ms で 2 回測る共有の sample を 1 秒ごと、READ_SAMPLE と書き込みの制限を外した。host 試験 3 本 PASS（直す前は FAIL）、x86_64・rpi5 warning 0、QEMU で群の外の利用者も読める。5330 の数は次の測定） | p025、BUG-422 |
| [p027](phase027/phase.md) | 5330 の残り: package の C-state 0% の原因（i915 の RC6・DC state の A/B）、i915 の qs40、S0 idle | cleared（2026-11-13 Q1。直しは p029 と共に main に merge。S0 idle は VMD の Bug の案） | p025、p028 |
| [p028](phase028/phase.md) | 5330 を人手なしで戻せる起動: Intel TCO watchdog の driver（止まれば再起動）と UEFI の一回だけの起動 | cleared（2026-11-13 Q1、P3 q2028。main に merge。5330 で人手なしに戻れる事を確かめた） | — |
| [p029](phase029/phase.md) | i915 の GT を RC6 に入れる: HW_CONTROL で DEVICE_LOST の原因、forcewake の参照の数の性能。考える Phase、実機は最後 | cleared（2026-11-13 Q1、P3 q2037。main に merge） | p027 |
- 2026-11-13 Q1: p029 の後の残り（範囲の外の手掛かり）: (1) amd64 を 100 Hz にしたので、ms で書いた待ち（例 i915 の `I915_WORKER_WAIT_MS` は 1 ms のつもりが 10 ms、c600 で 5 s に 190〜460 回の期限切れ）が 10 ms に丸まる。性能への影響は未確かめ。(2) i915 の単一の worker と GPU の job の 10 秒の期限の構造（待たされた時間も期限に数える）。(3) uncached の boot framebuffer の text console が 1 行 約 270 ms。(4) S0 idle は [BUG-424](../bugs/BUG-424.md)。
- 2026-11-14 Q1（WS302 の design-reviewer の指摘から）: 上の「amd64 を 100 Hz にした」は、その後の WS351 の可変の tick（68910dea12、走っている間 1000 Hz・idle の間 100 Hz）で古い。今の tick は WS351 の ws.md を正とする。
