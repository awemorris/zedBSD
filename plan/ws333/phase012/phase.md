<!-- awesome-plan project=zedbsd record=ws333-p012 -->

# ws333-p012: tick の位相を揃える（amd64 の HAL の実装）

Status: in-progress（実装と QEMU の確かめは済み。受け入れの 5330 の前後の数が未実施）
Parent: [WS333](../ws.md)
Queue: q1768-i01（P7）

## 目的

design.md §9 の p012 と §4.3.3 の段 1: 全 CPU の LAPIC の tick を共通の TSC の時刻に揃え、全 core が同じ時刻に起き同じ時刻に休むようにして、package が深い C-state に居られる間を長くする。今は各 CPU が自分の起動の時刻から周期の timer を始めるので、tick の位相が CPU ごとにずれる（`lapic.c` の `amd64_lapic_timer_start()`）。hal.h は変えない（契約は周期の tick と `HAL_TIMER_FREQUENCY` だけで位相を決めていない、hal.h:546〜556）。

## 判断

- 2026-10-31 ユーザーから、昼頃まで不在の間は実装と試験を進める、という趣旨の指示を受けた（Q1 経由）。
- **方式は (b) TSC-deadline の mode**（CPUID.01H:ECX[24]）。各 CPU は tick ごとに次の期限を「今の TSC より後の、周期の倍数」に書く。
  - (a)（各 CPU が周期の timer を始める時刻を TSC の周期の倍数に合わせる）を採らない理由: LAPIC の timer の 1 周期は calibration の整数の count（5330 なら 38.4 MHz / 16 で約 2400）で、TSC で見た周期との差（最大で 1 count、約 0.04%）が CPU の起動の時刻の差の分だけ積もる。AP が BSP の 1 秒後に始まると約 0.4 周期ずれ、system sleep の後の再開でもずれる。
  - (b) は期限が全 CPU で同じ TSC の倍数なので、起動の時刻・遅れた tick・再開に関係なく位相が揃い、ずれが積もらない。tick の rate は TSC の rate（timecounter の候補の値）に従う。
- 使う条件（どれかが欠けたら今の periodic）: build の macro `LAPIC_TICK_TSC_DEADLINE`（既定 1）、TSC が不変（CPUID 0x80000007 EDX bit 8、`amd64_timecounter_invariant()`）、TSC の rate が分かっている（`amd64_timecounter_bsp_candidate_frequency()`、CPUID 0x15 か PIT の測り）、その CPU に TSC-deadline の mode がある（CPU ごとに確かめる）。
- **今の形に戻す build の macro**: `-DLAPIC_TICK_TSC_DEADLINE=0`（全 CPU を periodic に、今までと同じ書き方）。`lapic.c` をこの値で compile できることを確かめた（下）。
- 起動の行: `A64 TIMER TICK source=tsc-deadline period=<TSC の count>` か `A64 TIMER TICK source=periodic`（計測の前後でどちらで走ったかを分かるように）。

## 成果（branch agent/p7-ws333b）

- `src/hal/amd64/bsp-pcat/lapic.c`: `choose_tick_source()`（BSP の calibration の後に 1 回）、`cpu_has_tsc_deadline()`、`arm_next_deadline()`（`next = (now / period + 1) * period` を `IA32_TSC_DEADLINE`（0x6E0）へ）、`amd64_lapic_timer_tick()`（tick ごとの再 arm、periodic の CPU は何もしない）、`amd64_lapic_timer_start()` は CPU ごとに mode を選ぶ。`amd64_lapic_timer_stop()` は CPU の印を外す（LVT を他の mode に書くと期限も外れる、SDM の規定）。
- `src/hal/amd64/bsp-pcat/lapic.h`: `amd64_lapic_timer_tick()` の宣言（HAL の実装の中の header。hal.h は不変）。
- `src/hal/amd64/bsp-pcat/clock.c`: `clock_handler()`（全 CPU の tick で呼ばれる）の最初で `amd64_lapic_timer_tick()`。
- 開発の管理情報の comment を消した（design.md §372）: `lapic.c` の BUG-094・BUG-303・ws052-p006、`lapic.h` の ws052-p006（comment だけ。log の文字列は変えていない）。
- 試験の道具: [tests/tick-phase/tick-rate.sh](../tests/tick-phase/tick-rate.sh)（QEMU+KVM で起動し、HMP で `kernel_ticks` を 10 秒の間を置いて 2 回読み tick の rate を出し、CPU 0〜3 の LVTT の mode を数える。console は読まない）。

