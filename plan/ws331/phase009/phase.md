<!-- awesome-plan project=zedbsd record=ws331-p009 -->

# ws331-p009: kernel の header から HAL を分ける

Status: planned
Parent: [WS331](../ws.md)
Queue: q1721（P2、p008 の後）

## 目的と範囲

2026-10-30 ユーザーから次の趣旨の指示を受けた: 整数の型は kernel で定義し直す。`sched.h` は `sched_private.h` と分ける。`sleep.h`・`random.h`・`clock.h` が HAL に依るのはよくない。`atomic.h`・`thread.h`・`vmspace.h` はそれぞれ `_private.h` を足し、HAL は `_private.h` からだけ参照する。構造体の pointer（hal_task_t・hal_space_t）の扱いは検討する。

1. `kern_cpu_id_t` などの kernel の整数の型。`sleep.h`・`random.h` から HAL を外す。
2. `sched.h` と `sched_private.h`、`atomic.h`・`thread.h`・`vmspace.h` の `_private.h`。driver は `_private.h` を include しない。
3. HAL の名前を使わない 6 個（io-scratch・pmem・buf・vm-object・input-device・input-keymap）から HAL の include を外す。
4. `clock.h` の `HAL_TIMER_FREQUENCY` は WS332 で KERN_CLOCK_HZ と一緒に無くす（ここでは扱わない）。
5. 確かめ: 20 port の build、grep で `include/kern/` の公開の header が HAL を include しない事（`_private.h` を除く）。
