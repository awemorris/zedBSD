<!-- awesome-plan project=zedbsd record=ws333-design-review -->

# WS333 設計の敵対的レビュー（design.md 第 1 版）

日付: 2026-10-31。担当: design-reviewer（P5 の依頼）。file への保存は P5 が行った（reviewer は本文を返すだけ。本文はそのまま写した）。反映は [design.md](design.md) §14。
対象: `plan/ws333/design.md`（第 1 版、commit 500061ba1）、`user-questions.md`、`ws.md`、`phase001/phase.md`、`phase002/phase.md`。
基点: worktree `/home/awe/zedBSD-worktrees/p5-ws333`（branch `agent/p5-ws333`、HEAD 500061ba1、base main aa112273a）。行番号はこの基点の物。
読んだ物: 上の対象の全文。`plan/ws301/{ws.md, design.md:1〜235}`、`plan/ws303/{ws.md, design-v5-review.md 全文, design.md の Q303-12 の行}`、`plan/ws302/ws.md`、`plan/ws331/{ws.md, phase001/phase.md の該当行}`、`plan/guardrail.md:1〜170`、`plan/master-design-policy.md:189・305〜330`、`docs/agent/spec-module-delegation.md:1〜80`。source: `include/hal/hal.h:130〜260・540〜590`、`src/hal/amd64/task.c:690〜712`、`src/hal/amd64/bsp-pcat/idle-suspend.c` 全文、`src/hal/amd64/bsp-pcat/lapic.c:245〜300`、`timecounter.c` の関数の一覧、`src/kern/sched.c:994〜1060・1550〜1600`、`src/kern/clock.c:80〜240`、`src/kern/sleep.c:1〜60・200〜245`、`include/kern/{sched.h:190〜290, types.h:21, sched_private.h:23, clock.h, platform.h:40〜80, boot.h:95〜290, lock.h:13〜57, pmem.h:70, sysctl.h:89〜99, cred.h:44}`、`src/kern/boot.c` の名前の表、`src/kern/platform/{pcat.c:686〜753, rpi5.c:321〜330}`、`src/drivers/generic/system-device.c:755〜820` と include、`src/drivers/acpi/acpi-event.c:1005〜1075`、`include/uapi/{sysctl.h:240〜313, system.h:278〜304・357〜368}`、`src/kern/sysctl.c:145〜240`、`userland/base/init/main.c` の signal、cdev の番号と ioctl の group の grep、`platform/amd64/vmunix.mk:478〜488`、`config/ci/config-intelmac.mk`。
**build・試験・QEMU は走らせていない。GPL の source は読んでいない。web は使っていない。`.internal/` は読んでいない。** 「推測」と書いた物は source・文書で確かめていない。

重大度: blocker（実装に入ると目的・安全の道を壊す、一意に読めない）、major（実装の前に設計を直す）、minor（実装の中で直せる・記録の整合）。

## 要点

- blocker 0、major 8、minor 24。
- hal.h を変えずに p003〜p016 を作れる、という結論は source と照らして崩れなかった（要る物は kernel の口の追加で、HAL の宣言ではない。m3）。
- 一番の問題は §4.3.3・§13・Q333-7 の「HAL の変更が要る」の分析が網羅していない事（M1）。ユーザーの規則（HAL を変える必要を主張する時は、既存の HAL と driver・kernel で足りない理由を具体的に示す）に対し、HAL の実装だけで出来る道（tick を CPU の間で揃える）と driver の MSR の道を見落とし、tick の停止で何が要るかの見立てにも CPU 0 の役目が抜けている。
- CPU の省電力を先にするという指示に対し、クリティカルパスがユーザーの UAPI の review で止まり（M3）、既定 n の `CONFIG_CPU_PERF` のため既定の image に P-state の改善が届く道が計画に無い（M4）。
- p002 に求める条件に「深さと 1 ms の tick の関係」が無く、条件が P1 に届く道も無い（M2）。

## Major

### M1. HAL の分析（§4.3.3・§6.2・§13・Q333-7）が網羅していない。HAL の実装だけで出来る道と driver の道を見落としている

