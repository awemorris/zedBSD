<!-- awesome-plan project=zedbsd record=ws041-design -->

# WS041 設計: 起きた thread を即時に走らせる

Parent: [WS041](ws.md) / Phase: ws041-p001
Status: 設計（2026-09-24）。HAL の変更は要らない（§6）

## 1. 今の動き（`src/kern/sched.c`、2026-09-24 に読んだ）

- CPU ごとに優先度 16 段（`SCHED_PRIOR_HIGH` 0〜`SCHED_PRIOR_LOW` 15、既定 8）の run queue。thread は CPU に固定される。
- `sched_wakeup()`・`sched_add()` は thread を run queue の**末尾**に入れ、quantum を満たし、その CPU の `need_resched` を立て、
  `notify_cpu()` を呼ぶ。
- **別の CPU** の thread を起こしたとき: `hal_cpu_notify()` の割込みで、相手の CPU の `sched_cpu_notify()` が `need_resched` を見て
  **すぐ `sched_yield()`** する。つまり別の CPU では、今でも起床はほぼ即時である。
- **同じ CPU** の thread を起こしたとき（割込みから、または syscall の中から）: `notify_cpu()` は自分の CPU なら何もしない。
  走っている thread は、**自分で寝るか、quantum（`SCHED_QUANTUM_TICKS` = 5 tick）を使い切るまで**譲らない。
  - amd64・arm64（1000 Hz）で最大 5 ms、**i386・pc98（100 Hz）で最大 50 ms** 待つ。
  - CPU を使い続ける process（build、動画の decode）の横で、keyboard の入力・pipe の相手・timer で起きた thread がこれだけ遅れる。
- CPU が idle なら、idle の loop が割込みの後に `sched_switch()` するので即時。
- 戻り道の `kernel_user_return_handler()`（`src/kern/signal.c`）は `need_resched` を見ていない。
- quantum が尽きた thread と、`sched_yield()` した thread は、自分の優先度の末尾へ戻る。

## 2. 目標

- 起きた thread が、走っている thread と**同じか高い優先度**なら、同じ CPU でも**次の安全な地点で**すぐ走る。
  低い優先度の thread は、今までどおり順番を待つ。
- CPU を使い続ける thread どうしの公平さを崩さない（起床で押しのけられた thread が不利にならない）。
- 起床を繰り返す thread の組が、CPU を使い続ける thread を飢えさせない。
- quantum をミリ秒で決め、platform の tick の周期から導く。

## 3. 決定

### D1. 切り替える地点

1. **user へ戻るとき**（`kernel_user_return_handler()` の先頭）。HAL は syscall・fault・**割込み**のいずれから user へ戻るときも
   これを呼ぶ（amd64・i386・arm64 の `int.c`、sparcv9・m68k の `trap.c` のすべてが呼ぶ）。user の code を走らせていた thread が割込みを受け、その割込みで
   別の thread が起きた場合も、ここで切り替わる。
2. **tick**（`sched_clock_cpu()`）。1 の地点を通らない場合（kernel の thread が走り続けている等）でも、1 tick 以内に切り替わる。
3. **別の CPU の通知**（`sched_cpu_notify()`）。今と同じ。
4. idle の loop（今と同じ）。

走っている kernel の code の任意の地点では切り替えない（`sched_wakeup()` の呼び手は spinlock を持っていることがある）。
`kern_preempt_disable()` の間は切り替えず、`kern_preempt_enable()` で行う（今と同じ仕組み）。

### D2. 誰が誰を押しのけるか

- CPU ごとに**今走っている thread**（`struct sched_cpu` に `current` を足し、切り替えのたびに更新）を持つ。
- 起床（`sched_wakeup()`・`sched_add()`）の時、起きた thread の優先度の値が走っている thread の値**以下**（同じか高い）なら、
  その CPU の **`preempt` の印**を立てる。走っているのが idle の thread なら常に立てる。低い優先度なら立てない（`need_resched` だけ）。
- 切り替えの地点（D1）は `preempt` の印を見て切り替える。`need_resched` だけでは user へ戻るときに切り替えない
  （低い優先度の thread のために、無駄に列を回さない）。

### D3. 押しのけられた thread は順番を失わない

- 印による切り替え（`sched_preempt()`、新設）では、まず次の thread を取り出し（起きた thread）、**押しのけられた thread を自分の優先度の
  列の先頭**に、**残りの quantum のまま**戻す。起きた thread が寝れば、押しのけられた thread が続きを走る。
- 今の `sched_yield()`（末尾へ、quantum を満たす）は、自分から譲る場合と quantum の終わりにだけ使う。
- 起きた thread は、優先度ごとの**起床の列**（`woken`、起きた順の FIFO）に入れる。`pick_next` は同じ優先度では起床の列を run queue より先に取る。
  押しのけない（低い優先度の）場合は今どおり run queue の末尾。
