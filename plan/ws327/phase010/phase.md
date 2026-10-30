<!-- awesome-plan project=zedbsd record=ws327-p010 -->

# ws327-p010 Photos の移行（縮小の thread と取り込みの thread）

- 親: [WS327](../ws.md)。設計: [design.md](../design.md) §1.1・§6.1・§6.3・§13。担当: 実装の担当（Sonnet 5.5 high）。依存: p007。
- Status: cleared（2026-10-30 Q1 の判定、ws.md の表）
- Disposition: normal

## 1. 読む物

- `AGENTS.md` §4・§7・§9・§10、`plan/coding-style.md` の全文。
- design.md の §3.2（thread の約束: 静的な条件変数が無い理由、`kl_once`）、§6.3、§13（共通の約束、特に §13.4 の静的な条件変数の形）。
- header（仕様、変えない）: `userland/desktop/include/keiland/keiland_thread.h`。

## 2. 触ってよい file

| file | 何を |
| --- | --- |
| `userland/desktop/photos/thumbs.c` | §3.1 |
| `userland/desktop/photos/media-worker.c` | §3.2 |
| `plan/ws327/phase010/phase.md` の「結果」 | 記録（`plan/ws327/ws.md` は触らない: 並べた Phase の merge が衝突するので Q1 が直す） |

Photos の Makefile は libkeiland を既に `REQUIRE` に持つので変えない。

## 3. 前後の対応（§13.2 の表を全ての `pthread_*` に。以下はその上で直す所）

### 3.1 `thumbs.c`（縮小の thread）

| 所 | 前 | 後 |
| --- | --- | --- |
| include | `<pthread.h>` | `<keiland/keiland_thread.h>`（`<keiland/keiland.h>` を既に include していれば足さない） |
| `struct worker` | `pthread_mutex_t lock; pthread_cond_t wake; pthread_t thread;` | `struct kl_mutex lock; struct kl_cond wake; struct kl_thread *thread;` |
| `static struct worker worker` | `.lock = PTHREAD_MUTEX_INITIALIZER, .wake = PTHREAD_COND_INITIALIZER` | `.lock = KL_MUTEX_INIT`（`.wake` は書かない。0 の byte のまま、§13.4 で作る） |
| 足す | — | §13.4 の `worker_once`・`worker_wake_error`・`worker_init`・`worker_ready`（file-scope の変数と static の関数の規約の comment と前方の宣言） |
| `ph_worker_start` | `pthread_create(&worker.thread, NULL, worker_main, NULL)` | 先に `error = worker_ready(); if (error != 0) return error;`。thread は §13.3、名前 `"photos-thumbs"`、引数 NULL |
| `ph_worker_stop` | signal・`pthread_join` | `kl_cond_signal`・`(void)kl_thread_join(worker.thread); worker.thread = NULL;`（`running` の時だけ、今と同じ。running なら ready は 0） |
| `ph_worker_queue`・`ph_worker_take` | `pthread_cond_signal(&worker.wake)`（start の前にも呼ばれうる） | `worker_ready()` が 0 の時だけ `kl_cond_signal`（0 でなければ thread は無く、待つ者も無い）。`worker_ready()` は lock の前に呼び、結果を局所の変数に |
| `worker_main` | `static void *worker_main(void *unused)`、`return NULL;`、`pthread_cond_wait` | `static void worker_main(void *unused)`、`kl_cond_wait` |

### 3.2 `media-worker.c`（取り込みの thread）

| 所 | 前 | 後 |
| --- | --- | --- |
| include | `<pthread.h>`、`<sys/socket.h>`（Wayland の fd の `shutdown`） | `<keiland/keiland_thread.h>`。`<sys/socket.h>` と `shutdown`・`dup` は**残す**（Wayland の接続を他の thread から切る手。network の層の物ではない。check-layers の許可の行がある） |
| `struct media_worker` | `pthread_mutex_t lock; pthread_cond_t wake; pthread_t thread;` | `struct kl_mutex lock; struct kl_cond wake; struct kl_thread *thread;` |
| `static struct media_worker worker` の初期化子（並びの順の形） | `{PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0, {0}, ...}` | `{KL_MUTEX_INIT, {{{0}}}, NULL, {0}, ...}`（cond は 0 の byte、§13.4 で作る。他の field の値と順は今のまま） |
| 足す | — | §13.4 の形（名前は `media_once`・`media_wake_error`・`media_init`・`media_ready` など、`thumbs.c` と別の file なので同じ名前でもよいが、読み分けのため別の名前に） |
| `ph_media_worker_start` | `pthread_create(..., media_main, NULL)` | 先に `media_ready()`（0 でなければ返す）、§13.3、名前 `"photos-import"` |
| `ph_media_worker_stop` | `pthread_mutex_lock`（cast 無し）・`shutdown`・`pthread_cond_signal`・`pthread_join` | `kl_mutex_lock`・`shutdown`（残す）・`kl_cond_signal`・`(void)kl_thread_join(worker.thread); worker.thread = NULL;` |
| `media_queue` | `pthread_cond_signal` | `media_ready()` が 0 の時だけ `kl_cond_signal` |
| `media_main` | `static void *media_main(void *unused)`、`pthread_cond_wait` | `static void`、`kl_cond_wait` |

## 4. 確かめの command（§13.5）