- 根拠:
  1. tick は CPU ごとに、その CPU が起きた時刻に周期の mode で program される（`src/hal/amd64/bsp-pcat/lapic.c:252〜276` の `amd64_lapic_timer_start`。BSP が 1 回だけ較正し、各 CPU は自分の起動の時と S0 idle の後に `LAPIC_PERIODIC` で始める）。よって 12 CPU の tick の位相は揃っていない。設計の見立て (b)（§3:63「1000 Hz の tick が全 CPU で揃わずに来るため package の深い C-state に入る間が短い」）の原因は「揃っていない事」だが、§4.3.3 の表 A は「止める」しか検討していない。位相を揃える（共通の TSC の時刻の倍数に合わせて program する。例えば TSC-deadline の mode で周期を作る）のは `src/hal/` の実装の変更で、hal.h の契約（`hal.h:548〜556`「周期の tick」、`HAL_TIMER_FREQUENCY`）の中に収まり、guardrail の「実装の修正・補完・最適化は承認なしでよい」に当たる（推測: 揃えると全 core が同じ時刻に起き同じ時刻に休むので、package が深い状態に居る時間が tick の 1 周期のほぼ全部になりうる。効果の量は未確認）。
  2. 表 A の「止める」の分析に CPU 0 の役目が無い。`kernel_ticks` を進め process の timer を満了させるのは CPU 0 の tick だけ（`src/kern/clock.c:108〜114`）。idle の CPU の tick を止める口を足しても CPU 0 は 1000 Hz で起き続け、package の深い状態は CPU 0 の tick で切られる。要るのは「止める」口ではなく、次の期限で一度だけ鳴らす（one-shot）口と、CPU 0 の仕事の受け渡しになる。また tick が止まると `charge_cpu_time`（`sched.c:1045`、`hw.cputimes`。WS301 §4.9 の legacy の governor の使用率の源）と `kern_random_tick`（`clock.c:120`）も止まる。HAL の差分の形（口の大きさ）が §4.3.3 の書き方と違う。
  3. `hal_cpu_idle_suspend()` は hal.h を変えずに呼べ、契約の文言は「for a system suspend-to-idle」という用途の記述（`hal.h:213〜215`）。設計はこれを「契約の読み替え」として退ける（§13:536）が、測りの道具としては使える: 試験の build で CPU 0 以外の、時間つきの sleep の無い CPU（`sched.c:1025〜1039` の自分の sleep の列が空）の idle をこれに替えると、「tick を止めた時に何 W 下がるか」の上限を HAL を変えずに測れる（CPU 0 は残るので 2 の制限つき）。
  4. 表 B（idle の深さの上限）について: package の C-state の上限は model ごとの MSR（SDM Vol 4 の MSR_PKG_CST_CONFIG_CONTROL、0xE2 の下位の bit。BIOS の lock の bit が立つと書けない）で driver が設定できる見込み（推測: SDM Vol 4 の手元の確かめはしていない。5330 で lock されているかも未確認）。latency の要る場面の粗い道としては kernel が idle を poll にする（`hal_cpu_idle()` を呼ばない）道もある。「宣言か callback の追加が要る」と言い切る前に、これらで足りない理由が要る。
- 起こる状況: ユーザーは Q333-7 で「HAL の変更が要る」という前提の問いを受け、p009 は判断の数として「1 秒あたりの割り込みの数（tick）」（§4.3.3:165）を測る。これは作りから 12000/s に決まっており判断の材料にならない。p017 で HAL の差分の案が出ても、これまでユーザーが退けてきた「粗い HAL の提案」と同じ形になる。
- 直し方の案: (1) §4.3.3 の表を「HAL の実装だけで出来る事（tick の位相を揃える、TSC-deadline）／ driver で出来る事（0xE2 の上限、p006 で SDM と 5330 の lock を確かめる）／ kernel だけで出来る事（poll の idle）／ それでも足りない事（one-shot の tick と CPU 0 の仕事の受け渡し）」の 4 段に書き直す。(2) tick の位相を揃える事を p002 の続きか独立の小さな HAL の実装の Phase（承認不要）として §9 に入れ、p009 の前に置く。(3) p009 の判断の数を、tick を揃える前後・試験の build での `hal_cpu_idle_suspend()` の流用の前後の package の電力と PC6 以深の滞在の率に替える。(4) Q333-7 の題と背景から「HAL の変更が要る」の断定を外す（m16）。

### M2. p002 に求める条件（§4.3.1）に、深さと tick の周期の関係が無い。条件が P1 に届く道も無い

