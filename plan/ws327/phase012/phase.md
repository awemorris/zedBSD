<!-- awesome-plan project=zedbsd record=ws327-p012 -->

# ws327-p012 IME の辞書の thread、libkeiland の picker-model・audio の移行

- 親: [WS327](../ws.md)。設計: [design.md](../design.md) §1.1・§2・§6.1・§6.3・§13。担当: 実装の担当（Sonnet 5.5 high）。依存: p007。
- Status: cleared（2026-10-30 Q1 の判定、ws.md の表）
- Disposition: normal

## 1. 読む物

- `AGENTS.md` §4・§7・§9・§10、`plan/coding-style.md` の全文。
- design.md の §2（library の中も公開の層を使ってよい）、§3.2、§6.3、§13（共通の約束、特に §13.4 の静的な条件変数の形）。
- header（仕様、変えない）: `userland/desktop/include/keiland/keiland_thread.h`。

## 2. 触ってよい file

| file | 何を |
| --- | --- |
| `userland/desktop/ime/ja-user.c` | §3.1 |
| `userland/desktop/libkeiland/ui/picker.h`・`picker-model.c` | §3.2 |
| `userland/desktop/libkeiland/audio/audio.c` | §3.3 |
| `plan/ws327/phase012/phase.md` の「結果」 | 記録（`plan/ws327/ws.md` は触らない: 並べた Phase の merge が衝突するので Q1 が直す） |

libkeiland の Makefile 3 本と IME の Makefile 3 本は変えない（libkeiland の Makefile は凍結。`thread-posix.c` は既に libkeiland に入っている）。

## 3. 前後の対応（§13.2 の表を全ての `pthread_*` に。以下はその上で直す所）

### 3.1 `ime/ja-user.c`（利用者の辞書を書く thread）

| 所 | 前 | 後 |
| --- | --- | --- |
| include | `<pthread.h>` | `<keiland/keiland_thread.h>` |
| `struct user_writer` | `pthread_t thread;` | `struct kl_thread *thread;`（`started` は残す） |
| 静的な lock 2 つ | `static pthread_mutex_t user_writer_lock = PTHREAD_MUTEX_INITIALIZER;`・`user_writer_file_lock` も | `static struct kl_mutex ... = KL_MUTEX_INIT;` |
| 静的な cond | `static pthread_cond_t user_writer_wake = PTHREAD_COND_INITIALIZER;` | `static struct kl_cond user_writer_wake;` と §13.4 の形（`user_writer_once`・`user_writer_wake_error`・`user_writer_init`・`user_writer_ready`） |
| `ja_user_save_later` | `if (!user_writer.started) { ... pthread_create(&user_writer.thread, NULL, user_writer_run, NULL) ... }`、`pthread_cond_signal` | lock の**前**に `ready = user_writer_ready();`。lock の中: `!started` の時、`ready != 0` なら `error = ready`（thread を作らない）、0 なら §13.3 で名前 `"ime-dict-write"`。signal は `error == 0` の時だけ（今と同じ）。`error != 0` の後の「今すぐ書く」の扱いは今のまま |
| `user_writer_flush` | signal・`pthread_join` | `started` の時だけ（今と同じ）`kl_cond_signal`・`(void)kl_thread_join(user_writer.thread); user_writer.thread = NULL;`（started なら ready は 0 だった） |
| `user_writer_run` | `static void *`、`pthread_cond_wait`、`return NULL;` | `static void`、`kl_cond_wait`、return 無し |

- 一度 flush した後も save_later は thread を作り直す（`started` が 0 に戻る）。cond は `kl_once` で一度だけ作るので作り直しても
  init を繰り返さない（design §6.3、review 5）。

### 3.2 `libkeiland/ui/picker.h`・`picker-model.c`（media の picker の縮小の thread）

