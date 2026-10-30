<!-- awesome-plan project=zedbsd record=ws327-p008 -->

# ws327-p008 Mailer の移行の残り（sync.c）と Mail の host 試験

- 親: [WS327](../ws.md)。設計: [design.md](../design.md) §6.2・§13。担当: 実装の担当（Sonnet 5.5 high）。依存: p007。
- Status: planned
- Disposition: normal

## 1. 読む物

- `AGENTS.md` §4・§7・§9・§10、`plan/coding-style.md` の全文（特に §14 の checklist）。
- design.md の §4.3（network の約束、特に poll・waker・send・receive）、§6.2（Mailer）、§13（共通の約束: 凍結・写しの表・起動の形・command・
  突然変異・終わりの条件）。§13 はこの phase.md の一部として従う。
- header（仕様）: `userland/desktop/include/keiland/keiland_thread.h`・`keiland_socket.h`（変えない）。
- 途中まで移した source（6bb8c3c8d、P2、host の compile は通るが未試験）: `userland/desktop/mailer/conn.c`・`tls.c`・`imap.c`・`mail.h`。
  読んで §6.2 と合っているか確かめる（食い違いは直してよい。直した点は結果に書く）。
  P2 の確かめ（p007）: host-mail-backend.c の IDLE の待ちだけを仮に `kl_socket_native` の fd の poll にした写しを、層の 2 file と
  `-std=gnu17` で build し fake-mail-server.py に対して流すと `host-mail-backend: PASS`（50 件、TLS・STARTTLS・IDLE・SMTP）。

## 2. 作る物・直す物（触ってよい file はこれだけ）

| file | 何を |
| --- | --- |
| `userland/desktop/mailer/sync.c` | 下の §3 の対応表どおりに移す |
| `userland/desktop/mailer/conn.c`・`tls.c`・`imap.c`・`mail.h` | 確かめて、誤りがあれば直す（6bb8c3c8d の続き） |
| `plan/tools/mail/run-host-mail-backend.sh` | §4.1 |
| `plan/tools/mail/host-mail-backend.c` | §4.2 |
| `plan/tools/mail/host-mail-sync.c` | §4.3（main で既に壊れている: 6a53bce99 で `ml_sync_start` の引数が変わり、`ml_sync_fd` が無くなった） |
| `plan/ws327/phase008/phase.md` の「結果」、`plan/ws327/ws.md` の p008 の行 | 記録 |

Mailer の Makefile（`userland/desktop/mailer/Makefile`）は libkeiland を既に `REQUIRE` に持つので変えない（ws.md の古い記述の
「Makefile 3 本」は誤り。Mailer は zedBSD だけで build する）。

## 3. sync.c の前後の対応