- 根拠: p002 の範囲は「最深の C-state の hint」（`phase002/phase.md` の範囲、design.md:138）。設計の条件 1〜4（:140〜143）は counter・ARAT・notify・hypervisor だけ。深い状態の入りと出の時間（§12.3 で未確認）が 1 ms の tick の周期に近いか越えると、tick ごとに入って出る分の電力と、割り込みごとの起きの遅れ（音・入力の latency）が出る。設計は「WS333 の後の Phase は idle を書かないので p002 と独立」（:144、§4.15:330）と言うが、tick を揃える（M1）か否かで適する深さが変わるので独立ではない。条件 1〜4 は design.md にあるだけで、P1 の `phase002/phase.md` に無い。
- 起こる状況: p002 が C10 の hint を常に使い、tick を止めないまま 1000 Hz で C10 と C0 を行き来する。電力が期待ほど下がらない（推測: hardware の自動の降格がどこまで効くかは未確認）か、深い状態の出の遅れで音や入力の応答が悪くなる。p002 の前後の比べで「MWAIT は効かない」と誤った結論になりうる。hypervisor の条件 4 が P1 に伝わらないと QEMU の回帰を壊しうる。
- 直し方の案: 条件 5「入る状態は、その状態の出の時間と目安の滞在の時間が tick の周期より十分短い物から選ぶ。深い状態の価値は p002 の測りで確かめる（hint ごとの package の電力と PC の滞在の率）」を足す。条件 1〜5 を Q1 経由で P1 に今すぐ渡し、p002 の phase.md に写してもらう。p002 が `idle-suspend.c` に触るなら、その file の開発の管理情報の comment（`idle-suspend.c:9` の「ws052-p006, HAL v2 H1v2」）を guardrail に従い消す事も条件に入れる（m13）。

### M3. CPU の省電力のクリティカルパスが、ユーザーの UAPI の review で止まる

- 根拠: §9:456「クリティカルパスは p003 → p005 → p008 → p009」。p003 の受け入れは「ユーザーの review の答え」（:462）で、p004・p005 とも p003 に依る。p005 の中身（topology・domain・要求の箱と notify の道・provider の口・受け手の登録・上限）の大半は kernel の中の口で、UAPI の review を要らない。CPU の ioctl だけが UAPI。
- 起こる状況: ユーザーが UAPI の review をすぐ返せないと、2026-10-31 の「CPU の省電力を先に」に反して P-state の実装の全部が止まる。decode の module（p006・p007）を並べても、p008 は p005 に依るので短くならない。
- 直し方の案: p003 を p003k（`include/kern/thermal.h`・`include/kern/cpuperf.h` の kernel の口、review は design-reviewer だけ）と p003u（`include/uapi/thermal.h` の全文、ユーザーの review）に分ける。p004 の core の部分・p005・p008 は p003k だけに依り、CPU の ioctl と `/sbin/thermal` の CPU の表示は p003u の後の小さな Phase に移す。p009 の受け入れ（strategy を選ぶ）は kernel の中の試験の口か p003u の後かを明記する。

### M4. 既定 n の `CONFIG_CPU_PERF` を y にする道が計画に無い。ユーザーの優先が既定の image に届かない

- 根拠: §4.1:85「`CONFIG_CPU_PERF`（既定 n、Q301-4）は MSR を書く・読む道だけを入れる」。Q301-4 の答えは、試験が通るまで無効にするという趣旨（`plan/ws301/ws.md` の 2026-10-29 の回答）で、「通ったら既定を決め直す」の時と担当が §9 に無い。RAPL の meter・C-state の滞在の統計（§4.6・§4.3.2）も y の時だけ。user-questions の Q333-1 は「B・C なら … `CONFIG_CPU_PERF=y` の kernel の時」と書くが、既定をいつ変えるかは聞いていない。
- 起こる状況: p009 で 5330 が合格しても、ユーザーが使う既定の image（`config-amd64.mk`）は HWP を使わず、P-state の改善（§3 の見立て (c)）と電力の読みが届かない。WS の受け入れの柱 (2)（strategy で CPU の要求が変わる）が既定の build では成り立たない。
- 直し方の案: p009 の後に「既定を y にするか」をユーザーに問う点（Q333-8 か p009 の受け入れの後の問い）を §9・§11 に置き、y にする config の変更を小さな Phase にする（`config/ci/config-amd64-server.mk` はユーザーの物なので触らない。m23）。危険の小さい部分（CPUID だけで決まる HWP の architectural な MSR）と model ごとの MSR（allow list）を別の config に分け、前者だけ先に既定 y にする案も比べる。

### M5. WS302 に渡す口が、既定の構成と最初の SET の前は core の種類しか返さない

