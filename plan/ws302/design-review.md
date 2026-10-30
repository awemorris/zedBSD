<!-- awesome-plan project=zedbsd record=ws302-design-review -->

# WS302 DTB 設計（p001 第 1 版）の敵対的レビュー

- 日付: 2026-10-31
- 対象: [design.md](design.md) 第 1 版、[proposed/include/kern/sched-hint.h](proposed/include/kern/sched-hint.h)、[proposed/include/kern/dtb.h](proposed/include/kern/dtb.h)、[user-questions.md](user-questions.md)（文脈は [ws.md](ws.md)）
- 基点: branch `agent/p5-ws302` の f2d6ca7b4（main 3d820d9ca の上の設計の commit 1 つ）。source の file:行 は 3d820d9ca と同じ物。
- 方法: 設計の主張を source と照らした（sched.c・sched.h・thread.c・process.c・syscall.c・signal.c・clock.c・hal/amd64/task.c・irq.c・hal.h・cpuperf.h・cpu-x86.c・input.c・gpu.c・lock.h・boot.c・Makefile・platform/*/vmunix.mk・plan/ws333/design.md）。build・QEMU は走らせていない。「推測」と書いた所は確かめていない。

## Blocker

### B1 user の thread を 6・7 に上げると、ほとんど全部の kernel の thread（8）より上になり、厳密な priority の下で飢えさせる

- 根拠:
  - `kthread_create` の呼び出し 97 か所のうち 90 か所が `SCHED_PRIORITY_DEFAULT`（8）。例: HID の report を decode する `usb_hid_worker`（src/drivers/usb/usb-hid.c:1378）、xHCI の port・isoch の worker（src/drivers/pci/pci-xhci.c:3512・:9028）、GPU の `gpu_monitor`（src/drivers/gpu/gpu.c:7502）、`pageout_worker`（src/kern/vm/pageout.c:214）。8 より上は `USB_BT_SCO_PRIORITY 4`（usb-bt.c:101）など少数だけ。
  - 取り出しは厳密に上の段から（sched.c:2199〜2211）。起床した thread は走っている thread より下の段なら `run[]` の後ろに入り横取りを頼まない（sched.c:2066〜2075）。quantum の終わりの `sched_yield` は同じ段の後ろへ戻すだけ（sched.c:776）で、上の段が忙しい限り下の段は回らない。他の CPU に移るのは idle の CPU の盗みだけ（sched.c:1577〜1607）で、全部の CPU が忙しい時（build と GPU の app、§4.8 の表の 5 行目）は盗みが起きない。
- 設計の誤り: §4.7 は「kernel の thread（`USB_BT_SCO_PRIORITY 4` など 0〜5）より上には user の thread を置かない（6 を上限）」と書き、user-questions.md Q302-3 も「kernel の thread の段（0〜5）には入らない」と説明しているが、事実は逆で、6・7 は kernel の thread のほぼ全部より上。
- 起こる状況: GPU を使う app（R5、7）が syscall をしながら CPU を使い切る（game の main loop、fence を ioctl で poll する render の thread。R3 の BUSY にならない、§4.4.3 の注）と、同じ CPU の HID の worker・xHCI の worker・gpu_monitor・pageout と、nice 0 の user の thread（terminal の shell、R6 の 8）が走れない。飢えの防ぎ（§4.7）の範囲が「`base_priority` が 8 より下の段と、今走っている thread より下の段」と曖昧で、
  (a) 下げた段（9〜15）だけなら 8 の kernel の thread は永久に待つ（入力が止まる、pageout が止まる）。
  (b) 走っている thread より下の全部の段なら、8 の HID の worker は 100 ms ごとに 1 quantum しか走れず、入力の遅れが 100 ms 単位で増える。受け入れ A1（入力 → 表示）の目的と逆で、ユーザーの「UI の応答を落とさない」に反する。
  R1（nice < 0 の BUSY）も 6〜7 で P に置くので同じ事が起きる。
- 直し方の案: user の段を kernel の既定より上に出さない写し方にする。例えば DTB が on の時、user の既定を 10、GPU の app 9、画面を作る process 8（kernel の既定と同じ）、nice < 0 は 9〜8、nice > 0 は 11〜12 とし、飢えの防ぎは「走っている thread より下の全部の user の段」に当てる（kernel の thread には当てない）。代価（kernel の忙しい thread が user を厳密に先に走る）を数で確かめる。どの写し方でも、§4.7 と Q302-3 の「kernel の段は 0〜5」という説明を事実に直し、ユーザーに「user の thread を kernel の worker と同じ段・上の段に置くか」を問いとして出す。

### B2 `sched_set_cpu` を外から任意の thread に使う前提が成り立たない（NEW の thread で queue が壊れ、刈り取り中の thread で解放済みの task を触る）

- 根拠:
  - `sched_set_cpu` の状態の判定は lock の外（sched.c:1704〜1710）で、断るのは RUNNING・ZOMBIE・DEAD だけ。NEW・EXITING・REAPING は通る。lock の中の再判定は `sched.cpu` と `need_migrate` だけ（sched.c:1721）。今は sched.c の外に呼び手が無いので表に出ていない（design.md §2.1 自身が書く事実）。
  - `thread_wait` は ZOMBIE を REAPING にした後、sched の lock を取らずに `hal_task_destroy(thread->task)` で task を解放し、`thread->task` を NULL にしない（thread.c:581〜589）。`thread_ref` は struct thread を残すが task は残さない（thread.c:617〜643）。
  - `sched_add` は `thread->sched.cpu` を 1 回読んで lock を取り、読み直さない（sched.c:463〜469）。NEW の thread は `attach_thread` で process の thread の列に入った後、`thread_start` までの間 NEW のまま列に見える（thread.c:647 以降）。
- 起こる状況:
  1. DTB が thread を `thread_ref` で取った後（§4.9 の 3）、その thread が別の CPU で exit し、親が `thread_wait` で刈る。DTB の `sched_hint_place` → `sched_set_cpu` が REAPING の thread の解放済みの task に `hal_task_transfer` する（解放後の使用。amd64 の `hal_task_transfer` は task.c:850 で `run_cpu` を読み、:864 で `target_cpu` に書く）。
  2. fork・thread の作成の直後、NEW の thread に DTB が `sched_hint_place`（PLACE の process の新しい thread、§4.6.4 で hint を継ぐ）。`sched_set_cpu` が `sched.cpu` を移し終える前に作り手の `sched_add` が古い CPU の `run[]` に入れると、thread は古い CPU の queue に居るのに `sched.cpu` は新しい CPU を指す。後の `queue_remove_thread`（sched.c:2178）が別の CPU の queue から外そうとして queue が壊れる（"scheduler queue underflow" の fatal か列の破壊）。
  3. EXITING（`cpu->retired` に置かれた thread、sched.c:973・:990）を移すと、退役の途中の task の `target_cpu` と `sched.cpu` を書き換える（推測: 害は出にくいが、意図しない状態）。
- 直し方の案: `sched_hint_place` と `sched_set_cpu` で、CPU の lock を取った後に `state` が RUNNABLE か SLEEPING である事を確かめ、それ以外は EINVAL（または EAGAIN）。RUNNABLE・SLEEPING の thread は自分では走れないので、印（MIGRATING）を立てた後は exit も刈り取りも起きない。`sched_hint_set` の EINVAL の対象にも NEW・EXITING・REAPING を明記する（header の comment は "idle, exiting or dead"、design §4.5 は "ZOMBIE・DEAD・idle" で食い違う）。§4.5 の「既存の `sched_set_cpu` を使うのでそのまま」は外部の呼び手を想定した確かめが無いので書き直す。試験に「thread の作成と exit を回しながら `dtb.stress=1`」を足す（m9）。

## Major

### M1 手渡しの席 `cpu->handoff` が 1 つで、新しい task の後継と通知の道の組み合わせで上書きされ thread が消える

- 根拠: 新しい task の入口から始まる後継は `complete_retired` を呼ばない（sched.c:959〜964 の comment）。設計は「その時の手渡しは次の tick の `complete_handoff` が片付ける」（§4.6.5 末尾）とするが、`sched_handoff` を呼ぶ所に `sched_cpu_notify`（sched.c:1790、IPI の中）も挙げる（§4.6.5 の 2）。
- 起こる状況: CPU A で X を手渡し → 後継は新しい thread Y（入口から始まり `complete_*` を通らない）→ 次の tick の前に DTB が Y に `move_pending` を立てて A に通知 → `sched_cpu_notify` が `sched_handoff` を呼び、`cpu->handoff` の X を Y で上書きする。X は RUNNABLE・MIGRATING のまま、どの queue にも無く、誰も enqueue しない（その process は止まる）。`sched_exit_current` が `cpu->retired != NULL` で fatal にしている（sched.c:970）のと同じ種類の約束が手渡しに無い。
- 直し方の案: `sched_handoff` の頭で `complete_handoff(cpu)` を先に呼ぶか、`cpu->handoff != NULL` なら手渡しをしない（`move_pending` を残して次の tick）を不変条件として §4.6.5 に書く。§4.6.5 の 5 の「次の tick で 1 回だけ試し直し」の間も席は埋まっているので同じ規則に従う。host 試験か `CONFIG_DTB_TEST` の stress でこの順を作る。

### M2 `sched_hint_set` の「queue に居れば入れ直す」が、時間つきで眠る thread を起こしてしまう

- 根拠: `queue_remove_thread`（sched.c:2178〜2188）は RUN・WOKEN に加え SLEEP の queue からも外す。§4.5 は「queue に居れば新しい段に入れ直す（`queue_remove_thread` → `queue_append`）」とだけ書き、kind を区別しない。
- 起こる状況: `sched_sleep(timeout)` で眠る thread（sleep の queue、sched.c:1328〜1329）に DTB が hint を置くと、SLEEPING のまま `run[]` に入る。`pick_next_locked` が SLEEPING の thread を走らせ、起床の約束（`wakeup_tick`、`interrupt_generation`）が壊れる。WOKEN の thread を `run[]` に移すと横取りの順（sched.c:184〜190 の comment）も崩れる。
- 直し方の案: 入れ直しは `queue_kind` が RUN か WOKEN の時だけ、同じ kind の新しい段へ（RUN は `queue_append`、WOKEN も同じ kind で）。SLEEP は `priority` を書き換えるだけ。段の添字を変える前に必ず古い段から外す（`queue_remove_thread` は `thread->sched.priority` を添字に使う、sched.c:2183・:2185）事を §4.5・§4.6.1 に明記する。

### M3 溢れ（sched の盗み）と PLACE（DTB の移動）が戦い、build の間ずっと行ったり来たりする。FAST の選びは E を返しうる

- 根拠: §4.6.3 の EFFICIENT の行き先は「使える EFFICIENT で idle → backlog の無い EFFICIENT」。backlog の無い CPU は別の thread を走らせている CPU（`cpu_has_backlog` は queue だけを見る、sched.c:2370〜2386）。§4.6.4 は「PLACE EFFICIENT（BUSY）の thread は idle の FAST の CPU が盗んでよい」。
- 起こる状況: S3（`make -j12`）で E 8 つが 1 本ずつ走り、4 本が P に溢れた状態。DTB の周期は P の BUSY の thread を「違う種類に居る PLACE」と見て `sched_hint_place` → 行き先は backlog の無い（走っている）E → 手渡しで E に 2 本目が待つ → P は idle になり 1 tick 後にその thread を盗み返す（溢れ）。thread ごとに 500 ms おき（`DTB_MOVE_INTERVAL_MS`）に移動・手渡し・盗みを繰り返し、周期の予算（32、手渡し 8）を食い、cache を冷やし、A5（build の時間）を悪くする。§13 の「溢れは S2 では起きない」は S3・S4 には当てはまらない。
  また FAST の順は「使える EFFICIENT で idle」を含むので、E に居る FAST の thread の行き先が別の E になりうる（E → E の無駄な移動）。
- 直し方の案: `sched_hint_place` の行き先は「希望の種類で idle の CPU」だけにし（backlog の無い走っている CPU へは移さない）、選びの結果が今の CPU と同じ種類なら EAGAIN を返す。DTB は「希望の種類に idle の CPU が無い間に、違う種類で走る PLACE の thread」を動かさない（溢れを認めたのと同じ判断）。host 試験の `sched_hint_choose` の表にこの場面を足す。

### M4 受け入れ A4（S2 の package の電力 on ≤ off × 0.95）は電力の上限に張り付いて測れない見込みが高い

- 根拠: 5330 の PL1 は 13.0 W（plan/ws333/design.md:59〜61、WS291 p022 の読み。同じ文書の 5330 の CPU の行には PL1 25 W ともある）。zedBSD の idle の package がすでに約 11.6 W（同:61）。S2 は 8 本の忙しい loop（§6.3）。
- 起こる状況: 8 本が全部の時間忙しいと、on・off のどちらでも package の電力は PL1（または PL2 の後の PL1）に張り付き、差は出ない（on の利得は同じ電力の下の仕事量か温度に出る）。A4 は DTB の良し悪しに依らず不合格か、揺れの中になる。PL1 が 13 W か 25 W かの食い違いも未解決。
- 直し方の案: 電力の受け入れを「決まった量の仕事のエネルギー（J、RAPL の積算）」か「上限に届かない負荷（例: 2〜4 本の忙しい loop、または速さを決めた loop）の package の電力」にする。p006 の最初に PL1・PL2 の実際の値と、S2 の off で上限に張り付くかを記録する手順を足す。Q302-7 の説明にもこの点を書く。

### M5 `CONFIG_DTB=n` が今と同じ事の確かめ（p002 の受け入れ）が、既定の full LTO では測れない

- 根拠: kernel の C は既定で `-flto`（Makefile:173〜176、`ZEDBSD_KERNEL_LTO ?= full`）。`.o` は LLVM の bitcode で、「sched.o・syscall.o・gpu.o・input.o の text の大きさ」は意味を持たない。struct の field の追加は text に出ない。
- 起こる状況: ws.md の受け入れ 4（`CONFIG_DTB=n` で今と同じ）の証拠が何も示さないまま通る。
- 直し方の案: `ZEDBSD_KERNEL_LTO=none` の build で `objdump -d` の差が無い事、または full LTO の `vmunix` の section の大きさと `sizeof(struct thread)`・`sizeof(struct process)`・`sizeof(struct sched)` の一致（`_Static_assert` か debug 情報の読み）で示す。p002 の受け入れの文を直す。

### M6 Phase の依存の誤り: p002・p003 は問いの答えに依る。p003 の危ない code が p004 まで試されない

- 根拠: §7・§10.2 の 5・ws.md は「p002・p003 は答えに依らず進められる」とする。しかし Q302-5 が B（統計を常に取る）なら p002 の `#if CONFIG_DTB` の構造が変わる。Q302-3 が B（nice を DTB に依らず常に効かせる）なら p003 の `base_priority`・飢えの防ぎは `#if CONFIG_DTB` の外に出る（§4.6 の「全部 `#if CONFIG_DTB`」が崩れる）。Q302-2 は p003 の盗みに入る（flag で止められる、とある）。
- また p003（6 LW）の受け入れは「QEMU の起動（hint 無し＝今と同じ）」だけで、手渡し・飢えの防ぎ・`sched_hint_place` を動かす道（`dtb.stress`、`dtb.classes`）は p004 に置かれている。最も危ない sched の変更が、試されないまま次の Phase の土台になる。
- 直し方の案: p002 は Q302-5、p003 は Q302-3（と Q302-2）の答えを依存に書く（または推しで作り、答えが違えば直す LW を見積もる）。`CONFIG_DTB_TEST` の `dtb.stress`（DTB の本体なしで、試験の kthread が無作為に `sched_hint_set`・`sched_hint_place` を呼ぶ形）と `dtb.classes` を p003 に移し、p003 の受け入れに QEMU §6.2 の 4（30 分の stress）を入れる。

### M7 nice で下げた thread が kernel の lock を持ったまま待たされる（priority の逆転）に触れていない

- 根拠: 下げた thread は上の段が忙しい間、100 ms に 1 quantum（10 ms）しか走らない（§4.7）。tick の横取りは kernel の中でも起きる（`sched_clock_cpu` の末尾の `sched_yield`、sched.c:1157〜1158。`preempt_count` が 0 なら）。
- 起こる状況: nice 10 の背景の build が vnode・VM・file system の sleep の lock を持ったまま横取りされると、その lock を待つ前面の process（shell の `ls`、compositor の file の読み）は最大で lock を持つ側の進みの 1/10 の速さで待つ。DTB on で応答が悪くなる道で、A1・A2 の場面（S5: nice の背景＋mview）で出うる。
- 直し方の案: 「kernel の中（`accounting_kernel_depth != 0`）で横取りされた user の thread は、その間 `max(base, 既定)` の段で queue に戻す」などの緩和を設計に入れるか、入れない理由と S5 で測る事を §4.7・§12 に書く。

### M8 どの process も GPU に 1 回仕事を出すだけで priority 7 と P-core を得る（権限の無い昇格）

- 根拠: R5 は「process の `gpu_submit_tick` が 1000 ms 以内」（§4.4.1）。`/dev/gpuN` を開いて `GPU_COMMAND_SUBMIT` などを出せる process はどれも当たる（§2.4）。BUSY は syscall で外れる（§4.4.1）。nice < 0 は root だけ（syscall.c:8936〜8941）なのに、同じ効果を GPU の ioctl 1 回で得られる。
- 起こる状況: 1 秒に 1 回空の submit を出し、200 ms 未満ごとに syscall を挟みながら回る process は、7 のまま P に置かれ、同じ CPU の 8 の thread（他の利用者、kernel の worker、B1）を厳密な priority で押しのける。多人数の機械では利用者の間の公平さが崩れる。
- 直し方の案: 上げは「quantum を使い切り続けない thread」だけに効かせる（例: 直近の周期で run の割合が閾値を超えた thread は上げを外す。BUSY と別の「重い」印）か、上げの CPU の割合に上限を置く。B1 の段の写し方と合わせて決め、§4.4.3 の外れる時の列と Q302-1 に書く。

### M9 R6（terminal、PREFER）がほとんど何もしない。ユーザーの規則が実質入らない

- 根拠: PREFER は「置き場所と盗みで好むだけ、DTB は動かさない」（§4.4.3）。置き場所の変更は作り手と同じ process の新しい thread だけ hint を継ぐ（§4.6.4）。fork の子（shell から起こした command）は別の process なので継がない。起床は居る CPU に戻す（変えない、§4.6.4）。盗みは idle の CPU が backlog のある CPU から取る時だけ（sched.c:2554〜2620）。
- 起こる状況: shell・editor が E で生まれると、E で待ちが出ない限りずっと E に居る。ws.md の「ターミナルアプリ … は … 速いコアに割り付ける」（ユーザーの規則）が、実際には「たまに盗まれた時だけ P」になる。受け入れにも terminal の道具が P に居るかの物差しが無い（A1 の打鍵 → 表示は Terminal の app（GPU、R5）を通り、shell の分は見えにくい）。
- 直し方の案: TTY の thread は「眠っている間に、backlog の無い FAST の CPU があれば `sched_set_cpu` で移す」（走っている thread の手渡しはしない）程度の弱い PLACE にする。P の席の奪い合いは priority（R4・R5 が上）で解ける。受け入れに「S3 以外の terminal の作業で、TTY の thread の走った時間の何割が P か」（`kern.dtb` に印ごとの CPU の種類の時間を足す）を加える。user-questions.md Q302-1 で、推しの A が規則を PREFER と AWAY での停止に弱めている事をはっきり書く（m12）。

### M10 `move_pending` の後始末と、走っているかの判定の競争が書かれていない

- 根拠: §4.5・§4.6.5 の 1 は、走っている thread に `move_pending` を立てて通知する。`sched_hint_activity` の `running` は lock 無しの読み（header :173）。
- 起こる状況:
  1. 通知が着く前に thread が眠ると、`move_pending` が立ったまま残る。後で起きて走った時、古い判断で手渡しされる（行き先の選びは今の空きで行うが、DTB はもう印を変えているかもしれない）。
  2. DTB が「走っていない」と読んで `sched_set_cpu` を呼ぶ間に走り始めると EINVAL（sched.c:1707）。`sched_hint_place` の戻り値の表（0・EALREADY・EAGAIN・EBUSY・EINVAL）では「hint に PLACE が無い」と区別できない。
- 直し方の案: `sched_hint_place` の中で、thread の CPU の lock の下で RUNNING か否かを決め、RUNNING なら印、そうでなければ lock を離して `sched_set_cpu`、EINVAL が「走り始めた」なら EAGAIN に読み替える。`move_pending` は thread が CPU を離れる全部の道（`switch_without_enqueue`・`sched_yield`・`sched_preempt`、sched.c:2306・:745・:828）で消すか、手渡しの時に `sched.hint` の今の種類と今の CPU の種類を照らし直してから行う、を §4.6.5 に書く。

### M11 飢えの防ぎの細部: 戻す所が網羅されず、kernel の段まで上げうる、`base_priority` の初期化が無い

- 根拠: §4.6.1 は上げた thread を戻す所に `sched_yield`（:776）・`sched_preempt`（:877）・`wake_enqueue_locked`（:2051）・`sched_add`（:468）を挙げる。queue に入れる所は他に `sched_set_cpu`（sched.c:1744・:1770）・`steal_runnable`（:2599・:2618）、新しい `complete_handoff` もある。上げ先は「今走っている thread の段」（§4.7）。thread を作る所は `priority` だけを置く（thread.c:104・:190・:256・:410）。
- 起こる状況: 上げられた thread は woken の先頭に居るので盗みで最初に取られる（sched.c:2489〜2491）。盗まれた先では上げたまま走り、戻らない。走っている thread が kernel の 4・5 の thread（DTB 自身が 5、§4.9）なら、user の thread がその段まで上がる。`base_priority` を入れ直しの所で無条件に `priority` に写す実装にすると、初期化されていない kernel・idle の thread が 0 になる。
- 直し方の案: 「queue に入れる全部の所を 1 つの関数にまとめ、`raised` なら `base_priority` に戻す」と書く。上げ先は `max(走っている段, user の上限の段)`。`base_priority` は thread の作成の全部の所（thread.c の 4 か所）で `priority` と同じ値に置く事を p003 の範囲（thread.c）に入れる。§10.2 の file の一覧に `src/kern/thread.c` を足す。

## Minor

### m1 手渡しの間の起床の説明が code と違う

§4.6.5 の 6「thread は RUNNABLE なので `sched_wakeup` は何もしない（:552）」は誤り。MIGRATING の判定が先で、`SCHED_WAKE_PENDING` を立てる（sched.c:545〜549）。`sched_notify_task` も `notify_pending` を立てる（sched.c:1256〜1258）。`complete_handoff` は `need_migrate` を 0 に戻し（WAKE_PENDING も消す）、`notify_pending` は残す、と書く。

### m2 「HAL の契約」と「盗みも頼っている」の根拠が弱い

`hal_task_transfer` の宣言（include/hal/hal.h:1035）に契約の comment は無い。「後継の文脈では去った task は止まっている」は amd64 の実装（task.c:618〜626・:658〜661・:672〜687）の性質で、盗みはむしろ `HAL_ERR_BUSY` で断られる事に頼っている（sched.c:2547〜2552・:2594〜2604）。§5 の「契約」を「amd64 の実装の性質、断られたら元に戻す」と言い直す。契約として hal.h に書くなら HAL の文書の変更で、事前の承認が要る（AGENTS.md §7）。

### m3 input の配りの数と、resync の report

`drv_input_subscriber_publish` の呼び出しは 9 か所（input.c:826・838・848・853・946・1081・2526・2576・2617）で、§2.3 の「6 か所」は誤り。後の 3 か所は reader が開いた時の resync（`INPUT_REPORT_RESYNC_BEGIN` など、input.c:2493〜2530）で、event を持つ。`kern_dtb_note_input` の除外に resync の flag を足さないと、compositor が device を開き直すたびに ACTIVE になる。

### m4 platform の数の誤り

vmunix.mk は 15（platform/ の 17 の directory には common・fpga を含む）。`ZEDBSD_CONFIG_CPPFLAGS` を使わないのは mips32-rg350 の 1 つ（§4.11 の「3 つ」は誤り）。input.c を build するのは 15 のうち 12（§2.3 の「17 のうち 12」）。

### m5 process の巡回は O(N²) で、周期の代価の見積もりに入っていない

`process_find_next_ref` は呼ぶたびに全部の process を `process_tree_lock`（irqsave）の中で走査する（process.c:773〜797）。1 周期で N 回呼ぶので N² の比べ。数百の process で §4.9 の「数十 µs」と A6 の「1 ms 未満」の見積もりを直すか、1 回の lock で pid と参照の列を取る形にする。

### m6 header の案の細部

- dtb.h: `struct dtb_thread_state` の comment は「balancer だけが読み書き」だが、`gpu_submit_tick` は thread が `kern_dtb_note_gpu` で書く（dtb.h:46〜58）。thread の GPU の印（§4.4.1）は規則 R1〜R7 のどれにも使われない。使わないなら field と書き込みを外す。
- sched-hint.h: `<stdbool.h>` を使っていない。`sched_hint_choose` の `idle`・`backlog` の配列の長さ（`KERN_CPU_MASK_WORDS` か）が書かれていない。`sched_hint_place` の comment「may sleep only as sched_set_cpu() does」は、`sched_set_cpu` が眠らないので誤解を招く。`struct sched_hint_state` の配置（`struct sched` の中、sched.h に include する）と、kernel の中だけの型で ABI の約束は無い事を書く。
- 名前: 既存の `sched_private.h` は下線、新しい `sched-hint.h` は hyphen。どちらも tree にあるが、sched の header の並びとしては揃えた方が読みやすい（推測、規約に明文は無い）。

### m7 起動の parameter の値の検査

`dtb=off`・`dtb.stress=1` は値を持つ parameter で、boot.c:297〜315 の「単語を取る key の列」と `parameter_word` に足す必要がある。`dtb.classes=PPEE…` は単語でない文字列なので別の検査が要る。§4.11 は enum と名前の表だけを挙げる。

### m8 記録の食い違い

- user-questions.md の前置き「敵対的レビューを反映済み」、ws.md の p001 の行「設計と review の反映は済み」は、この review の前なので事実と違う（design.md §14 は「review の後に書く」）。
- ws.md「今の事実」は古い行（sched.c 2952 行、:1665-1758、syscall.c:9796、gpu.c:219）のまま。design.md §2.8 で引き直した値に揃える。
- ws.md の「出典」と D1・D3 はユーザーの発言の原文を載せ、design.md §0 はそこを指す。AGENTS.md §8（2026-10-30、原文を記録に載せない）に合わない。直すのは Q1 の判断（既存の記録）。

### m9 試験の計画の穴

- B2・M1・M2 の競争を作る試験が無い。§6.2 の 4 の stress に「thread の作成と exit・fork の嵐」「時間つきの sleep の thread への hint」を足す。
- §6.2 の 5（SMP 1 の nice）は「0 でなく nice 0 より小さい」だけで、飢えの防ぎの効き過ぎ・効かなさを検出できない。期待の約 9% の幅（例: 5〜15%）で判定する。
- BUSY の検出は syscall の無い合成の loop（S2）でしか確かめない。実際の負荷（`xz -9`、S3 の compiler）の BUSY の数を `kern.dtb` で記録する項目を p006 に足す（§12.5 の未確認を測りに変える）。
- S2 の cursor の遅れの `c6.py` は WS075 の `measure-apps.sh`・`h4-ctl.py` の計測の仕組み（plan/tools.md:166）が要る。5330 でその器具が使えるかは未確認（推測: HDMI の capture の器具が要る）。`key-latency.py` は Keiland の `--log-frames` と `TERMINAL_PERF=1` の app の log を使う（kernel の console・serial の log ではないので AGENTS.md §9 には触れない）。

### m10 計数の「書き手 1 つ」の主張

`wakeups` を足す所は `sched_wakeup` だけでなく、tick の期限の起床（sched.c:1044〜1056）・`sched_notify_task`（:1259〜1264）・`sched_set_cpu` の持ち越し（:1766〜1771）もあり、移動の途中は別の CPU の lock の下になる。観察だけなので近似でよいが、§4.3 の「書き手 1 つ」は直す。`user_streak` は u32 で一周しうる（1 kHz で約 49 日）ので飽和させる。

### m11 依存の書き方

§6.3「WS333 の RAPL の meter（p011）を `/dev/thermal` で読む（p005 の後半・p013 の後）」の「p005」が WS333 の Phase か WS302 の p005 か読めない。`ws333-p005` のように書く。

### m12 ユーザーへの問いの公平さと抜け

- Q302-3 の「kernel の thread の段（0〜5）には入らない」は事実と違う（B1）。直さないとユーザーは安全だと誤解して選ぶ。
- Q302-1: 推しの A は terminal の規則を「P が空いている時だけ・60 秒操作が無ければ止める」に弱めており、ユーザーの規則（UI の応答を落とさない範囲で速い core）からの解釈である事を明記していない。「TTY を BUSY より上、ただし PREFER（能動的には動かさない）」の案が無い。
- 抜けている問い: user の thread を kernel の worker の段より上に置いてよいか（B1）。GPU を使うだけで上がる事の扱い（M8）。
- Q302-6: UAPI の形（struct の版・大きさ・field）が `proposed/` に無く、ユーザーが「後で変えにくい」口を形を見ずに選ぶ事になる。`proposed/include/uapi/` に案を置く。

### m13 64 本を超える process の「続きから」

§4.9 の 3「64 を超える process は次の周期に続きから」は、thread の列が順序を持たず作成・exit で変わるので、位置で続けると飛ばし・重なりが出る。tid の cursor（tid の昇順に次の 64 本）で続けるか、上限を超えた分は飛ばす事を書く。

## 確かめて問題が無かった点（主な物）

- lock の順: `process->lock`（20）を離してから sched（200）。cpuperf の box（125）を sched の lock の中で取らない（include/kern/lock.h:16・:44・:64、低い rank が先）。`kern_dtb_note_input` は subscriber の lock（`LOCK_RANK_DEVICE`、irqsave、input.c:1507・:1612）の中で atomic だけ。
- syscall の入口の窓: `sched_accounting_kernel_enter` は割り込みを止めた中（syscall.c:10443〜10446）。全部の HAL の syscall はここを通る（src/hal/*/int.c）。fault（user-probe.c:96〜99）と user への戻り（signal.c:410〜413）も depth を上げるので、`user_streak` の tick の分類は成り立つ。
- tick と通知の割り込みは EOI を先に送ってから sched を呼ぶ（clock.c:109・:198）ので、割り込みの中で切り替えた thread を他の CPU で再開する事は今の盗みと同じ前提（新しい危険ではない）。
- 後継の文脈では amd64 で去った task の `run_cpu` は -1（task.c:658〜661、新しい task の入口も `amd64_task_finish_switch` を先に呼ぶ、:667〜670 の comment）。HAL の差分が要らないという結論には同意する（m2 の言い方の直しだけ）。
- `CONFIG_DTB` を platform の欄 amd64 で入れ、config の stamp（Makefile:1021〜1030）が flag の変化で object を作り直すので、n と y の混ざった build にはならない。
- 引用の行の多く（sched.c:181・398・520・910・1009・1125〜1137・1691・1790・2051・2178・2192・2237・2269・2390・2479・2533・2554、thread.c:104・190・256・410、syscall.c:8880・10420・10445・10516、process.h:129、cpuperf.h:354・385、cpuperf.c:577、cpu-x86.c:171・495、entry.c:382・405、pcat.c:497、Makefile:941、boot.h:107、gpu.c:225・1840・2113、input.c:1517・1602）は一致した。

## 数

Blocker 2（B1・B2）、Major 11（M1〜M11）、Minor 13（m1〜m13）。
