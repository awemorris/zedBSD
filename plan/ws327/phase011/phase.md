<!-- awesome-plan project=zedbsd record=ws327-p011 -->

# ws327-p011 Settings の移行（見た目の読み・storage の走査・ごみ箱の量）

- 親: [WS327](../ws.md)。設計: [design.md](../design.md) §1.1・§6.1・§6.3・§13。担当: 実装の担当（Sonnet 5.5 high）。依存: p007。
- Status: planned
- Disposition: normal

## 1. 読む物

- `AGENTS.md` §4・§7・§9・§10、`plan/coding-style.md` の全文。
- design.md の §3.2、§6.3、§13（共通の約束。この phase.md の一部として従う）。
- header（仕様、変えない）: `userland/desktop/include/keiland/keiland_thread.h`。

## 2. 触ってよい file

| file | 何を |
| --- | --- |
| `userland/desktop/settings/settings.h` | `<pthread.h>` → `<keiland/keiland_thread.h>`、`struct se_look_loader` の `pthread_t thread; pthread_mutex_t lock;` → `struct kl_thread *thread; struct kl_mutex lock;` |
| `userland/desktop/settings/look.c` | §3.1 |
| `userland/desktop/settings/storage-scan.h`・`storage-scan.c` | §3.2 |
| `userland/desktop/settings/storage-trash.h`・`storage-trash.c` | §3.3 |
| `plan/ws327/phase011/phase.md` の「結果」、`plan/ws327/ws.md` の p011 の行 | 記録 |

Settings の Makefile 3 本（zedBSD・`Makefile.linux`・`Makefile.freebsd`）は libkeiland を既に link するので変えない（`-pthread` も残す）。

## 3. 前後の対応（§13.2 の表を全ての `pthread_*` に。以下はその上で直す所）

### 3.1 `look.c`（見た目の page の壁紙の小さな写しを読む thread）

| 所 | 前 | 後 |
| --- | --- | --- |
| 起動（656 行あたり） | `error = pthread_mutex_init(&loader->lock, NULL)`、`pthread_create(&loader->thread, NULL, look_load_run, loader)`、失敗で `pthread_mutex_destroy` | `error = kl_mutex_init(&loader->lock)`、§13.3 で名前 `"settings-look"`、失敗で `kl_mutex_destroy`（形と順は今のまま） |
| join（795・828 行あたり） | `(void)pthread_join(loader->thread, NULL); (void)pthread_mutex_destroy(&loader->lock);` | `(void)kl_thread_join(loader->thread); loader->thread = NULL; kl_mutex_destroy(&loader->lock);` |
| `look_load_run` | `static void *`、`return NULL;` | `static void`、return 無し |
| `clock_gettime(CLOCK_MONOTONIC)`（716 行あたり、log の時間） | — | 変えない |

### 3.2 `storage-scan.h`・`storage-scan.c`（storage の page の走査、`SE_SCAN_THREADS` 本）

| 所 | 前 | 後 |
| --- | --- | --- |
| `storage-scan.h` | `<pthread.h>`、`pthread_mutex_t lock; pthread_cond_t wake; pthread_t threads[SE_SCAN_THREADS];` | `<keiland/keiland_thread.h>`、`struct kl_mutex lock; struct kl_cond wake; struct kl_thread *threads[SE_SCAN_THREADS];` |
| `se_scan_start`（87・111 行あたり） | `(void)pthread_mutex_init`・`(void)pthread_cond_init`、各 worker の `pthread_create(&scan->threads[index], NULL, scan_worker, scan)` | `(void)kl_mutex_init`・`(void)kl_cond_init`（今と同じく見ない）、§13.3 で名前 `"storage-scan"`（全ての worker が同じ名前）。`thread_count` の数え方・1 本も起きなければ止める扱いは今のまま |
| `se_scan_finish`（177 行あたり） | `(void)pthread_join(scan->threads[index], NULL)` | `(void)kl_thread_join(scan->threads[index]); scan->threads[index] = NULL;` |
| `scan_worker` | `static void *`、`return NULL;` | `static void`、return 無し |
| broadcast・wait・signal（151・260・282・436 行あたり、cast の無い呼び方） | `pthread_cond_*` | `kl_cond_*` |
| `SE_SCAN_THREADS`（4） | — | 変えない（数は今のまま、`kl_thread_cpu_count` を使わない） |

### 3.3 `storage-trash.h`・`storage-trash.c`（ごみ箱の量）

| 所 | 前 | 後 |
| --- | --- | --- |
| `storage-trash.h` | `<pthread.h>`、`pthread_mutex_t lock; pthread_t thread;` | `<keiland/keiland_thread.h>`、`struct kl_mutex lock; struct kl_thread *thread;` |
| 起動（95・100 行あたり） | `(void)pthread_mutex_init`、`pthread_create(&trash->thread, NULL, trash_worker, trash)`、失敗で `pthread_mutex_destroy` | `(void)kl_mutex_init`、§13.3 で名前 `"storage-trash"`、失敗で `kl_mutex_destroy` |
| join（156 行あたり） | `pthread_join`・`pthread_mutex_destroy` | `(void)kl_thread_join(trash->thread); trash->thread = NULL; kl_mutex_destroy(&trash->lock);` |
| `trash_worker` | `static void *`、`return NULL;` | `static void` |

## 4. 確かめの command（§13.5）

1. `sh plan/ws327/tests/build-zedbsd.sh p011 amd64 "settings"`、同 `arm64`、`riscv64` → 各 `make=0 warnings=0`。
2. Linux: `make -j16 -f userland/desktop/keiland-linux.mk KEILAND_LINUX_BUILD=build/ws327-p011-linux build/ws327-p011-linux/lib/libkeiland.so build/ws327-p011-linux/bin/settings > build/ws327-p011-linux.log 2>&1; echo status=$?; grep -c 'warning:' build/ws327-p011-linux.log`
   → `status=0` と `0`。
3. `python3 -I plan/ws327/tests/check-layers.py userland/desktop/settings/` → PASS。
4. `sh plan/ws327/tests/check-base.sh` → PASS。

## 5. 突然変異の確かめ（§13.6）

Settings の thread の source に host 試験は無い。

1. `storage-scan.c` の `kl_cond_broadcast` を 1 つ `pthread_cond_broadcast` に戻す（git の外）→ check-layers の T1 が FAIL。
2. 戻して `cmp`。見た目の page・storage の page の動き（走査の終わり、ごみ箱の量）は p016 の QEMU。

## 6. 注意

- `settings.h` は Settings の多くの file が include する: `<pthread.h>` を外した後、他の file が pthread の物を使っていないか
  （`grep -n pthread userland/desktop/settings/`）を確かめる。
- `look.c` の「lock は running の間だけ有る」約束（settings.h の comment）は今のまま。
- 閉じた WS の試験 `plan/ws089/tests/host-wallpaper.sh`・`host-build.sh` は `look.c` を `-pthread` だけで build する（読むだけ、直さない）。
  壊れるかを流して見て（`sh plan/ws089/tests/host-wallpaper.sh` が `build/ws089/` に書く）、結果に「Q1 へ: 閉じた WS の試験が壊れた／壊れない」を書く。

## 7. 受け入れ（Q1 が見る）

§4 の 1〜4 が PASS、§5 の 1 が捕まる、規約の見直し、`git commit -m WIP -- userland/desktop/settings/ plan/ws327/phase011/phase.md plan/ws327/ws.md`。

## 結果

（実装の担当が書く）