- 根拠: §4.5:200「相対の性能（FREQUENCY・FEEDBACK・PROVIDER）は MSR を使うので `CONFIG_CPU_PERF=y` の時だけ。n の時は UNKNOWN」。さらに FREQUENCY の基準の Highest の MHz は「provider が enable した後（§4.4 の最初の要求の時）」に測る（WS301 design §4.7、設計は WS301 §4.7 のまま引き継ぐ）。起動の時は HWP を enable しない（§4.4:194）。
- 起こる状況: 既定 n では WS302 の DTB は P/E の相対の性能も HFI も得ず、core の種類だけで並べる。y でも Q333-1 が A なら最初の SET まで UNKNOWN。WS302 の p001 が「相対の性能がある」前提で設計すると既定の image で働かない。WS302 の ws.md（:81〜84）は今も「WS301 p001・p002・p004」に依ると書き、WS333 の Phase への写しが無い。
- 直し方の案: §4.5 に構成ごとに WS302 が得る物の表（n／y で SET の前／y で SET の後）を書く。y の時は相対の性能の測りを probe の時に（HWP を enable せずに出来る範囲で。APERF/MPERF は CPUID.06H:ECX[0] で architectural）行えるかを p006 で確かめる。WS302 の依存を WS333 の Phase（p001 → p003k、p002 → p005、p004 → p008）に直すよう Q1 に頼む。

### M6. `EMERGENCY_ACK` の権限が決まっていない（node は 0666）

- 根拠: §4.10:251「`/dev/thermal`（0x00010008、node 0666）」、:263「EMERGENCY_ACK | init が非常の停止を始めた印」。権限は「GET は誰でも、SET は root」（:253）で、ACK はどちらとも書いていない。§4.11:280「kernel は SIGUSR2 から 10 秒で ACK が無ければ 4 へ、SIGUSR2 から 120 秒で電源が落ちていなければ 4 へ」。
- 起こる状況: init 以外の process（誤った道具か悪意の process）が ACK を呼ぶと、init が停止を始めていなくても 10 秒の門が外れ、直の切りが 120 秒後に延びる。非常の水準の機械で 110 秒余計に熱い。ACK が SIGUSR2 の前に来た時の扱いも無い。
- 直し方の案: ACK は pid 1 だけ（`system-device.c:775・787・804` の HALT・POWEROFF・REBOOT と同じ判定）、かつ状態が SIGNALED の時だけ受け、他は EPERM・EINVAL。host 試験に「pid 1 でない ACK」「SIGNALED の前の ACK」を足す。

### M7. 受け入れの条件が落ちえない・測れない

- 根拠:
  1. §1:31 の柱 (1)「idle の package の電力が p002 の前より下がり、… 和らぐかを測る」: 下がる量の基準が無く、後半は測るだけ。p009 の受け入れは「script の PASS、数の記録」（:472）で PASS の定義が無い。
  2. §8.4:446「p002 の前・p002 の後・p008 の後 … idle の package の電力（meter）」: meter は p004（core）と p008（RAPL の provider）で入り、その時には p002 は main に入っている。HAL の MWAIT を止める switch は p002 の範囲に無い（`phase002/phase.md`）。よって meter で「p002 の前」は測れない。p002 自身の前後の数は WS291 の診断の patch（MCHBAR の写し、§3:61）で測った物で、器具が違う。
  3. §4.3.3:165 の「1 秒あたりの割り込みの数（tick）」は作りから決まった数（M1）。
- 起こる状況: どんな結果でも p009 が cleared になり、WS の受け入れが「測った」で終わる。Q333-7 の判断の材料が出ない。
- 直し方の案: 柱 (1) を「測るだけ」と明記するか、数の基準（例: Linux の値との差の何割を埋める、は Q1 とユーザーに）を置く。「p002 の前」は p002 の数（器具を明記）を引用するか、試験の build で HLT に戻す（HAL の実装の中の build の macro）。tick の数の代わりに M1 の (3) の比べを入れる。

### M8. 標準廉価実装手順の適用（p006・p007a〜e）が手順の §1 の条件に合わない部分を含む

