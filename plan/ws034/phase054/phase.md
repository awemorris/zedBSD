<!-- awesome-plan project=zedbsd record=ws034p054 -->

# ws034-p054: pipe・device の I/O が 512 byte ずつ、libc の余分な system call

Phase ID: `ws034-p054`
Parent: [WS034](../ws.md)
Status: **cleared**（q368-i01、2026-09-24）
Phase disposition: normal
Queue: q368（q368-i01）
実行: メインセッション

## 経緯

rpi4 の確認（ws036-p012）で `dd if=/dev/zero bs=4096 count=512` が 256 KiB しか書かないのに気づいた。調べると、クライアント PC
として効く性能の問題がいくつか重なっていた。

## 見つけたこと

1. **pipe・socket・device の `read(2)` は 1 回 512 byte まで**。syscall の bounce buffer が stack の 512 byte で、regular file 以外は
   1 回の backend の転送で終わる。`dd if=/dev/zero of=f bs=1M count=10` が **640 KiB（実際は 5 KiB）** しか作らない（dd は短い
   読みも 1 block と数える）。多くの script がこの形で file を作る。
2. pipe の容量が 4 KiB。
3. **libc の `read()`・`write()` は毎回 `pthread_testcancel()` を前後 2 回呼び、それぞれ system call を 1〜2 回する**
   （自分の TCB を kernel に聞く `thread_self(GET_TLS)` と、`thread_cancel(CLEAR)`）。stdio も lock のたびに TCB を聞き、unlock の
   たびに waiter の有無を見ずに `usync` の wake をする。`getc()` 1 回で最大 6 回ほどの system call。
4. `posix_memalign()` が無かった（POSIX）。fork の子の `pthread_self()` が親の thread id のままだった。
5. `/dev/random`・`/dev/urandom` が無く、`getentropy` は RDRAND の無い機械で ENOSYS（→ 新 Phase p055）。
6. pipe の `read` で待っている thread に `pthread_cancel` が届かない（kernel は cancelable な usync の待ちだけを起こす）
   （→ 新 Phase p056）。

## 変更

- `src/kern/syscall.c`: bounce buffer を `syscall_io_buffer()` に。regular file は今まで通り共有の pool、**pipe・socket・device は
  heap に最大 64 KiB**（待つ間に pool を占めないように）。解放は `syscall_io_buffer_release()`。
- `include/kern/cdev.h`・`src/kern/cdev.c`: `cdev_ops.flags` と `CDEV_READ_NEVER_WAITS`。この flag の device（`/dev/zero`・`/dev/full`）
  の `read` は、signal が来るまで要求を満たすまで読み続ける。`cdev_file_read_never_waits()`。`audio.c`・`gpu.c` の cdev_ops を
  指示付きの初期化子に。
- `include/kern/pipe.h`: `KERN_PIPE_CAPACITY` を 16 KiB に（`PIPE_BUF` は 512 のまま）。
- `userland/base/libc/pthread.c`: thread が 1 つの間は `self_tcb()` が `main_tcb` を返す（system call なし）。`pthread_cancel()` が
  一度も呼ばれていなければ `pthread_testcancel()` はすぐ戻る。fork の子で自分の thread id を取り直す。
- `src/libc/stdio.c`: FILE の lock を 0/1/2（2 = 待つ thread がいる）にし、unlock は 2 のときだけ wake。
- `src/libc/stdlib-extra.c`・`<stdlib.h>`: `posix_memalign()`。

## 検証

| 試験 | 結果 |
| --- | --- |
| `dd if=/dev/zero of=/tmp/g bs=1048576 count=10` | 10485760 byte（前は 640 KiB 以下） |
| 16 MiB の throughput（`plan/ws034/tests/stream-bench.py`、3 回の最小。[stream-bench.txt](evidence/stream-bench.txt)） | 旧 → 新: `dd\|dd` 4 KiB 1.24 → 0.27 s、64 KiB 2.10 → 0.18 s（旧は 512 byte ずつ）、`cat\|cat` 2.14 → 1.21 s、`cat\|dd` 2.08 → 1.09 s、`/dev/zero` 1 MiB×16 は旧が 8 KiB しか運ばず、新は 16 MiB を 0.08 s |
| `plan/ws034/tests/cancel-test.c`（guest） | 6/6（[cancel-test.txt](evidence/cancel-test.txt)）: fork の子の `pthread_self`、`pthread_testcancel` で回る thread の cancel、2 thread が同じ FILE に書いても行が壊れない |
| CI の kernel（amd64・pcat・pc98・rpi4） | warning 0。rpi4 の disk-image と QEMU の login、pcat の disk-image も warning 0 |

## 残り

- `cat` は同じ大きさの `dd` より遅いまま（`cat /tmp/f \| cat` 1.2 s に対し `dd\|dd` 0.27 s）。buffer の置き場所（stack・static・heap・
  page 境界、`plan/ws034/tests/copybench.c`）では差がほぼ無かった。原因は未特定。この環境の時間は揺れが大きい（同じ試験で 2〜3 倍）。
- 最初の起動で一度だけ、login 直後に shell が消えて getty が起動し直した（3 回の再起動では再現せず）。原因は未特定。
- 複数 thread の process では `self_tcb()` は今も system call（thread pointer を直接読むには、TLS の無い状態の扱いが要る）。
