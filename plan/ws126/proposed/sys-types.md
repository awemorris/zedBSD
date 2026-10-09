# 案: libc の `<sys/types.h>` に POSIX の型を足す（ws126-p002 の止まり、2026-10-09 P1）

2026-10-09 q916 item 1（commit b6c91e342）で適用した。結果は [ws126-p002](../phase002/phase.md) の「依存の解消」。

## 足りない型と、今どこにあるか

| 型 | 今の定義 | 案 |
| --- | --- | --- |
| `time_t` `clockid_t` `timer_t` | `<uapi/time.h>`（typedef） | `<sys/types.h>` が `<uapi/time.h>` を include する（`<uapi/time.h>` は struct timespec も持つが、`<sys/types.h>` の名前空間に出ても POSIX は禁じない。気になるなら typedef だけを `<uapi/types.h>` に移し、`<uapi/time.h>` はそれを include する） |
| `clock_t` | `<time.h>` の `typedef long clock_t;` | `<uapi/types.h>`（か `<sys/types.h>`）に移し、`<time.h>` は `<sys/types.h>` を include する |
| `key_t` | `<sys/ipc.h>` の `typedef int key_t;` | `<sys/types.h>` に移し、`<sys/ipc.h>` は include する |
| `fsblkcnt_t` `fsfilcnt_t` | `<uapi/statvfs.h>`（fsblkcnt_t。fsfilcnt_t も同じ所の見込み） | `<sys/types.h>` が `<uapi/statvfs.h>` の typedef を見る（typedef だけを `<uapi/types.h>` に移す） |
| `pthread_t` ほか `pthread_*_t` | `<pthread.h>`（struct の中身ごと） | `<sys/types.h>` に移す（`pthread_attr_t` は `struct sched_param` を含むので、`<sched.h>` の struct の定義も要る。`<pthread.h>` は `<sys/types.h>` を既に include している） |

## 確かめ方

- `zedbsd-clang` で `#include <sys/types.h>` だけを入れ、上の型を 1 つずつ宣言して compile（2026-10-09 に 10 個が未定義）。
- 既存の C の program の build（同じ型が 2 つの header で typedef されないこと。C11 は同じ typedef の繰り返しを許すが、kernel の側の include の規則（kernel は libc の header を読まない）に触れないこと）。
- その後 `make … python3`（ws126-p002）で、configure の `checking size of time_t` が 8 になること。