- 根拠: 手順の §1 の 1「作る物の振る舞いが外部の標準仕様で決まっている」（`docs/agent/spec-module-delegation.md`）。p007b の「strategy の表と MANUAL の式、efficient より下にしない」（:467）は WS301 §4.4 の zedBSD の方針で、SDM が決める物ではない。p007d の「governor の判断の関数（使用率 → 段）」（:469、WS301 §4.9 の 80%・30% は「初期値、p007 で直す」）も方針。p007a の「5330 の表の試験」は観測の値。C-state の滞在・RAPL・0xCE は model ごとの MSR（SDM Vol 4）で、ADL が表にあるかは未確認（§12.2）のまま p007e を立てている。
- 起こる状況: Sonnet の実装の担当が、仕様書に無い方針の式を header の comment だけから書き、手順の「逆方向の独立の実装」の試験は方針の誤りを見つけない。p008 の 5330 の測りで値を直す時、header（基点の SHA で固定）を変えられず手順と食い違う。Vol 4 に ADL が無ければ p007e の半分が要らない。
- 直し方の案: p007 を SDM・APM・ACPI の decode・encode（CPUID の leaf の解析、HWP の CAPABILITIES・REQUEST の field、HFI の表の形、RAPL の単位と一周、`_PSS` の package の形、CPUID.05H）に限る。strategy の写し・MANUAL の式・governor は p005 か p008（Opus）に移す。p006 の最初に Vol 4 の ADL の表を確かめ、無い MSR の module は立てない。小さな p007c・p007e はまとめてよい（起動の回数と手順の費用を減らす）。

## Minor