- （p003 で訂正）最初の実装は起きた thread を run queue の先頭に入れていた。これでは起床の多い thread（networkd、kernel の worker）が
  先頭を奪い合い（後に起きた方が先）、押しのけられた thread も先頭に戻るので、起きた thread の前に押しのけられた thread が戻ってしまった。
  端末の入力の echo が quantum 1 つ分（9 ms）遅れたことで見つかった。

### D4. 飢えさせない: quantum は起床で満たさない

- 今は起床のたびに quantum を満たしている。これをやめ、**quantum は使い切って末尾へ戻るときにだけ満たす**。
- 起床と寝ることを繰り返す thread も、走っている間の tick で quantum を減らされる。使い切れば末尾へ回り、CPU を使い続ける
  thread の番が来る。起床の組が列の先頭を占め続けることはない。
- 初めて走る thread（`sched_add()` の新しい thread）は満たした quantum で始める。

### D5. quantum はミリ秒で決める

- `SCHED_QUANTUM_MS` = **10 ms**。tick の数は `KERN_MS_TO_TICKS()`（最小 1）で導く: amd64・arm64 で 10 tick、i386・pc98 で 1 tick。
- 今は 5 tick（amd64 5 ms、i386 50 ms）。amd64 は長くなるが、起床の即時化で対話の遅れは quantum に依らなくなり、
  CPU を使い続ける thread の切り替えの費用は減る。i386 は 50 ms から 10 ms に短くなる。

### D6. 多重の起床

- 同じ地点までに複数の thread が起きた場合、起きた順に走る（起床の列は FIFO）。押しのけられた thread は、そのすべての後に続きを走る。

## 4. 実装の範囲（p002）

| 場所 | 変更 |
| --- | --- |
| `include/kern/sched.h` | `SCHED_QUANTUM_MS`、`SCHED_QUANTUM_TICKS` を導出に。`sched_preempt_point()` の宣言 |
| `src/kern/sched.c` | `struct sched_cpu` に `current`・`preempt`。起床での判定と先頭への挿入、quantum を満たさない。`sched_preempt()`（押しのけられた thread を先頭へ）。tick・通知・`kern_preempt_enable()` で `preempt` を見る |
| `src/kern/signal.c` | `kernel_user_return_handler()` の先頭で `sched_preempt_point()` |
| 試験 | host の sched の fixture（あれば）に、優先度ごとの判定・先頭への戻し・quantum を満たさないこと |

## 5. 測定（p003）

- `-smp 1` の QEMU（全部が同じ CPU）で、CPU を使い続ける process の横で:
  1. pipe の往復（2 process が交互に 1 byte を送り返す）1000 回の時間
  2. `nanosleep(1 ms)` の起床の遅れ
  3. 入力の echo（serial から 1 文字、shell の echo まで）
- amd64（1000 Hz）と i386（100 Hz）で、変更前と後を比べる。kernel の `SCHED_WAKE_LATENCY` の診断（起床から走るまでの分布）も使う。
- CPU を使い続ける 2 process の公平さ（同じ時間で両者の CPU 時間が同じくらい）が崩れないこと、起床を繰り返す 2 process の組と CPU を
  使い続ける process が並んでも後者が止まらないこと。

## 6. HAL

変更は要らない。使うのは既存の `hal_cpu_notify()` と、HAL が user へ戻るたびに呼ぶ `kernel_user_return_handler()` だけ。

## 7. 自己レビュー（敵対的な見直し）

| 疑い | 検討 | 結論 |
| --- | --- | --- |
| 起床の組が CPU を使い続ける thread を飢えさせる | D4: quantum を起床で満たさない。組も tick で減らされ、使い切れば末尾へ | 待ちは最大で組の quantum 分 |
| tick で減らすだけでは、tick の間に寝る短い走りは数えられない | 短い走りは確率的にしか数えられない（tick の標本）。平均では走った時間に比例する | 当面は許容。必要なら後で実時間で数える |
| 押しのけられた thread を先頭へ戻すと、起きた thread の後ろではなく前になる | `sched_preempt()` は先に次の thread を取り出してから押しのけた thread を先頭へ戻す | 起きた thread が先に走る |
| user へ戻るたびに lock を取ると遅い | `preempt` の印を lock 無しで読み、立っているときだけ lock を取る | 印が無ければ費用はほぼ無い |
| kernel の thread は user へ戻らない | tick（D1-2）で 1 tick 以内に切り替わる | 許容 |
| 割込みの handler の中で切り替える | しない。切り替えは user へ戻る地点・tick・通知・idle だけ（tick は今も割込みの中で yield している） | 今と同じ前提 |
| i386 の quantum が 1 tick（10 ms）になり、切り替えが増える | 50 ms → 10 ms。100 Hz の i386 でも 1 秒に 100 回。対話の応答の方が重要（ユーザーの方針） | 許容。測定で確かめる |
| 優先度の低い thread の起床が、高い thread に無駄な列の回転をさせる | D2: 印を立てない。`need_resched` だけ | 回らない |

## 8. 範囲外

- CPU 間の負荷の分散（thread は CPU に固定のまま）。
- 実時間での CPU 時間の計測、動的な優先度（nice による重み付け等）。