| 所 | 前 | 後 |
| --- | --- | --- |
| `picker.h` の struct | `pthread_mutex_t lock; pthread_cond_t wake; ... pthread_t thread;` | `struct kl_mutex lock; struct kl_cond wake; ... struct kl_thread *thread;`（`lock_made`・`thread_made` は残す）。`#include <pthread.h>`（24 行）を消す（`<keiland/keiland.h>` を既に include し、それが `keiland_thread.h` を含む） |
| 作る（97〜102 行あたり） | `error = pthread_mutex_init(&picker->lock, NULL)`、`error = pthread_cond_init(&picker->wake, NULL)`、失敗で `(void)pthread_mutex_destroy` | `kl_mutex_init`・`kl_cond_init`（誤りの扱いは今のまま）、`kl_mutex_destroy` |
| 起動（238 行あたり） | `pthread_create(&picker->thread, NULL, model_thread, picker)` | §13.3、名前 `"kl-picker-scan"` |
| 止め（256〜264 行あたり） | lock・signal・unlock・`pthread_join` | `kl_*`、`(void)kl_thread_join(picker->thread); picker->thread = NULL;` |
| 片付け（282・283 行あたり） | `(void)pthread_cond_destroy`・`(void)pthread_mutex_destroy` | `kl_cond_destroy`・`kl_mutex_destroy` |
| `model_thread` | `static void *model_thread(void *data)`、`return NULL;` | `static void model_thread(void *data)` |

### 3.3 `libkeiland/audio/audio.c`（音の stream の lock）

| 所 | 前 | 後 |
| --- | --- | --- |
| include | `<pthread.h>` | `<keiland/keiland_thread.h>` |
| struct（70 行あたり） | `pthread_mutex_t lock;` | `struct kl_mutex lock;` |
| 作る（203 行あたり） | `error = pthread_mutex_init(&stream->lock, NULL);` | `error = kl_mutex_init(&stream->lock);`（誤りの扱いは今のまま） |
| 片付け（253 行あたり） | `(void)pthread_mutex_destroy` | `kl_mutex_destroy` |
| try（357・376・509 行あたり） | `status = pthread_mutex_trylock(&stream->lock);`（0 でなければ取れない） | `status = kl_mutex_try(&stream->lock);`（0 か EBUSY。判定は今のまま） |
| lock・unlock | `pthread_mutex_lock`・`unlock` | `kl_mutex_lock`・`kl_mutex_unlock` |
| `poll`（853 行あたり、Wayland の fd） | — | 変えない（check-layers の許可の行がある） |

## 4. 確かめの command（§13.5）

1. Linux: `make -j16 -f userland/desktop/keiland-linux.mk KEILAND_LINUX_BUILD=build/ws327-p012-linux build/ws327-p012-linux/lib/libkeiland.so build/ws327-p012-linux/libexec/keiland-ime > build/ws327-p012-linux.log 2>&1; echo status=$?; grep -c 'warning:' build/ws327-p012-linux.log`
   → `status=0` と `0`。
2. host 試験: `sh plan/ws327/tests/host-apps.sh build/ws327-p012-linux ime picker` → `ws327-host-apps: PASS`（ime: ws095 の
   host-engine.c の 230 件、picker: ws197 の picker-model-host-test。どちらも ASan・UBSan）。
3. `sh plan/ws327/tests/build-zedbsd.sh p012 amd64 "keiland-ime"`、同 `arm64`、`riscv64` → 各 `make=0 warnings=0`（libkeiland.so も build する）。
4. `python3 -I plan/ws327/tests/check-layers.py userland/desktop/ime/ userland/desktop/libkeiland/` → PASS。
5. `sh plan/ws327/tests/check-base.sh p012` → PASS。

## 5. 突然変異の確かめ（§13.6）

1. `keiui_picker_fini` の止めの `kl_cond_signal`（259 行あたり）を消す → 縮小を作り終えた thread は `kl_cond_wait` に入っているので
   join が戻らず、picker の試験が `timeout 60` で FAIL。（範囲の変化の signal（314 行）を消す壊し方は、試験が start の直後に範囲を書くので
   捕まらない事が多い。design-reviewer #3）
2. `ja-user.c` の `user_writer_flush` の `kl_thread_join` を消す → ime の試験の「the writer wrote 明後日 before the dictionary was freed」
   が FAIL するか、ASan が報告する（時機による。捕まらなければ「見逃し」と書く）。
3. `ja-user.c` の `user_writer_ready` が常に ENOMEM を返す形にする → save_later は同期で書く道に落ち、ime の試験は PASS のまま
   （落ちる道が動く事の確かめ。結果に書く）。
4. `audio.c` の `kl_mutex_try` を 1 つ `pthread_mutex_trylock` に戻す → check-layers の T1 が FAIL（audio.c の動きは p016 の QEMU）。
5. 各々の後に戻し `cmp`。

## 6. 注意