- **m1** §2.2:46 の `kern_cpu_id_t` は `include/kern/sched.h` でなく `include/kern/types.h:21`（`typedef uint32_t`）。`kern_cpu_current()` の戻りは `unsigned`（`sched.h:202`）で `kern_cpu_id_t` ではない。provider の口の型（§4.4）と揃えて書く。
- **m2** §12.7 は今確かめられる: `kern_platform_halt` は `include/kern/platform.h:46` にあり、`src/kern/platform/` の 12 file 全部が定義する（`kern_platform_poweroff` も 12）。§4.11:281 の「他の 9 platform」は 10（12 から pcat・rpi5 を除く。`hal-console.c` は 4 つの board を束ねる）。既定は weak の関数 1 つにすれば数に依らない。
- **m3** guardrail「driver は HAL を知らない」で driver が要る kernel の口の一覧が無い。`kern_rtc_read_counter`（`clock.h:169`）と `kern_pmem_alloc`（`pmem.h:70`、HFI の page）はある。CPU の数と最大（`kern_cpu_count`・`KERN_CPU_MAX` の類）は `include/kern/` に無い（grep で 0）。provider の CPU ごとの表の大きさに要る。p005 の範囲に「kernel の口の追加（CPU の数・最大）」を書く（HAL は変えない）。
- **m4** 起動の parameter の key: §4.11:275 は `thermal.emergency=off` だけを `include/kern/boot.h:107〜129` の固定の enum に足すと書くが、`cpuperf=off`（§4.15:328）と `cpuperf.hwp=off`（§8.4:449）も同じく enum と `src/kern/boot.c` の名前の表（`PARAMETER_NAME`）に要る。どの Phase で足すか（p008）を書く。
- **m5** lock の記述の食い違いと抜け: §4.2:130 は apply の中で `kern_thermal_event` を呼んでよいとし、§4.12:294 は「受け手の lock を持たずに」とする。事象の ring の lock の rank と写しの spinlock との順が無い。sensor の push の道（acpi-thermal の AML の lock・rpi5-fan の lock → 写しの spinlock）が §4.12 に無い。meter の 64 bit の積み（worker）と GET_METER の読み（ioctl）が同じ値を更新・読むので lock か seq が要る。
- **m6** worker 1 本に、眠る受け手（Dell の窓、cpu の 1 秒の確かめ、AML・SMI）、WMI の attach の試し直し、fan の 5 秒の読み、passive、meter の 60 秒、boost が乗る（§4.12:300）。受け手が眠る間、fan の STALLED の「3 周期」と passive の 5 秒が延びる。v5 m10 の後半（「worker 1 本で SMI と DDV の止まりが passive・strategy を待たせる事も書く」）は §8.3 で m10 → §4.7 とされるが §4.7 は前半だけ。HFI の 1 秒の読み（WS301 §4.7）を走らせる thread が §4.12 の一覧に無い。
- **m7** §4.11:276 の「resume の時（`sleep.c` の終わりの通知）」: `sleep_post`（`sleep.c:201・508`）は `/dev/system` の事象で、kernel の中の callback ではない。見張りが古さの基準を置き直す hook を `sleep.c` に足す（kernel の変更、WS052 の file）事を p010 の範囲に書く。また「凍らせた後は全 CPU が suspend しているので見張りは走らず」は不正確: user を凍らせた後（`sleep.c:174`）も device の suspend の間は kernel の thread が走る。その間の非常は SIGUSR2 が凍った init に届かず 10 秒の門で直の切りになる、と状態遷移に書く。
- **m8** 120 秒の門（:280）の直の切りが、init が既に呼んでいる `KERN_SYSTEM_POWEROFF`（`system-device.c:785〜800` → `system_shutdown_prepare`）と同時に走りうる。sync の助けの thread と shutdown の準備が並ぶ。一度だけの「停止の道に入った」の flag を置き、init の POWEROFF が進行中なら見張りはそれを待つ（上限つき）と書く。
- **m9** §4.8:239 の SET の戻り: 全部の受け手が EOPNOTSUPP（n の kernel の cpu だけの機械、QEMU）の時の戻りが一意でない（「受け手 0 → EOPNOTSUPP」と「EOPNOTSUPP は失敗に数えない」の組）。0 と「全部が対応外」の flag か EOPNOTSUPP かを決める。§8.4 の QEMU の受け入れがこれに依る。
- **m10** 名前のぶつかり: UAPI の struct の接頭辞 `thermal_*`（§4.10:252）は、残っている `struct thermal_header`・`struct thermal_entry`（`include/uapi/sysctl.h:290・299`）と p004〜p016 の間に並ぶ。p004 で `sysctl.c` が両方の header を読むので同じ名前を使わないと決める。`KERN_THERMAL_*` は kernel の header（`KERN_THERMAL_APPLY_UNCONFIRMED`・`KERN_THERMAL_DOMAIN_ALL`・`KERN_THERMAL_STRATEGY_*`、§4.2）と UAPI の両方で使うので、共有の定数は UAPI で 1 回だけ定義し kernel の header は include する。
- **m11** topology の扱い: §4.5:199「`CONFIG_CPU_PERF` に依らず常に出す」と §4.14:312「ABSENT（hypervisor）」が食い違う（hypervisor の下でも CPUID だけの topology を出すか）。CPUID.1AH・1FH は論理 CPU ごとに、その CPU の上で実行する必要があり、n でも notify の道（p005）が要る。§4.4 の ops の `topology` の comment に「その CPU の上で」を書く。
- **m12** §4.3.2:149 は package の MSR を「CPU 0 で読む」、§4.4:184 は「package の任意の CPU」。片方に揃える（複数の package の機械では package ごと）。core の滞在の読み（GET_CPU_IDLE）は相手の CPU を notify で起こすので、Keiland の monitor（p016）が周期で読むと「周期の読みで CPU を起こさない」（WS301 M5）に反する。monitor は package の値だけを読む、と p016 に書く。
- **m13** 計画で手を入れる file に、source の中の開発の管理情報が残る: `src/kern/clock.c`（3 か所、p005 の notify の道）、`src/drivers/generic/system-device.c`（5、p010）、`src/kern/sleep.c`（1、p010）、`src/drivers/acpi/acpi-thermal.c`（1、p004）、`userland/desktop/libkeiland-backend-zedbsd/monitor-zedbsd.c`（6、p016）、`src/hal/amd64/bsp-pcat/idle-suspend.c`（1、p002）。guardrail「既存の物は、その file が作業の対象になった時に、その作業の中で削除する」に従い、各 Phase の範囲に書く（log の文字列を変えるなら照合する道具も追従させる）。
- **m14** 記録: ws.md の 2026-10-31 の 2 つの決定は「という趣旨の指示を受けた」で原文が無い（AGENTS.md §8「原文を引用し、解釈と分けて書く」）。queue.md:368・cockpit.md:132〜133 にも原文が無い。Q1 に原文を求めて ws.md に写す。ws.md の Phase の表は p001・p002 だけで、design §9:487 の「ws.md の表はこの表の写し」と食い違う（p003 以降を立てるまで「写しは未作成」と書く）。
- **m15** Q333-1 の推し B（`/etc` に保存し rc が渡す）を作る Phase が無い（`/sbin/thermal` の保存、rc の script、保存の file の形）。p004 か p013 の範囲に入れる。B の「Dell の mode が今と同じなら書かない」は今の mode を確かに読める事に依るが、v5 M8 で ODV0 は確かめにならず、成否は SET の時の sentinel だけ。起動の時の読みの道を書く。
- **m16** user-questions: (a) Q333-7 の題「HAL の変更が要る」と背景の 1・2 は M1 の見落としの上に立つ。HAL の実装だけの道（tick を揃える）と driver の道を書き、断定を外す。(b) Q333-7 の B「要らないかもしれない案になる」は推しに寄せた書き方。(c) Q333-7 が今決める事は「いつ決めるか」だけで、これは設計の担当か Q1 が決めてよい手順の事（AGENTS.md §2「範囲の中の普通の技術の判断は担当がしてよい」）。問いから外し、p009 の後に数とともに聞く、と知らせるだけにできる。(d) 用語: Q333-7 の「S0 idle」、Q333-2 の「WS300」（説明無し）。(e) 前置き:12「どれも推しで進めても後で直せる」は Q333-4（3 軸の向き）には当たらない。UAPI の意味として出した後は直しにくい。
- **m17** Phase の大きさ: p004（7 LW）は WS303 p003a（6 LW、v5 m23 で「少なめ」）に meter・世代の 2 周目・注入・試験の受け手を足した物、p005（5 LW）は WS301 p002（5）と p006（UAPI と道具、3）の大半。どちらも少なめ。§9 の LW の和は 62 で、:483 の「約 64」と合わない。Phase の ID の文字の添え字（p007a〜e）は ID の形 wsNNN-pNNN から外れる（WS303 p003a の前例はある）。directory の名前と Queue の ID の形を Q1 と決める。
- **m18** §8.3 で「着手の時に読む」と送った v5 の m5・m24・m18 は、行き先の phase.md（p011・p012）がまだ無く、§8.3 の表にしか残らない。ws.md の Resume point か p001 の phase.md の残りに列べる。
- **m19** Dell の SMM の値を GPL の file から取る件（§10:493）: guardrail は GPL の値の使用を WS141・WS037・WS214・WS039 に限り「他の WS の GPL の参照には適用しない」と書く。WS303 の Q303-12 の答えを WS333 に持ち越す事を、ws.md のユーザーの決定と guardrail の表に Q1 が写す。
- **m20** `thermal.emergency=off`（§4.11:275）は、ユーザーが求めた安全の機能（Q303-5、非常の水準で電源を落とすという趣旨）を止める switch。WS303 v5 M6 の案だが、ユーザーの決定ではない。少なくとも Q1 に確かめる点（§11.2）に入れ、off の時は起動のたびに log と GET_STATUS の印を出すと書く。
- **m21** WS301 design は自分の中で食い違っている: `plan/ws301/design.md:138`「`CONFIG_CPU_PERF=y` の kernel は起動の時に HWP を enable する」と §4.4「HWP も enable しない」・§4.8 の状態遷移（PROBED → HWP_ENABLED）。WS333 §4.4:194 は後者を採るが、「WS301 §4.1〜§4.12 を引き継ぐ」（:169）だけでは前者も引き継ぐ。WS301 :138 と §4.8 を置き換える、と明記する。
- **m22** §4.4:171「CPUID・RDMSR・WRMSR は driver の中の 1 つの小さな file の inline asm」は Q301-8 の答え（2026-10-29）に合う。ただ 2026-10-30 の guardrail は I/O を `kern_*` の口に寄せる書き方で、MSR が「I/O」に入るかは書いていない。根拠に Q301-8 を引き、2026-10-30 の規則と矛盾しない事を Q1 に確かめる（WS331 は HAL の header の `asm_inb` などを `kern_io_*` に替える方向、`plan/ws331/phase001/phase.md:169・196`）。
- **m23** §7:375 の「全 platform の既定の build と `CONFIG_CPU_PERF=y` の build」で config を足す時、`config/ci/config-amd64-server.mk` はユーザーの物で変えない（AGENTS.md §7）と書く。
- **m24** §4.3.3:162 の「深い C-state の出る時間（数百 µs の見込み）」と user-questions の C-state の用語の「起きるのに数百 µs かかる見込み」は根拠の無い数（§12.3 で未確認と認めている）。数を消すか「未確認」を本文にも付ける。

