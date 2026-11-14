<!-- awesome-plan project=zedbsd record=ws302-p003 -->

# ws302-p003: sched の口（hint・段・種類の表・行き先・手渡し・飢えの防ぎ）

Status: in-progress
Parent: [WS302](../ws.md)
Queue: q2099-i01（P6）

## 目的

DTB の本体（p004）が使う sched の口を作り、DTB の本体が無くても試験の thread で全部の道を通して壊れない事を示す。DTB は sched ではないので、sched は hint を持って自分の判断の所（置き場所・盗み・idle の CPU の起こし・頼まれた移動・走っている thread の手渡し）で使うだけにし、果たせない頼みは断る。

本体は [design.md](../design.md) 第 3 版の §4.5（口）・§4.6（sched の変更）・§4.7（段と飢えの防ぎ）・§4.11（試験の config）・§6.1・§6.2 の 3、§7 の p003 の行。

## 作る物（全部 `#if CONFIG_KERN_DESKTOP_BALANCER`。n の kernel は今と同じ）

1. **段**: `sched_hint_set()`（hint と base の段を置く。RUN・WOKEN の queue に居れば新しい段に入れ直し、SLEEP は field だけ、走っていて下がれば preempt）。queue に入れる全部の所で `queue_level_locked()` が段を base に戻し、user の thread が kernel の中に居れば `SCHED_HINT_USER_DEFAULT`（10）より下げない（kernel の中の優先）。
2. **CPU の種類の表**: `sched_cpu_classes_set()`（CPU ごとに 1 語の atomic、FAST と EFFICIENT がそろう時だけ種類の好みが効く）。
3. **行き先の選び**: 純な関数 `sched_hint_choose()`（MOVE・MOVE_IDLE_CORE・PLACE）と段の純な関数（`sched_hint_queue_level()`・`sched_hint_starve_level()`）。新しい source `src/kern/sched-hint.c`（host 試験の対象）。
4. **置き場所・盗み・tick の助け**: 新しい user の thread は作り手の hint と base の段を継ぎ（作り手の CPU の lock の下で読む）、作り手の CPU に backlog があれば PLACE の順で idle の CPU を選ぶ。盗みは同じ段の中で盗む CPU の種類を望む thread を先に、NO_OVERFLOW の thread は違う種類へは飢えの上限まで渡さない。tick の助けは待ちの先頭の thread の種類の idle の CPU を先に起こす。起床の道は変えない。
5. **`sched_hint_place()`**: 走っていない thread は hint の移動の道（元の CPU の lock の中で状態を確かめ直す。`sched_set_cpu` は変えない、D-5・D-6）、走っている thread は `move_pending` を立てて CPU に通知。
6. **手渡し**: CPU の通知か tick で `move_pending` の current を席（`handoff`、1 つ、上書きしない）に置いて切り替え、後継（切り替えの後の 4 か所と自分の再開の後）か次の tick の頭が transfer して行き先に入れる。断られれば 1 回だけ次の tick で試し直し、駄目なら元の CPU に戻す。BOUND は手渡さない。
7. **飢えの防ぎ**: tick ごとに今の段より下の queue の先頭の user の thread が 100 ms 待てば、`max(今の段, 8)` の woken に 1 回だけ上げる。
8. **計数**: `sched_hint_counters()`（CPU ごとに足し、読む時に合わせる）。
9. **試験の config `CONFIG_KERN_DESKTOP_BALANCER_TEST`**: `src/kern/sched-hint-test.c`。起動の parameter `desktop_balancer.classes=PPEE`（偽の種類の表）と `desktop_balancer.stress=1`（10 ms ごとに無作為の user の thread に無作為の hint と `sched_hint_place`）。結果は global の報告の struct に書き、QEMU の gdbstub で読む（console・serial の log は読まない）。

## 試験

- host 試験（`plan/ws302/tests/balancer-host/`）: `sched_hint_choose` を 5330 の形（P 論理 4・E 8）・PPEE・全部 P・全部 ANY・usable 0 の P で、idle と backlog の bit の組を回して、MOVE が違う種類を返さない・MOVE_IDLE_CORE が兄弟の忙しい CPU を返さない・PLACE の順を独立の参照の実装と照らす。段の関数（kernel の中で 10 より下げない、上げが 8 より上にならない、上げの条件）。突然変異の確かめ。
- build: x86_64・rpi5・vf2 の n と y（と x86_64 の y＋thermal、y＋試験）が warning 0。n の kernel は基点 991f37fe79 と同じ（`tests/dis-compare.py`）。
- QEMU（x86_64、SMP 4、KVM）: y の image の起動（boot-test.sh）と、y＋試験の image で `desktop_balancer.classes=PPEE desktop_balancer.stress=1` の 30 分の stress（kstress の iostress・credrace・sigexec、thread の作成と exit・fork の嵐、時間つきの sleep が早く起きない事、syscall の無い忙しい loop）。止まらない・panic しない（serial の対話で guest が答える）、手渡しと移動の数 > 0、手渡しの失敗の数を記録（gdbstub で報告の struct を読む）。

## 受け入れ

- 上の host 試験 PASS、build warning 0、n の kernel が基点と同じ、QEMU の y の起動 PASS、30 分の stress PASS（guest が答え、手渡し > 0・移動 > 0、時間つきの sleep の早起き 0）。
- mk の追加の案（下）はユーザーの承認まで当てない。当たるまで y は overlay（[../tests/desktop-balancer-overlay.mk](../tests/desktop-balancer-overlay.mk)）で build する。

## mk の追加の案（承認まで当てない）

（実装の後に書く）

## 進み

（実装の後に書く）

## 結果

（試験の後に書く）