- libkeiland の中の変更は Linux・FreeBSD・zedBSD の全ての libkeiland に入る。Linux の build を必ず流す（FreeBSD は p016）。
- `picker.h` は `internal.h` を include する library の内部の header。公開の header（`include/keiland/`）は変えない。
- `ja-user.c` の 2 つ目の lock（`user_writer_file_lock`）は `ja_user_save`（同期の書き）と thread の書きを分ける。順と範囲は今のまま。
- `kl_cond_wait` の偽の起きは今の `while` で見直される（変えない）。
- 閉じた WS の試験 `plan/ws095/tests/host-engine.sh` は ja-user.c を `-pthread` だけで build するので、移した後は link できない
  （直さない。host-apps.sh の ime が同じ試験を層と一緒に build する）。結果に「Q1 へ: ws095 の host-engine.sh が壊れた（閉じた WS、
  削除の候補）」と書く。

## 7. 受け入れ（Q1 が見る）

§4 の 1〜5 が PASS、§5 の 1・4 が捕まる、規約の見直し、`git commit -m WIP -- userland/desktop/ime/ja-user.c userland/desktop/libkeiland/ui/picker.h
userland/desktop/libkeiland/ui/picker-model.c userland/desktop/libkeiland/audio/audio.c plan/ws327/phase012/phase.md`（ws.md は Q1）。

## 結果

実装の担当、2026-10-30（host の `date`）。

流した command と最後の行:

- Linux: `make -j16 -f userland/desktop/keiland-linux.mk KEILAND_LINUX_BUILD=build/ws327-p012-linux .../lib/libkeiland.so .../libexec/keiland-ime` → `status=0`、`warning:` 0 件。
- host 試験: `sh plan/ws327/tests/host-apps.sh build/ws327-p012-linux ime picker` → `ime: PASS (host-engine: 230 passed, 0 failed)`・`picker: PASS (picker-model-host-test: PASS)`・`ws327-host-apps: PASS`。
- zedBSD: `build-zedbsd.sh p012 {amd64,arm64,riscv64} "keiland-ime"` → 各 `make=0 warnings=0`。
- `python3 -I plan/ws327/tests/check-layers.py userland/desktop/ime/ userland/desktop/libkeiland/` → `ws327-check-layers: PASS`（T1・N1 とも 0）。
- `sh plan/ws327/tests/check-base.sh p012` → `ws327-check-base: PASS`。`git diff --check` は出力なし。

突然変異（git の外で控えを取り、壊し、戻して `cmp`）:

1. `keiui_picker_fini` の止めの `kl_cond_signal` を消す → picker の試験が FAIL（`picker-run: FAIL`）。捕まった。
2. `user_writer_flush` の `kl_thread_join` を消す → ime の試験は PASS のまま（230 件）。**見逃し**（時機による、と phase.md が予告した形。
   ime の試験は join の有無で結果が変わらない時機だった）。
3. `user_writer_ready` が常に ENOMEM を返す → ime の試験は PASS のまま（同期で書く道に落ちても 230 件が通る事を確認）。
4. `audio.c` の `kl_mutex_try` を 1 つ `pthread_mutex_trylock` に戻す → check-layers が `T1 FAIL audio.c:358: pthread_mutex_trylock`。捕まった。
5. 4 つとも戻して `cmp` で一致を確認し、戻した後に host 試験と check-layers を再度流して PASS。

自分で決めた点:

- `ja_user_save_later` の `struct kl_thread_options options` は関数の先頭の宣言に置き、`ready` は lock の前に取る（phase.md の通り）。
  `ready != 0` の時は thread を作らず `error = ready`、後の「今すぐ書く」道は今のまま。
- `user_writer_run` の末尾の comment「Succeeded: every text was written.」は `return NULL;` と一緒に消した（return が無くなったため）。
- `audio.c` は `<keiland/keiland.h>` が既に `keiland_thread.h` を含むが、phase.md の表に従い明示の include に置き換えた。
- `picker.h` の `#include <pthread.h>` は消し、`keiland_thread.h` は足していない（`<keiland/keiland.h>` 経由）。

未実施: audio.c の動き（QEMU）は p016。FreeBSD の build は p016。

Q1 へ: ws095 の `plan/ws095/tests/host-engine.sh` は ja-user.c を `-pthread` だけで build するので、移した後は link できない（閉じた WS、削除の候補）。
host-apps.sh の ime が同じ試験を層と一緒に build する。