（reviewer の原文の M4・m20・m22 にあったユーザーの発言の引用は、2026-10-30 の記録の規則（発言の原文を載せない）に従い、写す時に要点に替えた。）

## §8.3（WS303 v5 review の行き先）の照合

- 解消と認めた物: M1（§4.8:239〜240、2 周目・適用済みの世代・SUPERSEDED・Dell の眠り・UNCONFIRMED）、M2（§4.2 の 1 つの ops、§8.1）、M3（§4.11:276、跳ぶ時計・置き直し・atomic の排他。ただし m7）、M4（ACK と 120 秒、上書きの禁止、REBOOT・HALT の hook、§8.4 の試験。ただし M6・m8）、M5（sync の thread と 10 秒、halt と印）、M6（水準の検査・off の switch・zone を持つ機械の確かめ。ただし m20）、M7（push を p004 に、注入を p004 に）、M8（§4.14 の sentinel、試験の 3 の前の確かめ）、M9（§11.2-1 で Q1 に）。m1〜m4・m7〜m9・m11〜m15・m17・m19〜m23・m25 は該当の節に入っている。
- 一部だけの物: m10（前半だけ、m6）、m5・m18・m24（「着手の時」に送っただけ、m18）、m15（数の誤り、m2）。

## 確かめて正しかった主張（要約）