| 所 | 前 | 後 |
| --- | --- | --- |
| include | `<fcntl.h>`・`<poll.h>`・`<pthread.h>`、`<unistd.h>`（pipe の read・write・close） | `<keiland/keiland_socket.h>`・`<keiland/keiland_thread.h>`。`<unistd.h>` は他に使う物が無ければ外す |
| 冒頭の comment | 「The window wakes the thread through a pipe (a byte for each job)」 | 「through a waker (kl_socket_waker; rings merge until the thread's wait finds them)」の意味に直す |
| `struct ml_sync` | `pthread_t thread; int wake[2]; pthread_mutex_t lock;` | `struct kl_thread *thread; struct kl_socket_waker *waker; struct kl_mutex lock;`（`started` は残す）。構造体の comment の「its pipe」を「its waker」に |
| `ml_sync_start` | `wake[0] = wake[1] = -1`、`(void)pthread_mutex_init`、`pipe`・`fcntl(O_NONBLOCK)`、`pthread_create(sync_run)` | `kl_mutex_init` の失敗は `free(sync)` して返す（他を作る前）。`kl_socket_waker_open(&sync->waker)` の失敗は `ml_sync_stop(sync)` して返す（今の pipe の失敗と同じ）。thread は §13.3 の形で名前 `"mail-sync"` |
| `ml_sync_stop` | `write(wake[1], 's')`、`pthread_join`、`close(wake[0..1])`、`pthread_mutex_destroy` | `kl_socket_waker_ring(sync->waker)`、`kl_thread_join(sync->thread)`、`if (sync->waker != NULL) kl_socket_waker_close(sync->waker)`、`kl_mutex_destroy` |
| `ml_sync_queue` | `write(wake[1], 'j')` | `kl_socket_waker_ring(sync->waker)`（comment「a full pipe already wakes it」は「rings merge」に） |
| `sync_run` | `static void *sync_run(void *)`、`return NULL;` | `static void sync_run(void *argument)`、return 無し（前方の宣言も直す） |
| `sync_wait` | `struct pollfd watched[1 + ML_ACCOUNTS_MAX]`、`watched[0].fd = wake[0]`、各 account の `imap.conn.fd` を POLLIN、`poll(watched, count, timeout)`、`ready < 0` は errno、wake の read、`revents & (POLLIN \| POLLHUP \| POLLERR)` | `struct kl_socket_wait watched[1 + ML_ACCOUNTS_MAX]`（`memset` で 0）、`watched[0].waker = sync->waker; .events = KL_SOCKET_READABLE`、各 account は `.socket = sync->accounts[index].imap.conn.socket; .events = KL_SOCKET_READABLE`、`error = kl_socket_poll(watched, count, timeout, &found)`（`size_t found`）、誤りはその値を返す、wake の read は消す（poll が waker を空にする）、`found == 0` が時間切れ、各 session は `(watched[index].ready & (KL_SOCKET_READABLE \| KL_SOCKET_FAILED)) == 0U` なら飛ばす |
| lock の所（`sync_take`・`sync_post` など全て） | `pthread_mutex_lock`・`unlock` | `kl_mutex_lock`・`kl_mutex_unlock` |

- `conn.socket` を直に見る（`imap.c` の `ml_imap_close` と同じ形）。design §6.2 の「ml_conn の関数に」は、field を直に見る形に決めた
  （関数を足さない）。
- 時間（`SYNC_IDLE_MS`・`SYNC_RETRY_MS`、`ml_conn_ready` で 0）は今のまま。

## 4. host 試験の直し

### 4.1 `run-host-mail-backend.sh`

- 2 つの build の両方: `-std=gnu99` を `-std=gnu17` に（層の source は C11 の `<stdatomic.h>`・`_Static_assert`。Mailer の source は
  zedBSD で clang の既定 gnu17）、`-Iuserland/desktop/include` を足し、source に
  `userland/desktop/libkeiland/platform/thread-posix.c userland/desktop/libkeiland/platform/socket-posix.c` を足す。`-pthread` は残す。
- 他は変えない（`mktemp -d` の出力先も今のまま）。

### 4.2 `host-mail-backend.c`

- `test_imap` の IDLE の待ち: `struct pollfd` と `imap.conn.fd` を、`struct kl_socket_wait`（`.socket = imap.conn.socket`、
  `.events = KL_SOCKET_READABLE`）と `kl_socket_poll(&watched, 1U, 5000, &found)` に。`ready` は「`ml_conn_ready` か、poll が 0 を返し
  `found > 0`」。`<poll.h>` を外し `<keiland/keiland_socket.h>` を足す。
- 他に `conn.fd` を見る所が無いか grep し、あれば同じく直す。

### 4.3 `host-mail-sync.c`

- `ml_sync_start(NULL, 0U, &sync)` を `ml_sync_start(NULL, 0U, NULL, NULL, &sync)` に（ready が NULL なら sync は窓に知らせない:
  `sync_post` の `sync->ready != NULL`）。
- `test_wait_for` の「次の wake」: `struct pollfd` と `ml_sync_fd`（もう無い）を消し、`kl_thread_sleep(100U)` に（100 ms ごとに
  `ml_sync_take` を見る、今と同じ刻み）。`<poll.h>` を外し `<keiland/keiland_thread.h>` を足す。

## 5. 確かめの command（§13.5）

1. `sh plan/tools/mail/run-host-mail-backend.sh build/ws327-p008-mail` → exit 0（backend の各 check と sync の各 check が PASS。
   `host-mail-*` が出す行を読む）。
2. `sh plan/ws327/tests/build-zedbsd.sh p008 amd64 "mailer"`、同 `arm64`、`riscv64` → 各 `make=0 warnings=0`。
3. `python3 -I plan/ws327/tests/check-layers.py userland/desktop/mailer/` → `ws327-check-layers: PASS`。
4. `sh plan/ws327/tests/check-base.sh` → `ws327-check-base: PASS`。

## 6. 突然変異の確かめ（§13.6、それぞれ 1 の試験が FAIL か時間切れになる事）

1. `ml_sync_queue` の `kl_socket_waker_ring` を消す → sync の試験の sign-in が `TEST_WAIT_MS`（10 s）で来ない。
2. `sync_wait` の session の判定から `KL_SOCKET_READABLE` を外す（FAILED だけ）→ sync の試験の `arrived` が来ない。
3. `conn.c` の読みの前の待ちの events を `KL_SOCKET_WRITABLE` にする → backend の試験の読みが時間切れか誤り。
4. 各々の後に元に戻し `cmp` で確かめる。見逃しがあれば結果に書く（試験の追加は Q1 に頼む: 試験の file の変更は §2 の 3 つだけ）。

## 7. 注意

- waker は poll が空にする。`ring` は 1 回分にまとまるので、sync の thread は poll から返った後に job の list を見る（`sync_run` は既に
  そうしている。順を変えない）。
- `kl_socket_poll` の waits は socket と waker のどちらか一方だけを持つ（両方・どちらも無いは EINVAL）。`memset` で 0 にしてから入れる。
- `ml_conn` の `socket` は閉じた後 NULL（`ml_conn_close`）。IDLE の account は開いている（`idling`）。
- Mailer の network の I/O は全て sync の thread（kl_thread）の上: SIGPIPE は止まっている（TLS の library の write は EPIPE で返る）。

## 8. 受け入れ（Q1 が見る）

§5 の 4 つが PASS、§6 の 3 つが捕まる、規約の見直し、`git commit -m WIP -- userland/desktop/mailer/ plan/tools/mail/run-host-mail-backend.sh
plan/tools/mail/host-mail-backend.c plan/tools/mail/host-mail-sync.c plan/ws327/phase008/phase.md plan/ws327/ws.md`。
QEMU の Mailer（fake-mail-server での接続・IDLE・送信）は p016。

## 結果

（実装の担当が書く）