1. `sh plan/ws327/tests/build-zedbsd.sh p010 amd64 "photos"`、同 `arm64`、`riscv64` → 各 `make=0 warnings=0`。
2. `python3 -I plan/ws327/tests/check-layers.py userland/desktop/photos/` → PASS（`media-worker.c` の `<sys/socket.h>` は許可の行で通る）。
3. `sh plan/ws327/tests/check-base.sh p010` → PASS。
4. host の compile の確かめ（Photos は Linux で build しないので、文法と型の確かめとして）:
   `for f in thumbs media-worker; do cc -std=gnu17 -D_GNU_SOURCE -Wall -Wextra -Werror -fsyntax-only -I. -Iuserland/desktop/include userland/desktop/photos/$f.c || echo FAIL $f; done`
   （移す前の tree で通る事を P2 が確かめた。この確かめは補助）。

## 5. 突然変異の確かめ（§13.6）

Photos に host 試験は無い。

1. `thumbs.c` の `kl_cond_wait` を 1 つ `pthread_cond_wait` に戻す（git の外で）→ check-layers の T1 が FAIL（と build も失敗）。
2. `ph_worker_queue` の `worker_ready()` の判定を逆にする → 結果に「host 試験が無いので捕まらない、p016 の QEMU の縮小で見る」と書く。
3. 戻して `cmp`。縮小・取り込みの動き（start の前の要求を含む）はこの Phase では**未試験**と結果に書く。p016 の QEMU が見る。

## 6. 注意

- `worker` の初期化子の `.wake` を書かない（指示の初期化子は書かない field を 0 にする）。`KL_MUTEX_INIT` は静的な記憶だけ。
- `ph_worker_queue` は start の前にも呼ばれる（縮小の要求が先に来る）。今は 0 の byte の cond に signal していた（POSIX の実装次第で
  未定義）。新しい形では ready で守る。
- `worker_ready()` を lock の中で呼んでも dead lock はしない（`kl_once` は別の物）が、規約の「条件の中で関数を呼ばない」に従い局所の変数で。
- `media-worker.c` の `shutdown(worker.cancel, SHUT_RDWR)` は Wayland の fd の複製を切る手で、残す。

## 7. 受け入れ（Q1 が見る）

§4 の 1〜3 が PASS、§5 の 1 が捕まる、規約の見直し、`git commit -m WIP -- userland/desktop/photos/thumbs.c userland/desktop/photos/media-worker.c
plan/ws327/phase010/phase.md`（ws.md は Q1）。

## 結果

2026-10-30 10:05（実装の担当）

流した command と最後の行:

- `sh plan/ws327/tests/build-zedbsd.sh p010 amd64 "photos"` → `zedbsd amd64 make=0 warnings=0`。arm64・riscv64 も同じ（`make=0 warnings=0`）。
  3 つとも `build/ws327-p010-<arch>/bin/photos` ができた。最後の整形（comment の追加）の後に amd64 を再 build して `make=0 warnings=0`。
- `python3 -I plan/ws327/tests/check-layers.py userland/desktop/photos/` → `ws327-check-layers: PASS`（T1 0・N1 0）。
- host の `cc -std=gnu17 -D_GNU_SOURCE -Wall -Wextra -Werror -fsyntax-only`（thumbs・media-worker）→ どちらも通った。
- `sh plan/ws327/tests/check-base.sh p010` → `ws327-check-base: PASS`。

突然変異（git の外、控えを取り `cmp` で戻した）:

1. `thumbs.c` の `kl_cond_wait` を `pthread_cond_wait`（cast 付き）に戻す → check-layers が `T1 3 found` で FAIL、amd64 の build も `make=2`。捕まった。
2. `ph_worker_queue` の signal の判定を `ready != 0` に逆にする → build は `make=0 warnings=0`、check-layers は PASS。host 試験が無いので
   捕まらない（予想どおり）。p016 の QEMU の縮小で見る。
3. 戻して `cmp` で一致を確認。

自分で決めた点:

- build の worktree: `build/` が無かったので、`NoctLang`・`distfiles`・`llvm`・`llvm-build`・`llvm-source`・`sources` を main の `build/` への
  symlink（読むだけ）、`host-noct-state`・`tmp`・`<arch>` は worktree の実 directory にした（`build/<arch>` は link にしていない）。
- `ph_worker_take` も start の前に呼びうるので、`ph_worker_queue` と同じく lock の前に `worker_ready()` を局所の変数に取り、0 の時だけ signal。
- `media_queue` は `running` を確かめた後（start が `media_ready` を済ませた後）に `media_ready()` を呼び、0 の時だけ signal。
- `thumbs.c` の `#include <keiland/keiland_thread.h>` は `<fcntl.h>` の後の字順に置いた（`app.h` は keiland_thread.h を含まない）。
- 初期化子: thumbs は `.wake` を書かず `.lock = KL_MUTEX_INIT` のみ。media-worker は `{KL_MUTEX_INIT, {{{0}}}, NULL, {0}, ...}`
  （他の field は今のまま。`-Wall -Wextra -Werror` で missing-braces は出なかった）。

未実施・未試験:

- 縮小・取り込みの動き（start の前の要求、`kl_cond_wait`、join の順序を含む）は host でも QEMU でも**未試験**。p016 の QEMU が見る。
- Linux の build は Photos が対象外なので流していない。

Q1 へ: 他の worktree（p008・p009）が `build/llvm-source` を自前で build していた（共有の symlink が無かったため）。上の symlink の形を
共有すると無駄が減る。
