<!-- awesome-plan project=zedbsd record=ws327-p002 -->

# ws327-p002 thread の層（`keiland_thread.h`、KL_VERSION 103）と host 試験

- 親: [WS327](../ws.md)。設計: [design.md](../design.md) §3。Queue: q1697 / q1697-i01（担当 P2、2026-10-30）。
- Status: cleared（2026-10-30。zedBSD の guest での実行は p003 と一緒に、未実施）
- Disposition: normal

## 目的・受け入れ

- `kl_thread_*`・`kl_mutex_*`・`kl_cond_*`・`kl_once`・`kl_thread_key_*`（POSIX、pthread）。build warning 0（zedBSD 3 arch・Linux）、host 試験（TSan）。

## 実装

- `userland/desktop/include/keiland/keiland_thread.h`（新）、`keiland.h` が include、`keiland_version.h` 103、`exports.map` 再生成。
- `userland/desktop/libkeiland/platform/thread-posix.c`（新、3 つの Makefile に追加）: thread の記録は owners（2 から）で join・detach と thread の
  終わりが放す。起動は非同期の signal を止め同期の fault は止めない。名前は Linux・FreeBSD だけ。優先度は Linux の thread の nice だけ（hint）。
  mutex・cond・once・key は interface の記憶（64・64・32・16 byte）の中に pthread の物を置く（`_Static_assert` で大きさと alignment）。
  cond は CLOCK_MONOTONIC。stack は `PTHREAD_STACK_MIN` に切り上げ。

## 確かめ（2026-10-30）

- host: `sh plan/ws327/tests/host-test.sh build/ws327-linux thread` → gcc・clang × （libkeiland.so に link・TSan・ASan+UBSan）の 6 build で
  `thread-test.c` 28 項目 PASS（mutex の加算 8×100000、生産 1・消費 4 の 20000 件、`wait_for` の時間切れ、once 8 thread、key の destroy、
  detach（ASan の漏れ 0）、join の EDEADLK、`KL_MUTEX_INIT`・`KL_ONCE_INIT`、各 initializer の byte が 0、`kl_mutex_try` の EBUSY、名前の切り
  詰めと読み返し（Linux）、優先度 LOW・HIGH、EINVAL）。
- build: Linux（`keiland-linux.mk`、`build/ws327-linux/lib/libkeiland.so`）warning 0。zedBSD は p003 の欄。
- 未実施: zedBSD・FreeBSD の上での thread-test の実行。
