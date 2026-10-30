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
| `plan/ws327/phase011/phase.md` の「結果」 | 記録（`plan/ws327/ws.md` は触らない: 並べた Phase の merge が衝突するので Q1 が直す） |

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
4. `sh plan/ws327/tests/check-base.sh p011` → PASS。

## 5. 突然変異の確かめ（§13.6）

Settings の thread の source に host 試験は無い。

1. `storage-scan.c` の `kl_cond_broadcast` を 1 つ `pthread_cond_broadcast` に戻す（git の外）→ check-layers の T1 が FAIL。
2. 戻して `cmp`。見た目の page・storage の page の動き（走査の終わり、ごみ箱の量）はこの Phase では**未試験**と結果に書く。p016 の QEMU が見る。

## 6. 注意

- `settings.h` は Settings の多くの file が include する: `<pthread.h>` を外した後、他の file が pthread の物を使っていないか
  （`grep -n pthread userland/desktop/settings/`）を確かめる。
- `look.c` の「lock は running の間だけ有る」約束（settings.h の comment）は今のまま。
- 閉じた WS の試験 `plan/ws089/tests/host-wallpaper.sh`・`host-build.sh` は `look.c` を `-pthread` だけで build する（読むだけ、直さない）。
  壊れるかを流して見て（`sh plan/ws089/tests/host-wallpaper.sh` が `build/ws089/` に書く）、結果に「Q1 へ: 閉じた WS の試験が壊れた／壊れない」を書く。

## 7. 受け入れ（Q1 が見る）

§4 の 1〜4 が PASS、§5 の 1 が捕まる、規約の見直し、`git commit -m WIP -- userland/desktop/settings/ plan/ws327/phase011/phase.md`（ws.md は Q1）。

## 結果

2026-10-30 10:09 実装の担当（Sonnet 5.5 high）。

- 変えた file: `settings.h`・`look.c`・`storage-scan.h`・`storage-scan.c`・`storage-trash.h`・`storage-trash.c`。pthread の呼び出しは Settings の全 source から消えた
  （`grep -n pthread userland/desktop/settings/*.c *.h` は 0 行。Makefile 3 本の `-pthread` は残した）。名前は `settings-look`・`storage-scan`（4 本とも）・`storage-trash`。
- 流した command と最後の行:
  - `sh plan/ws327/tests/build-zedbsd.sh p011 amd64|arm64|riscv64 "settings"` → `zedbsd amd64 make=0 warnings=0`・`zedbsd arm64 make=0 warnings=0`・`zedbsd riscv64 make=0 warnings=0`。
  - Linux の make（§4 の 2） → `status=0`、`grep -c 'warning:'` は 0。
  - `python3 -I plan/ws327/tests/check-layers.py userland/desktop/settings/` → `ws327-check-layers: PASS`（T1 0・N1 0）。
  - `git diff --check` → 問題なし。`sh plan/ws327/tests/check-base.sh p011` は commit の後に流した（下の「check-base」）。
- 突然変異（git の外、控えを取り `cmp` で戻りを確認）:
  1. `storage-scan.c` の `kl_cond_broadcast` を 1 つ `pthread_cond_broadcast` に → check-layers `T1 1 found`・FAIL。戻して `cmp` 一致。
  2. `storage-trash.h` の `struct kl_mutex lock;` を `pthread_mutex_t lock;` に → check-layers FAIL（T1）。戻して `cmp` 一致。
  3. `look.c` の `options.run = look_load_run;` を消す → Linux の settings の build が失敗（`-Werror`、error 42 行）。戻して `cmp` 一致。
- 自分で決めた点:
  - `settings.h` は `<keiland/keiland.h>`（`keiland_thread.h` を含む）を既に include するので、`<pthread.h>` を外すだけにして `keiland_thread.h` は足さなかった（§13.2）。`storage-scan.h`・`storage-trash.h` は `settings.h` の中で `keiland.h` より前に include されるので、自分で `<keiland/keiland_thread.h>` を include する。
  - `storage-scan.c` の join の loop は `scan->threads[index] = NULL;` を足すために `{ }` を付けた（付けないと `-Wmisleading-indentation`）。
  - thread の起動の形は §13.3 のとおり（`kl_thread_options` を `memset`、優先度 NORMAL、`stack_size` 0）。`kl_mutex_init` の戻り値は look.c は今までどおり見て、storage-scan・storage-trash は `(void)`。
- 閉じた WS の試験: `sh plan/ws089/tests/host-wallpaper.sh` を流した（`build/ws089-host/` に書く）。**Q1 へ: 壊れた**。link の段で `kl_mutex_*`・`kl_thread_start`・`kl_thread_join`
  などが undefined reference（`look.c` と `storage-trash.c` を `-pthread` だけで build し libkeiland を link しないため）。直さなかった（読むだけ）。
- 未試験: 見た目の page（壁紙の小さな写し）・storage の page（走査の終わり、ごみ箱の量）の動き。p016 の QEMU が見る。FreeBSD の build は範囲の外（流していない）。
- 作業の途中で、Q1 の指示により、worktree の `build/llvm-source`・`build/llvm-build` に途中まで始まった LLVM の build を止め、`build/tmp/stale-llvm/` へ `mv` した（rm していない。Q1 が消す物）。
  `build/{NoctLang,distfiles,llvm,llvm-build,llvm-source,sources}` は共有の tree への symlink にした。`build/amd64`・`build/arm64` は実 directory。