- `hal_cpu_idle()` は `sti; hlt; cli` だけ（`src/hal/amd64/task.c:706〜712`）。hal.h の契約: idle（`hal.h:194〜197`）、idle_suspend（`:213〜232`、状態は HAL が選ぶ :219〜221）、notify は両方の待ちを終わらせる（`:154〜171`）、counter は HAL が入る全ての idle の状態で進む（`:569〜586`）、RTC の節は周期の tick だけ（`:548〜556`）。
- `idle-suspend.c`: LPIT の hint、MONITOR/MWAIT と 05H の拡張・割り込みで抜ける bit の確かめ、invariant TSC の確かめ、LAPIC の timer を止めて局所の源を静め、ECX bit 0 で MWAIT、`sti; nop; cli`。amd64 の counter は TSC だけなので、p002 の条件 1（C1 より深い状態は invariant TSC の時だけ）は正しい。
- `sched_idle` → `kern_sleep_idle` → `hal_cpu_idle`（`sched.c:1562〜1599`、`sleep.c:211〜244`）。時間つきの sleep の起床は各 CPU の自分の tick（`sched.c:1019〜1039`）。CPU 0 だけが `kernel_ticks` と process の timer（`clock.c:102〜121`）。S0 idle の時計の進め（`clock.c:130〜183`）。notify の handler（`clock.c:193〜203`）。`HAL_TIMER_FREQUENCY` 1000（`include/hal/arch/amd64.h:16`）。LAPIC の周期の timer（`lapic.c:252`）。
- `kern_cpu_current()`（`sched.h:202`）、`kern_cpu_id_t` と `hal_cpu_id_t` の同じ型の確かめ（`sched_private.h:23`）。
- `hw.thermal`（`include/uapi/sysctl.h:240〜313`、`HW_THERMAL 9`、24・64 byte）、registry（`sysctl.c:147〜240`、4 source × 16 sensor）、源（`acpi-thermal.c:38・112・252`、`rpi5-fan.c:330`）、読み手 2 つ（`sysctl/main.c:99・226・749〜803`、`monitor-zedbsd.c:643〜681`）。
- cdev の 0x00010008 は空き（使用は 0x00010000〜0x00010007・0x00010010〜・0x00010020 と別の major）。ioctl の group 'T' は `include/uapi/` で空き。`struct system_event` は 128 byte（`system.h:357〜368`）。
- init の signal は SIGHUP・SIGINT・SIGTERM（`init/main.c:293〜295`）。`mount_sync_all`（`mount.c:946`）。`drv_acpi_poweroff` は `_PTS` を評価してから event の lock を取り、S5 の値は起動の時に覚えている（`acpi-event.c:1011〜1064`）。HALT・POWEROFF・REBOOT は pid 1 だけ（`system-device.c:773〜812`）。`kern_platform_poweroff` の契約（`platform.h:51〜59`）。rpi5 は `hal_poweroff`（`rpi5.c:321〜330`）。起動の parameter は固定の enum（`boot.h:107〜129`）。
- driver の inline asm の前例（`i915/device.c:2265` の CPUID、`pcat-ide.c:218` の outb）。amd64 の pcat の driver の build（`platform/amd64/vmunix.mk:483`、Intel Mac は amd64・pcat）。i386 は性能を考えない対象（`master-design-policy.md` §11.1）。
- p003〜p016 に hal.h の差分を要る物は見つからなかった（notify・count・current・counter・poweroff は既存の口。足す物は kernel の口: m3・m4・m7）。

## 確かめられなかった事（推測のまま）

- tick の位相を揃えた時と止めた時の電力の差、5330 の深い C-state の出の時間と目安の滞在の時間、hardware の自動の降格の効き（M1・M2）。
- SDM Vol 4 の 0xE2 の field と 5330 の BIOS の lock、ADL の Vol 4 の表の有無（M1・M8）。
- `/var/log` を init が書ける時点か、devfs の mode の表の場所（読んでいない）。
- `include/kern/lock.h` の同じ rank の入れ子の規則（設計も p004・p005 に送っている）。