## 確かめ（2026-10-31、P7）

host の / の空きが少ないため build・image・QEMU は /tmp（`/tmp/p7-ws333b/`）。image は `plan/tools/boot-images/config-amd64-boot.mk`（text の login だけ）。libc が openssl を要るので openssl の package だけは worktree の `build/packages/` で build した（Q1 の許可）。

| 確かめ | コマンド | 結果 |
| --- | --- | --- |
| vmunix の build | `make -j24 ZEDBSD_CONFIG=plan/tools/boot-images/config-amd64-boot.mk BUILD=/tmp/p7-ws333b/amd64 /tmp/p7-ws333b/amd64/vmunix` | rc 0、`warning:` 0（`-Werror` の下） |
| 戻す macro | 同じ flag の `lapic.c` に `-DLAPIC_TICK_TSC_DEADLINE=0` を足して compile | rc 0（warning 0） |
| image | 同じ config・BUILD で `disk-image`（flock） | rc 0。kernel・HAL の warning 0（`warning:` は openssl の package の 2 件と perl の locale の行だけ） |
| 起動（今の道、fallback） | `plan/tools/boot-test.sh /tmp/p7-ws333b/amd64/hdd-image.img`（uefi-nvme、KVM `-cpu host`、SMP 4）。QEMU は monitor を足す wrapper（`QEMU=`）で LVTT を見た | PASS（画面に `login:`、`/tmp/p7-ws333b/boot3-fallback/login.png`）。KVM の `-cpu host` は不変の TSC の bit を隠すので 4 CPU とも LVTT `periodic` |
| 起動（新しい道） | 同じで `-cpu host,+invtsc` | PASS（`/tmp/p7-ws333b/boot3-invtsc/login.png`）。4 CPU とも LVTT `tsc-deadline` |
| tick の rate | `plan/ws333/tests/tick-phase/tick-rate.sh OUT host IMAGE`・`OUT host,+invtsc IMAGE` | periodic 997.7 ticks/s（4 CPU periodic）、TSC-deadline 995.8 ticks/s（4 CPU tsc-deadline）。どちらも 1000 Hz（差は HMP の読みの時刻の誤差の内、10 秒で数十 ms） |

## 未実施

- **5330 の前後の package の電力と PC（package の C-state）の滞在の率**: 5330 が止まっているので未実施（後で）。比べる物は同じ image で kernel だけ替えた 2 つ（既定と `-DLAPIC_TICK_TSC_DEADLINE=0`）。器具は p002 と同じ診断の patch か、p011 の後なら meter（design.md §9）。方式の採否はこの数で決める（下がらなくても記録する）。
- tick の位相が実際に揃っていることの直の観察（QEMU の HMP では `IA32_TSC_DEADLINE` を読めない）。5330 の PC の滞在の率で間接に見る。
- 全 CPU が同じ時刻に tick を処理することによる lock の取り合い・遅れ（design.md の危険の表）: 5330 の計測で見る。悪ければ macro で戻す。
- QEMU の TCG（`-cpu max`）の道。
- 古い Intel（Haswell〜Kaby Lake 世代）の一部の microcode に TSC-deadline の timer の erratum があるとされる（未確認、spec update を読んでいない）。今の code は model・microcode で除外していない。対象の機械（5320・5330・13400F）はこれより新しい。古い機械で tick が止まる・早まる時はこの点を疑い、除外の表を足す（出典を確かめてから）。

## 残り

- 5330 の計測（上）の後に cleared の判定（Q1）。
