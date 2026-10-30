<!-- awesome-plan project=zedbsd record=ws327-design -->

# WS327 設計: libkeiland の thread（`kl_thread_*`）と network（`kl_socket_*`）の薄い wrapper と app の移行

- 親: [WS327](ws.md)（ws327-p001、2026-10-30 P2）。ユーザーの決定は ws.md の 1・2 の原文。
- 目的: app が pthread・C11 threads・BSD socket・`poll` を直に呼ばず、POSIX（zedBSD・Linux・FreeBSD）と Win32 で同じ interface の
  libkeiland の層を使う形にする（決定 1）。app の対応も入れる。libbrowser は使わない（決定 2）。
- 状態: p001 の設計（design-reviewer の指摘 18 件を反映、§11。Q1 の答えは §12）。p007（2026-10-30）で標準廉価実装手順の委譲の形に
  した: 実装の Phase の共通の約束 §13、p007 の design-reviewer の指摘 §14。Phase の番号は ws.md の表（p004〜p006 は p008〜p015 に分けた）。

## 0. 要約

| 層 | header（新） | 中身 | 実装（POSIX） | KL_VERSION | Phase |
| --- | --- | --- | --- | --- | --- |
| thread | `keiland_thread.h` | thread の起動・join・detach・名前・優先度の hint、mutex、条件変数、once、thread ごとの値、CPU の数、sleep | `platform/thread-posix.c`（pthread） | 103 | p002 |
| network | `keiland_socket.h` | address（IPv4・IPv6）と名前の解決、socket の作成・connect・bind・listen・accept・send・receive・shutdown・close、非 blocking、複数の socket の待ち（`kl_socket_poll`）、待ちを他の thread から起こす waker | `platform/socket-posix.c`（BSD socket・`poll`） | 104 | p003 |

- atomic は C11 の `<stdatomic.h>` をそのまま使う（包まない、§3.6）。
- 方針（WS309・WS314）を保つ: `kl_app_dispatch` は socket を待たない。socket の待ち（`kl_socket_poll`）は app の network の thread の中で使い、
  結果は `kl_app_post` で app の thread に返す。
- Win32 は interface が移せる事を §5 の表で示す（build は範囲の外）。

## 1. 棚卸し（2026-10-30、worktree の c90327de3 で grep。path は `userland/desktop/` から）

grep の語: `pthread_*`・`thrd_`・`mtx_`・`cnd_`・`<threads.h>`・`socket(`・`connect(`・`bind(`・`listen(`・`accept(`・`poll(`・`select(`・
`getaddrinfo`。kl_app を使う program（`kl_app_open` の呼び手）と libkeiland を link する program を見た。

### 1.1 移す物（p008〜p012。p008 Mailer、p009 Music・Video Player・media-app、p010 Photos、p011 Settings、p012 IME・picker・audio）

| program | file | 使っている物 | 移し先 |
| --- | --- | --- | --- |
| Mailer | `mailer/sync.c` | sync の thread（`pthread_create`・`join`）、mutex、窓 → thread の wake の pipe（`pipe`・`fcntl`・`write`・`read`）、IMAP の socket と wake の `poll` | `kl_thread`・`kl_mutex`・`kl_socket_waker`・`kl_socket_poll` |
| Mailer | `mailer/conn.c` | `getaddrinfo`・`socket`・非 blocking の `connect` と `poll`（15 s）・`SO_ERROR`、読みの前の `poll`（30 s）、`read`・`write`・`close` | `kl_socket_resolve`・`kl_socket_open`・`kl_socket_connect`・`kl_socket_poll`・`kl_socket_receive`・`kl_socket_send`・`kl_socket_close` |
| Mailer | `mailer/tls.c` | 静的な mutex（`PTHREAD_MUTEX_INITIALIZER`）、`SSL_set_fd(ssl, fd)` | `KL_MUTEX_INIT`、`kl_socket_native` |
| Music | `music/play.c`・`play.h` | 再生の thread、mutex・条件変数（broadcast）、`nanosleep` | `kl_thread`・`kl_mutex`・`kl_cond`・`kl_thread_sleep` |
| Video Player | `videoplayer/media.c`・`main.c`・`videoplayer.h` | 復号の thread、mutex・条件変数・`pthread_cond_timedwait`（相対の待ちを絶対の時刻に直している）、`nanosleep` | 同、`kl_cond_wait_for` |
| media-app（Music・Video Player が link） | `media-app/avcodec.c` | `pthread_once` | `KL_ONCE_INIT`・`kl_once` |
| Photos | `photos/thumbs.c` | 縮小の thread、静的な mutex と条件変数（`PTHREAD_*_INITIALIZER`、start の前にも queue が signal する） | `kl_thread`・`KL_MUTEX_INIT`・`kl_cond`（`kl_once` で init） |
| Photos | `photos/media-worker.c` | 取り込みの thread、静的な mutex と条件変数（`PTHREAD_*_INITIALIZER`）、Wayland の fd の複製の `shutdown` で取り消し | thread・mutex・cond を移す（条件変数は start で init）。Wayland の fd の `shutdown` は Wayland の接続の扱いで network の層ではないので残す |
| Settings | `settings/look.c`・`settings.h` | 見た目の読みの thread、mutex | `kl_thread`・`kl_mutex` |
| Settings | `settings/storage-scan.c`・`.h` | 走査の thread の組（`SE_SCAN_THREADS` 本）、mutex・条件変数 | 同 |
| Settings | `settings/storage-trash.c`・`.h` | ごみ箱の量の thread、mutex | 同 |
| IME（libkeiland を link） | `ime/ja-user.c` | 利用者の辞書を書く thread、静的な mutex 2 つと条件変数 | `kl_thread`・`KL_MUTEX_INIT`・`kl_cond`（init は最初の起動の時） |
| libkeiland | `ui/picker-model.c`・`picker.h` | media の picker の走査の thread、mutex・条件変数 | `kl_*`（library の中も公開の層を使う） |
| libkeiland | `audio/audio.c` | mutex（`trylock` を含む） | `kl_mutex`・`kl_mutex_try` |
| 解説 | `docs/howto/keiland-examples/08-worker.c`・`08-language.c` | C11 の `thrd_*`・`mtx_*`・`cnd_*`、`<stdatomic.h>` | `kl_thread`・`kl_mutex`・`kl_cond`（atomic はそのまま） |

- Terminal・Installer・Phone・Files・PDF Viewer・Browser の shell は thread・socket を直に使っていない（WS314 で `kl_process`・`kl_launch`・
  `kl_app_post` に移った）。依頼文の「Terminal・Installer・Phone の thread」は 2026-10-30 の tree には無い（grep で 0）。
- Mailer の SMTP・IMAP は `conn.c` の上にあるが、`imap.c:735`（`imap->conn.fd >= 0` で開いているかを見る）と `sync.c:437`（poll に `conn.fd`）が
  fd を直に見る。p008 で `conn.socket` に直す（§6.2）。Mailer の network の I/O（IMAP・SMTP・TLS）は全て sync の thread の上（`sync.c` の job）。
- 既存の host 試験が移す source を `-pthread` だけで link している（p008・p009 で build を直す、§6.4）: `plan/tools/mail/run-host-*.sh`
  （conn.c・sync.c・tls.c、`host-mail-backend.c:379` は `imap.conn.fd` を poll）、`plan/tools/music/host-music-play.sh`（play.c）、
  `plan/tools/media/run-host-codec.sh`（avcodec.c を `-std=c89`）。

### 1.2 移さない物（理由）

| 物 | 理由 |
| --- | --- |
| libbrowser・libmedia | 単体の library（決定 2）。 |
| compositor（`wayland/`）・`libkeiland-backend-*`・sessiond・printd・X server | app ではない（system の部品）。WS327 の所有の外。 |
| libkeiland の `platform/`（`platform-posix.c`・`process-posix.c`）・`instance.c` の UNIX socket・`ui/*.c` の Wayland の fd の `poll` | library の OS の層そのもの。`instance.c` の UNIX socket は同じ program の 2 つの起動の間の local な口で、`kl_socket` は IP の network の層（§4.1）。Win32 では名前付き pipe（WS314 §6）。 |
| `installer/admin/`（root の helper） | app の API ではない別の program（WS314 §11 と同じ）。 |
| `preview/make.c` の `socket(AF_INET)` | sandbox の自己試験（network が閉じている事を確かめる）。I/O ではなく sandbox の一部（Preview の platform の部分）。 |
| `ime/main.c` の `poll`（Wayland の fd と signal の pipe） | IME の自前の loop（WS314 §11）。socket ではない。 |
| `photos/media-worker.c` の Wayland の fd の `dup`・`shutdown` | Wayland の接続を他の thread から切る手。network の層の物ではない。 |
| `userland/tests/*` の network の probe | 試験の program（OS の network の試験）。Keiland の app ではない。 |
| libvulkan・libvulkan-compat・libwayland・libegl・X server の GLX | 描画・Wayland の library（check.sh の `not_applications`）。 |

### 1.3 OS の境界の check（p007 の道具、p016 で check.sh へ）

WS327 の間は `plan/ws327/tests/check-layers.py`（許可の表 `plan/ws327/tests/layers-allow.tsv`、引数で範囲を絞れる）が T1・N1 を見る。p016 で
`plan/tools/keiland-os-boundary/check.sh` に移す案を Q1 に出す（`plan/tools/` は Q1 の範囲）。中身:

- `T1`: app（check.sh の「application」の集合）と libkeiland の `platform/` の外が `pthread_*`・`thrd_*`・`mtx_*`・`cnd_*`・`call_once`・
  `<pthread.h>`・`<threads.h>` を使わない。
- `N1`: 同じ範囲が `socket`・`connect`・`bind`・`listen`・`accept`・`accept4`・`getaddrinfo`・`poll`・`select`・`<sys/socket.h>`・`<netdb.h>`・
  `<poll.h>`・`<netinet/*>`・`<arpa/inet.h>` を使わない。呼び出しは `(?<![\w.>])name\s*\(`（`->select(`・`.select(` の member を除く）。
- libbrowser・libmedia を外すのは T1・N1 だけ（A1〜A5 は今どおり見る）。
- 許す行（`layers-allow.tsv`、理由つき。p016 で check.sh に移す時に `app-allow.tsv` へ）: libkeiland の Wayland の fd の `poll`（`ui/app.c`・`ui/window.c`・`ui/primary.c`・`ui/clipboard.c`・
  `system/system.c`・`audio/audio.c`）、`instance.c`（local な socket）、`preview/`（sandbox）、`ime/main.c`（自前の loop）、
  `photos/media-worker.c`（Wayland の fd の `shutdown` と `<sys/socket.h>`）、libbrowser・libmedia（単体の library: check の集合から外す）。

## 2. 置き場と原則

- 公開の header: `userland/desktop/include/keiland/keiland_thread.h`・`keiland_socket.h`（新）。`keiland.h` が両方を include する。
  header は OS の header（`<pthread.h>`・`<sys/socket.h>`・`<windows.h>`）を include しない。型は不透明（thread・socket・waker は library が
  確保する record の pointer）か、大きさの決まった記憶（mutex・条件変数・once・key: §3.2、埋め込みと静的な初期化のため）。
- 実装: `libkeiland/platform/thread-posix.c`・`socket-posix.c`（新、WS314 と同じ置き場）。Win32 は `thread-win32.c`・`socket-win32.c` を足す（§5）。
- 誤りは errno の値（`<errno.h>` の名前、POSIX の値）で返す。Win32 は Win32・WinSock の誤りを同じ名前の errno の値に写す（§5.3）。
  関数は 0 か誤りの値を返し、成果は引数の pointer で返す（libkeiland の他の関数と同じ形）。
- 呼べる thread: 特に書かない限りどの thread からでも（同じ物を同時に使う時の約束は各節）。`kl_app_*` の「app の thread だけ」とは別。
- library の中（libkeiland の他の部分）も公開の層を使ってよい（`picker-model.c`・`audio.c` を p012 で移す）。`platform.h` の `keiui_*` は
  library の内部の起こし・lock で、WS314 の物をそのまま残す（Win32 の時に `kl_*` の上に作り直せる）。

## 3. thread（`keiland_thread.h`、p002、KL_VERSION 103）

### 3.1 API

```c
#include <stddef.h>
#include <stdint.h>

struct kl_thread;

/* The longest name a thread keeps (bytes, without the terminating NUL); a longer one is cut. */
#define KL_THREAD_NAME_MAX	15U

/* A thread's priority (kl_thread_options' priority): a hint, which a system without priorities ignores. */
#define KL_THREAD_PRIORITY_NORMAL	0U
#define KL_THREAD_PRIORITY_LOW		1U	/* work the user does not wait for (a scan, a thumbnail) */
#define KL_THREAD_PRIORITY_HIGH	2U	/* work that keeps time (sound, a video's pictures) */

struct kl_thread_options {
	const char *name;		/* the thread's name (debuggers, the system's lists); NULL: none */
	void (*run)(void *argument);	/* what the thread runs; the thread ends when it returns */
	void *argument;
	unsigned priority;		/* KL_THREAD_PRIORITY_* */
	size_t stack_size;		/* bytes; 0: the system's */
};

int kl_thread_start(const struct kl_thread_options *options, struct kl_thread **thread);
int kl_thread_join(struct kl_thread *thread);
int kl_thread_detach(struct kl_thread *thread);
int kl_thread_is_current(const struct kl_thread *thread);
void kl_thread_sleep(uint32_t milliseconds);
void kl_thread_yield(void);
unsigned kl_thread_cpu_count(void);

/* The storage of a mutex, a condition and a once (§3.2). */
#define KL_THREAD_STORAGE	64U
#define KL_THREAD_ONCE_STORAGE	32U

struct kl_mutex {
	union {
		unsigned char bytes[KL_THREAD_STORAGE];
		void *pointer;
		long long number;
		double real;
	} opaque;
};

struct kl_cond {
	union {
		unsigned char bytes[KL_THREAD_STORAGE];
		void *pointer;
		long long number;
		double real;
	} opaque;
};

struct kl_once {
	union {
		unsigned char bytes[KL_THREAD_ONCE_STORAGE];
		void *pointer;
		long long number;
	} opaque;
};

struct kl_thread_key {
	union {
		unsigned char bytes[16];
		void *pointer;
		long long number;
	} opaque;
};

/* A mutex of static storage that needs no kl_mutex_init (nor kl_mutex_destroy). */
#define KL_MUTEX_INIT	{ { { 0 } } }
#define KL_ONCE_INIT	{ { { 0 } } }

int kl_mutex_init(struct kl_mutex *mutex);
void kl_mutex_destroy(struct kl_mutex *mutex);
void kl_mutex_lock(struct kl_mutex *mutex);
int kl_mutex_try(struct kl_mutex *mutex);
void kl_mutex_unlock(struct kl_mutex *mutex);

int kl_cond_init(struct kl_cond *cond);
void kl_cond_destroy(struct kl_cond *cond);
void kl_cond_wait(struct kl_cond *cond, struct kl_mutex *mutex);
int kl_cond_wait_for(struct kl_cond *cond, struct kl_mutex *mutex, uint32_t milliseconds);
void kl_cond_signal(struct kl_cond *cond);
void kl_cond_broadcast(struct kl_cond *cond);

void kl_once(struct kl_once *once, void (*run)(void));

int kl_thread_key_create(struct kl_thread_key *key, void (*destroy)(void *value));
void kl_thread_key_delete(struct kl_thread_key *key);
int kl_thread_key_set(struct kl_thread_key *key, void *value);
void *kl_thread_key_get(struct kl_thread_key *key);
```

### 3.2 約束

- **start**: thread を起こし、`*thread` に記録を返す。誤り: EINVAL（`run` が NULL・優先度が範囲の外）、EAGAIN（system の資源）、ENOMEM。
  名前は `KL_THREAD_NAME_MAX` で切る。名前・優先度は付けられない system では黙って付けない（start は失敗しない）。stack の大きさは
  system の最小（`PTHREAD_STACK_MIN`）より小さければ最小に切り上げる（Win32 は reserve の大きさ、`STACK_SIZE_PARAM_IS_A_RESERVATION`）。
  POSIX: thread は非同期の signal を全て止めて始まる（signal は app の thread に届く。WS314 の platform と compositor の `kwl_thread_start` と
  同じ）。同期の fault（SIGSEGV・SIGBUS・SIGFPE・SIGILL・SIGTRAP）は止めない（止めると fault が sanitizer・debugger の処理を通らず SIG_DFL に
  なる、review 12）。止めた SIGPIPE は thread に保留されるだけなので、kl_thread の上の `write` は EPIPE で返る。
- 記録の寿命: 返った thread は **join か detach のどちらか一度だけ**。join は thread の終わりを待って記録を放す（0、EDEADLK: 自分自身）。
  detach は待たず、記録は thread が終わる時に library が放す。どちらの後も pointer は使えない。どちらもしない thread の記録は放されない（漏れ）。
- `run` の戻り値は無い（結果は `argument` の記録で返す）。thread の中から途中で終える関数は無い（`run` から戻る）。
- `kl_thread_is_current`: 呼んだ thread がその thread か（1・0）。
- `kl_thread_sleep`: 少なくともその間眠る（signal で早く起きても残りを眠り直す）。`kl_thread_yield`: 他の thread に譲る。
- `kl_thread_cpu_count`: 使える CPU の数（1 以上。分からなければ 1）。thread の組の数を決める時に使う（Settings の走査）。
- **mutex**: 再帰しない（同じ thread で 2 度 lock は dead lock）。`kl_mutex_try` は 0 か EBUSY。lock・unlock は失敗を返さない（誤った使い方は
  未定義）。`kl_mutex_init` の誤り: ENOMEM・EAGAIN。`KL_MUTEX_INIT` は静的な記憶の mutex（`static struct kl_mutex lock = KL_MUTEX_INIT;`）だけに
  使い、destroy しない。
- **条件変数**: 偽の起き（spurious）は起こりうる（呼び手は条件を loop で確かめる）。`kl_cond_wait_for` は 0（起こされた、または偽の起き）か
  ETIMEDOUT。`UINT32_MAX` ms（約 49 日）も有限の待ち（Win32 は `INFINITE` と同じ値になるので 1 を引いて渡す）。待ちの時計は単調（POSIX: `CLOCK_MONOTONIC` の condattr、時刻の変更に影響されない）。静的な初期化は無い（zedBSD の
  `PTHREAD_COND_INITIALIZER` は 0 でない時計を持ち、単調の時計の設定も要る）。`kl_cond_init` の誤り: ENOMEM・EAGAIN。
- **once**: `kl_once(&once, run)` は process で一度だけ `run` を呼び、他の thread の同時の呼び出しは `run` の終わりを待つ。once は `KL_ONCE_INIT`。
- **thread ごとの値**: `create`（`destroy` は値が NULL でない thread の終わりに呼ばれる。NULL 可。呼ばれる回数は system による: POSIX は
  `PTHREAD_DESTRUCTOR_ITERATIONS` まで繰り返し、Win32 の FLS は 1 回）、`set`（0・ENOMEM）、`get`（無ければ NULL）、`delete`（その時に生きている
  thread の値に destroy が呼ばれるかは**未規定**: POSIX は呼ばず、Win32 の `FlsFree` は呼ぶ。呼び手は delete の前に値を片付ける、review 6）。
  誤り: EAGAIN（key の数の上限）、ENOMEM。
- **記憶の大きさ**: `KL_THREAD_STORAGE`（64）・`KL_THREAD_ONCE_STORAGE`（32）・`KL_THREAD_KEY_STORAGE`（16）は ABI の一部。library は build の時に
  `_Static_assert(sizeof(pthread_mutex_t) <= KL_THREAD_STORAGE)` と alignment を確かめる。大きさの見込み（p002 の build の assert で確かめる）:
  glibc x86_64 の mutex 40・cond 48、aarch64 48・48、FreeBSD は pointer（8）、zedBSD は `<sys/types.h>` の struct（mutex は uint32・pthread_t・
  unsigned 4 つ、cond は 3 語、once は 1 語）、Win32 の SRWLOCK・CONDITION_VARIABLE・INIT_ONCE は pointer（8）。
- **静的な初期化が 0 の byte で済む事**: `PTHREAD_MUTEX_INITIALIZER`・`PTHREAD_ONCE_INIT` は glibc・FreeBSD（NULL）・zedBSD（`{0,...}`）で
  全 byte 0、Win32 の `SRWLOCK_INIT`・`INIT_ONCE_STATIC_INIT` も 0。p002 の host の試験で、各 OS の initializer の byte が 0 である事を確かめる
  （違う OS が出たら `KL_MUTEX_INIT` を使えない OS として遅延の初期化に変える）。

### 3.3 thread の数（WS309 の方針: 名前と数を明示）

kl_thread 自身は thread を作らない（`kl_thread_start` の 1 回が 1 本）。移した app の thread は §6 の表のとおり名前を付ける。

### 3.4 実装（POSIX、`platform/thread-posix.c`）

- 記録: `struct kl_thread { pthread_t thread; void (*run)(void *); void *argument; char name[16]; unsigned priority; atomic_int owners; }`。
  owners は 2（作った側と thread）で始まり、thread の終わりと join・detach で 1 つずつ減らし、0 にした方が放す。join は `pthread_join` の後に放す。
- 起動: `pthread_attr_setstacksize`（0 以外）、全 signal を止めて `pthread_create`（WS314 の `keiui_thread_start` と同じ）。trampoline が
  名前（Linux・FreeBSD の `pthread_setname_np`。zedBSD の libc に無いので `#if`）と優先度（下）を付けて `run` を呼ぶ。
- 起動の mask: 全 signal から同期の fault（SIGSEGV・SIGBUS・SIGFPE・SIGILL・SIGTRAP）を除いた集合を止める。stack は `PTHREAD_STACK_MIN` に切り上げ。
- 優先度: POSIX の `SCHED_OTHER` に priority の幅は無い。Linux は thread ごとの nice（`setpriority(PRIO_PROCESS, gettid(), ±n)`、LOW は +10、
  HIGH は上げる権利が無ければ黙って NORMAL）、FreeBSD は `pthread_setschedparam` の SCHED_OTHER の範囲が 0 だけなので付けない、zedBSD も
  付けない（libc の `pthread_attr_setschedparam` は 0 以外を ENOTSUP）。hint なので失敗しても start は 0。
- mutex: `pthread_mutex_init(NULL attr)`。cond: `pthread_condattr_setclock(CLOCK_MONOTONIC)`、`wait_for` は `clock_gettime(MONOTONIC)` に足した
  絶対の時刻で `pthread_cond_timedwait`。once: `pthread_once`。key: `pthread_key_*`。
- sleep: `nanosleep` を EINTR で残りから繰り返す。cpu: `sysconf(_SC_NPROCESSORS_ONLN)`。
- CPU の指定（affinity）は入れない: 使う app が 0、zedBSD の libc に無い、Win32 と Linux で意味（processor group）が違う。要る時に別の版で足す。

### 3.5 誤りの時の app の扱い

今の app は `pthread_create` の失敗を errno の値で扱っている。`kl_thread_start` も errno の値を返すので扱いは変わらない。

### 3.6 atomic

C11 の `<stdatomic.h>` をそのまま使う（zedBSD・Linux・FreeBSD の clang・gcc、Win32 の clang と MSVC の `/experimental:c11atomics`）。
包むと `_Atomic` の型を隠す事になり、薄い wrapper にならない。解説に書く。

## 4. network（`keiland_socket.h`、p003、KL_VERSION 104）

### 4.1 範囲

- IP（IPv4・IPv6）の TCP（stream）と UDP（datagram）。local な socket（UNIX domain）・raw・multicast・TLS は範囲の外（TLS は app の物、
  Mailer の `tls.c`）。
- socket の概念はそのまま見せる（決定 1）: open・connect・bind・listen・accept・send・receive・shutdown・close、非 blocking、待ち。

### 4.2 API

```c
#include <stddef.h>
#include <stdint.h>

struct kl_socket;
struct kl_socket_waker;

/* An address's family. */
#define KL_ADDRESS_IPV4	4U
#define KL_ADDRESS_IPV6	6U

/* An IP address and port, a value of its own (copied freely). */
struct kl_address {
	unsigned family;		/* KL_ADDRESS_IPV4 or KL_ADDRESS_IPV6 */
	uint16_t port;			/* in the host's byte order */
	uint8_t bytes[16];		/* in the network's order: 4 for IPv4, 16 for IPv6 */
	uint32_t scope;			/* IPv6's scope (an interface's index); 0 for none */
};

/* The longest text kl_address_format writes, with its NUL ("[v6%scope]:port"). */
#define KL_ADDRESS_TEXT	64U

int kl_address_parse(const char *text, uint16_t port, struct kl_address *address);
int kl_address_format(const struct kl_address *address, char *text, size_t size);
void kl_address_any(unsigned family, uint16_t port, struct kl_address *address);
void kl_address_loopback(unsigned family, uint16_t port, struct kl_address *address);

/* A socket's kind. */
#define KL_SOCKET_STREAM	1U	/* TCP */
#define KL_SOCKET_DATAGRAM	2U	/* UDP */

/* kl_socket_open's and kl_socket_accept's flags. */
#define KL_SOCKET_NONBLOCKING	0x1U

/* Names resolved (kl_socket_resolve's families): either family, or one. */
#define KL_ADDRESS_ANY	0U

int kl_socket_resolve(const char *host, uint16_t port, unsigned family, unsigned kind,
    struct kl_address *addresses, size_t capacity, size_t *count);

int kl_socket_open(unsigned family, unsigned kind, unsigned flags, struct kl_socket **socket);
int kl_socket_set_nonblocking(struct kl_socket *socket, int on);
int kl_socket_connect(struct kl_socket *socket, const struct kl_address *address);
int kl_socket_connect_result(struct kl_socket *socket);
int kl_socket_bind(struct kl_socket *socket, const struct kl_address *address);
int kl_socket_listen(struct kl_socket *socket, unsigned backlog);
int kl_socket_accept(struct kl_socket *socket, unsigned flags, struct kl_socket **accepted, struct kl_address *peer);
int kl_socket_send(struct kl_socket *socket, const void *bytes, size_t size, size_t *sent);
int kl_socket_receive(struct kl_socket *socket, void *buffer, size_t size, size_t *received);
int kl_socket_send_to(struct kl_socket *socket, const void *bytes, size_t size, const struct kl_address *to, size_t *sent);
int kl_socket_receive_from(struct kl_socket *socket, void *buffer, size_t size, struct kl_address *from, size_t *received);
int kl_socket_shutdown(struct kl_socket *socket, unsigned how);
int kl_socket_local_address(const struct kl_socket *socket, struct kl_address *address);
int kl_socket_peer_address(const struct kl_socket *socket, struct kl_address *address);
int kl_socket_set_option(struct kl_socket *socket, unsigned option, int value);
intptr_t kl_socket_native(const struct kl_socket *socket);
void kl_socket_close(struct kl_socket *socket);

/* kl_socket_shutdown's how. */
#define KL_SOCKET_SHUT_RECEIVE	1U
#define KL_SOCKET_SHUT_SEND	2U
#define KL_SOCKET_SHUT_BOTH	3U

/* kl_socket_set_option's options. */
#define KL_SOCKET_REUSE_ADDRESS	1U	/* bind a port another socket left in TIME_WAIT (SO_REUSEADDR; Win32: no-op, see §5.2) */
#define KL_SOCKET_NO_DELAY	2U	/* TCP: send small writes at once (TCP_NODELAY) */
#define KL_SOCKET_KEEP_ALIVE	3U	/* TCP: probe an idle connection (SO_KEEPALIVE) */
#define KL_SOCKET_IPV6_ONLY	4U	/* IPv6: no IPv4-mapped peers (IPV6_V6ONLY; 1 when opened) */
#define KL_SOCKET_BROADCAST	5U	/* UDP: send to a broadcast address (SO_BROADCAST) */

/* What a wait waits for (events) and what it found (ready). */
#define KL_SOCKET_READABLE	0x1U	/* a receive or an accept does not block */
#define KL_SOCKET_WRITABLE	0x2U	/* a send does not block; a connect in progress ended (kl_socket_connect_result) */
#define KL_SOCKET_FAILED	0x4U	/* ready only: the socket failed (a connect refused, a reset); kl_socket_connect_result says why */

/* One thing a wait waits for: a socket or a waker (exactly one of the two). */
struct kl_socket_wait {
	struct kl_socket *socket;
	struct kl_socket_waker *waker;
	unsigned events;		/* KL_SOCKET_READABLE | _WRITABLE (a waker: READABLE) */
	unsigned ready;			/* written by kl_socket_poll */
};

/* No limit to a wait (kl_socket_poll's timeout). */
#define KL_SOCKET_FOREVER	(-1)

int kl_socket_poll(struct kl_socket_wait *waits, size_t count, int milliseconds, size_t *ready);

int kl_socket_waker_open(struct kl_socket_waker **waker);
void kl_socket_waker_ring(struct kl_socket_waker *waker);
void kl_socket_waker_close(struct kl_socket_waker *waker);
```

### 4.3 約束

- **address**: `parse` は数字の address（"192.0.2.1"・"2001:db8::1"・"fe80::1%2"、括弧無し）を読む（0・EINVAL）。名前は読まない（`resolve`）。
  `format` は "192.0.2.1:993"・"[2001:db8::1]:993"（0・ENOSPC）。`any`・`loopback` はその family の 0.0.0.0・::・127.0.0.1・::1。
  family が 4・6 以外の address は EAFNOSUPPORT。
- **resolve**: 名前（か数字の address）を `kind` の socket の address の列に解く（`getaddrinfo` の順のまま、最大 `capacity`、`*count` は書いた数）。
  **blocking**（app の thread から呼ばない。network の thread で）。誤り: ENOENT（名前が無い）、EAGAIN（一時の失敗、後で再び）、EAFNOSUPPORT、
  ENOMEM、EIO（その他）。`capacity` が 0 は EINVAL。
- **open**: family・kind の socket を作る（close-on-exec、子の process に受け継がれない）。IPv6 は `KL_SOCKET_IPV6_ONLY` を 1 で開く（OS の既定が
  Linux・zedBSD の 0 と FreeBSD・Win32 の 1 で違うので揃える）。SIGPIPE は出ない（§4.4）。誤り: EAFNOSUPPORT（その family が無い system）、
  EMFILE・ENFILE、ENOMEM、EACCES。
- **connect**: blocking の socket は繋がるまで待つ（0 か ECONNREFUSED・ETIMEDOUT・ENETUNREACH・EHOSTUNREACH など）。非 blocking は
  EINPROGRESS を返し、`kl_socket_poll` の WRITABLE（か FAILED）を待ってから `kl_socket_connect_result` が 0 か誤りを返す（POSIX の `SO_ERROR`）。
  `connect_result` は connect が終わる前に呼んでも「まだ」を区別できない（`SO_ERROR` は 0）ので、必ず待ちの後に呼ぶ。
  非 blocking の connect の途中でもう一度 `kl_socket_connect` を呼ぶのは EALREADY（呼び手は待つ）。UDP の connect は相手を決めるだけ。
- **bind・listen**: errno の値（EADDRINUSE・EADDRNOTAVAIL・EACCES）。listen の backlog 0 は system の既定。
- **accept**: 接続を 1 つ取り、`flags`（`KL_SOCKET_NONBLOCKING`）で新しい socket を作る（受け継ぎの違い（Linux は非 blocking を受け継がず BSD・
  Win32 は受け継ぐ）を flag で揃える）。`peer` は NULL 可。非 blocking の listen の socket で接続が無ければ EAGAIN。取る前に消えた接続
  （ECONNABORTED）と Linux が accept に渡す保留中の network の誤り（EPROTO・ENETDOWN・ENETUNREACH・EHOSTUNREACH・EHOSTDOWN・ENONET・
  ENOPROTOOPT・EOPNOTSUPP）は library の中で次の接続を取り直す（blocking は待ち、非 blocking は無ければ EAGAIN）。
- **send・receive**: `*sent`・`*received` に byte の数。非 blocking で進めなければ EAGAIN（0 byte。EWOULDBLOCK は返さない）。receive の 0（`*received == 0`、戻り値 0）は
  相手が送りを閉じた（stream の終わり）。send は相手が閉じていれば EPIPE（SIGPIPE 無し）、ECONNRESET。blocking の send は全部送るとは限らない
  （呼び手が残りを送る）。EINTR は library の中で繰り返し、返さない。
- **send_to・receive_from**: datagram。1 回が 1 つの datagram。0 byte の datagram は正当で、受けた 0 は stream の終わりの意味を持たない。buffer より長い datagram は切られ、`*received` は buffer の大きさ（切られた事は
  分からない、POSIX と Win32 の違いを避けて切るだけ）。
- **shutdown**: 送り・受けを閉じる。ENOTCONN（繋がっていない）。
- **local_address・peer_address**: bind・connect の後の address。ENOTCONN。
- **set_option**: 0 か ENOPROTOOPT（その種類の socket に無い）・EINVAL。
- **native**: OS の handle（POSIX は fd、Win32 は `SOCKET`）。**TLS の library（OpenSSL の `SSL_set_fd` など）に渡すためだけ**。待ちに使わない、
  閉じない（`kl_socket_close` が閉じる）。Mailer の `tls.c` が使う。native を経た I/O（TLS の library の `write`）の SIGPIPE は呼び手の責任
  （review 3）: kl_thread の上なら SIGPIPE は止まっていて EPIPE で返る。Mailer の network の I/O は全て sync の thread（kl_thread）の上。
  Win32 の OpenSSL の `SSL_set_fd` は int で受けるが、SOCKET の値は実際には 32 bit に収まる（OpenSSL 自身が同じ cast をする）。
- **close**: socket を閉じ記録を放す。同じ socket を他の thread が待ち・送り・受け中に閉じない（呼び手の責任。fd の番号の再利用を避ける）。
- **同時の使用**: 1 つの socket を 2 つの thread で同時に使うのは、片方が送り・片方が受けの時だけ（POSIX と WinSock で安全な範囲）。
- **poll**: `waits` の各 socket・waker のどれかが用意できるか時間が切れるまで待つ。`milliseconds` は 0（待たない）・正・`KL_SOCKET_FOREVER`。
  返る時に各 wait の `ready` を書き、`*ready` は ready が 0 でない wait の数（0 は時間切れ）。
  - ready の意味: READABLE（receive・accept が block しない: byte、stream の終わり、誤り、接続の要求）、WRITABLE（send が block しない、
    非 blocking の connect が終わった）、FAILED（socket が失敗した: connect の拒否・reset。events に関わらず立つ）。
    **ready は「events との積」に FAILED を足した物**（review 2）: POSIX の POLLIN → READABLE、POLLOUT → WRITABLE、POLLHUP（両方向が閉じた）→
    events の bit 全て（receive は 0 か誤り、send は EPIPE で、どちらも block しない）、POLLERR・POLLNVAL → FAILED。FAILED を受けた呼び手は
    receive か `connect_result` で誤りを取る（Mailer は `READABLE | FAILED` で読みに進む、§6.2）。
  - waker の wait は鳴っていれば READABLE を返し、**poll が waker を空にする**（同じ鳴りが 2 度報告されない）。約束: poll が返る前に済んだ ring は
    その poll の READABLE に含まれうり、poll が waker を空にした後の ring は次の poll を起こす（失われない）。よって呼び手は **poll が返った後に
    自分の queue（ring の前に書いた物）を見る**。
  - EINTR は残りの時間で繰り返す（返さない）。誤り: EINVAL（socket と waker の両方・どちらも無い wait、count が 0 で時間が FOREVER）、ENOMEM。
  - `count` が 0 なら時間だけ眠る（`KL_SOCKET_FOREVER` は EINVAL）。
- **waker**: 他の thread から poll を起こす物。`ring` はどの thread からも呼べ、何度鳴らしても 1 回分にまとまる（poll が空にするまで）。
  close は誰も待っていない時に。1 つの waker を同時に 2 つの poll に入れない。

### 4.4 実装（POSIX、`platform/socket-posix.c`）

- 記録: `struct kl_socket { int fd; int family; int kind; }`、`struct kl_socket_waker { int ends[2]; atomic_int rung; }`。
- address の写し: `sockaddr_in`・`sockaddr_in6`（`sin6_scope_id`）、parse は `inet_pton`（"%scope" は数字か `if_nametoindex`）、format は `inet_ntop`。
- open: `socket(family, type | SOCK_CLOEXEC | (nonblocking ? SOCK_NONBLOCK : 0), 0)`（3 OS に有り）。IPv6 は `IPV6_V6ONLY` を 1（zedBSD は
  bind の前だけ変えられる。open の直後なので足りる）。`SO_NOSIGPIPE` の有る system（FreeBSD）は付ける（native を経た `write` のため）。
  Linux・zedBSD に無いので native の I/O は kl_thread の上で（§4.3）。
- connect: `connect`、EINTR は blocking の時に `poll` で終わりを待って `SO_ERROR`（POSIX の connect の EINTR の後は再び呼べないため）。
  `connect_result`: `getsockopt(SO_ERROR)`。
- accept: `accept4(SOCK_CLOEXEC | (flags ? SOCK_NONBLOCK : 0))`。
- send: `send(MSG_NOSIGNAL)`、receive: `recv`。send_to・receive_from: `sendto`・`recvfrom`。
- poll: `struct pollfd` の配列（count が 32 以下は stack、超えれば malloc）、`poll`。EINTR は単調の時計で残りを計って繰り返す。
  waker は `ends[0]` を POLLIN で入れ、POLLIN なら読み切ってから rung を 0 にし、READABLE。
- waker: `pipe2(O_CLOEXEC | O_NONBLOCK)`。ring は rung を 0 → 1 にした時だけ 1 byte を書く。poll は**先に読み切り、後で rung を 0 に戻す**
  （review 1: 逆の順だと、戻した直後の ring の byte を読み切りが食べ、rung が 1 のまま pipe が空になり以後の ring が何も書かず永久に起きない）。
  読み切りと戻しの間の ring は rung が 1 なので書かず、この poll の返りに含まれる（呼び手は返った後に queue を見る）。
- resolve: `getaddrinfo`（`AI_ADDRCONFIG` は付けない: loopback だけの guest で IPv6 の loopback が解けなくなる）、`EAI_NONAME`・`EAI_NODATA` →
  ENOENT、`EAI_AGAIN` → EAGAIN、`EAI_FAMILY` → EAFNOSUPPORT、`EAI_MEMORY` → ENOMEM、`EAI_SYSTEM` → errno、他 → EIO。

## 5. Win32 への写し方（interface が移せる事の確かめ。build は範囲の外）

### 5.1 thread

| kl | POSIX | Win32 |
| --- | --- | --- |
| `kl_thread_start` | `pthread_create`（全 signal を止めて）、trampoline | `_beginthreadex`（CRT の初期化のため `CreateThread` でなく）、trampoline。record の `HANDLE` と id |
| 名前 | `pthread_setname_np`（Linux・FreeBSD）、zedBSD は付けない | `SetThreadDescription`（UTF-8 → UTF-16。Windows 10 1607 以降、無ければ付けない） |
| 優先度 | Linux の thread の nice、他は付けない | `SetThreadPriority`（LOW: `THREAD_PRIORITY_BELOW_NORMAL`、HIGH: `THREAD_PRIORITY_ABOVE_NORMAL`） |
| stack | `pthread_attr_setstacksize`（最小に切り上げ） | `_beginthreadex` の大きさと `STACK_SIZE_PARAM_IS_A_RESERVATION`（reserve として） |
| `kl_thread_join` | `pthread_join` | `WaitForSingleObject(INFINITE)`・`CloseHandle` |
| `kl_thread_detach` | `pthread_detach` | `CloseHandle`（thread は動き続ける） |
| `kl_thread_is_current` | `pthread_equal(pthread_self())` | `GetCurrentThreadId() == id` |
| `kl_thread_sleep`・`yield` | `nanosleep`・`sched_yield` | `Sleep`・`SwitchToThread` |
| `kl_thread_cpu_count` | `sysconf(_SC_NPROCESSORS_ONLN)` | 呼び手の thread の processor group の数（`GetActiveProcessorCount(現在の group)`。thread は既定で 1 つの group で走るので全 group の数は多すぎる） |
| mutex | `pthread_mutex_t`（再帰しない） | `SRWLOCK`（exclusive、再帰しない。`SRWLOCK_INIT` は 0 の byte） |
| `kl_mutex_try` | `pthread_mutex_trylock` → EBUSY | `TryAcquireSRWLockExclusive` → EBUSY |
| 条件変数 | `pthread_cond_t`（CLOCK_MONOTONIC） | `CONDITION_VARIABLE`、`SleepConditionVariableSRW`（ms は相対なので `wait_for` がそのまま写る。`UINT32_MAX` は 1 引く。`ERROR_TIMEOUT` → ETIMEDOUT） |
| once | `pthread_once` | `InitOnceExecuteOnce`（callback の引数で `run` を渡す） |
| thread ごとの値 | `pthread_key_*`（終わりの destroy） | `FlsAlloc`・`FlsSetValue`・`FlsGetValue`（TLS の `TlsAlloc` は destroy を呼ばないので FLS）。FLS の callback には値だけが渡り key は渡らないので、各 thread の値を小さな record（`destroy` と値）に包んで FLS に置き、library の callback（NTAPI）が record の `destroy` を呼んで record を放す。key の 16 byte は FLS の index と `destroy`（record を作る時に写す）。`set` は初めての時に record を確保する（ENOMEM、header の約束の内）。`FlsFree` は生きた値に callback を呼ぶ（§3.2 の未規定）。p007 で直した（元の「key に index と destroy を入れ trampoline で」は callback が key を知らず成り立たない） |
| signal | thread は全 signal を止めて始まる | signal の概念が無い（何もしない） |

### 5.2 network

| kl | POSIX | Win32（WinSock 2） |
| --- | --- | --- |
| 初めての使用 | — | `WSAStartup(2.2)` を `kl_once` で一度（`WSACleanup` は呼ばない: process の終わりに OS が片付ける） |
| `kl_socket_open` | `socket(SOCK_CLOEXEC [| SOCK_NONBLOCK])` | `WSASocketW(..., WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT)`（overlapped でない socket は同期の file object で、一つの thread の blocking の recv が他の thread の send を待たせうる）、非 blocking は `ioctlsocket(FIONBIO)`。UDP は `SIO_UDP_CONNRESET` を切る（繋いでいない UDP で Windows だけが ICMP の到達不能を WSAECONNRESET にする） |
| SIGPIPE | `MSG_NOSIGNAL`（と FreeBSD の `SO_NOSIGPIPE`） | 無い |
| `connect`（非 blocking） | EINPROGRESS | `WSAEWOULDBLOCK` を EINPROGRESS に写す |
| `connect_result` | `getsockopt(SO_ERROR)` | 同（`SO_ERROR`） |
| `accept` | `accept4(SOCK_CLOEXEC [| SOCK_NONBLOCK])` | `accept`（受け継ぐ非 blocking を flags に合わせ `ioctlsocket` で直す、handle の継承を `SetHandleInformation` で切る） |
| send・receive | `send(MSG_NOSIGNAL)`・`recv` | `send`・`recv`（大きさは int: `INT_MAX` で切って短く送る） |
| shutdown | `SHUT_RD`・`SHUT_WR`・`SHUT_RDWR` | `SD_RECEIVE`・`SD_SEND`・`SD_BOTH` |
| close | `close` | `closesocket` |
| `KL_SOCKET_REUSE_ADDRESS` | `SO_REUSEADDR` | 何もしない（Windows の `SO_REUSEADDR` は使用中の port を奪える別の意味。TIME_WAIT の port は既定で bind できる）。`SO_EXCLUSIVEADDRUSE` は付けない |
| `KL_SOCKET_IPV6_ONLY` | `IPV6_V6ONLY` | 同 |
| resolve | `getaddrinfo` | `GetAddrInfoW`（UTF-8 → UTF-16）、`WSAHOST_NOT_FOUND` → ENOENT、`WSATRY_AGAIN` → EAGAIN |
| address の text | `inet_pton`・`inet_ntop` | `InetPtonW`・`InetNtopW`（同じ形） |
| **poll** | `poll` | **`select`**（下） |
| waker | `pipe2` と印 | loopback の TCP の組（127.0.0.1 の port 0 に `SO_EXCLUSIVEADDRUSE` で listen → connect → accept、listen の socket は閉じる）と印。UDP は落ちうる（落ちると印が 1 のまま永久に鳴らない）ので使わない。ring は 1 byte の `send`（非 blocking）、poll は読み切ってから印を下ろす（§4.4 と同じ順） |
| native | fd（`int`） | `SOCKET`（`UINT_PTR`、`intptr_t` に入る） |

- **poll の写し（WSAPoll の不具合を補う）**: Windows 10 2004 より前の `WSAPoll` は、非 blocking の connect が失敗した socket を
  POLLERR・POLLHUP で報告せず時間切れまで待つ（Microsoft が認めた不具合）。Win32 の `kl_socket_poll` は `select` で作る: READABLE を
  readfds、WRITABLE を writefds、全ての socket を exceptfds に入れる。失敗した connect は exceptfds に出る（→ FAILED）。Win32 の `fd_set` は
  数の配列（fd の値の上限は無い）なので、count に合わせて `fd_count` と配列を malloc した物を `fd_set *` として渡す（`FD_SETSIZE` の 64 に縛られない）。
  時間は `timeval`。`select` は 1 つも socket が無いと WSAEINVAL なので、count 0 は `Sleep`。waker は loopback の socket なので同じ集合に入る。
  （代案: `WSAPoll` を使い、connect 中の socket が 1 つでもある時だけ `select` に切り替える。どちらも interface は同じ。）
- 「どれかが失敗した connect」の判定は `kl_socket_connect_result`（`SO_ERROR`）で両方同じ。

### 5.3 誤りの値

POSIX の errno の値をそのまま返す。Win32 は `GetLastError`・`WSAGetLastError` を、MSVC の `<errno.h>` の POSIX の補い（`ECONNREFUSED` など、
VS 2010 以降）の値に写す表を `socket-win32.c` に持つ（`WSAEWOULDBLOCK` → EAGAIN、`WSAEINPROGRESS`（WinSock 1.1 の blocking の呼び出しの途中で、非 blocking の connect の意味ではない）→ EALREADY、`WSAECONNREFUSED` →
ECONNREFUSED、`WSAECONNRESET` → ECONNRESET、`WSAETIMEDOUT` → ETIMEDOUT、`WSAENETUNREACH` → ENETUNREACH、`WSAEHOSTUNREACH` → EHOSTUNREACH、
`WSAEADDRINUSE` → EADDRINUSE、`WSAEADDRNOTAVAIL` → EADDRNOTAVAIL、`WSAEAFNOSUPPORT` → EAFNOSUPPORT、`WSAEMFILE` → EMFILE、`WSAENOTCONN` →
ENOTCONN、`WSAESHUTDOWN` → EPIPE、`WSAENOBUFS` → ENOMEM、`WSAEALREADY` → EALREADY、`WSAECONNABORTED` → ECONNABORTED、`WSAENETRESET` →
ECONNRESET、`WSAENETDOWN` → ENETDOWN、`WSAEHOSTDOWN` → EHOSTUNREACH（mingw の errno.h に EHOSTDOWN が無い）、`WSAEACCES` → EACCES、`WSAEINVAL` → EINVAL、`WSAEISCONN` → EISCONN、他 → EIO）。`recvfrom` の
`WSAEMSGSIZE` は誤りにせず「切った datagram」として buffer の大きさを返す（§4.3）。MSVC・mingw の errno.h では EAGAIN と EWOULDBLOCK が別の値
（11・140）なので、library は EAGAIN だけを返す（header に「EWOULDBLOCK と比べない」と書く）。

## 6. app の移行（p008〜p012）

### 6.1 thread の名前と数（WS309 の方針）

| program | thread（名前） | 数 | 何で止まって待つか |
| --- | --- | --- | --- |
| Mailer | `mail-sync` | 窓 1 つに 1（今と同じ） | `kl_socket_poll`（IDLE の各 account の socket と waker）、時間切れ |
| Music | `music-play` | 再生中に 1 | `kl_thread_sleep` で回る（音の stream の空き。条件変数は止めの broadcast だけで、待つ者は無い） |
| Video Player | `video-decode` | 開いた動画 1 つに 1 | `kl_cond_wait_for`（絵の空き・要求、`MEDIA_WAIT_MS`） |
| Photos | `photos-thumbs`・`photos-import` | 1・1 | `kl_cond_wait` |
| Settings | `settings-look`・`storage-scan`（`SE_SCAN_THREADS` 本）・`storage-trash` | 開いた page の分 | `kl_cond_wait`・file の I/O |
| IME | `ime-dict-write` | 書く物がある間 1 | `kl_cond_wait` |
| libkeiland | `kl-picker-scan` | picker の走査中に 1 | `kl_cond_wait` |

数は今の source の数と同じ（移すだけ）。名前は p008〜p012 で各 thread の `kl_thread_options.name` に入れる。

### 6.2 Mailer（network）

- `conn.c`: `ml_conn.fd` を `struct kl_socket *socket` に。`ml_conn_open` は `kl_socket_resolve(host, port, KL_ADDRESS_ANY, STREAM, 16 個)` →
  各 address で `kl_socket_open(family, STREAM, NONBLOCKING)` → `kl_socket_connect` → EINPROGRESS なら `kl_socket_poll(WRITABLE, 15 s)` →
  `kl_socket_connect_result` → `kl_socket_set_nonblocking(0)`（今と同じく接続の後は blocking で、読みの前に待つ）。`conn_fill` は
  `kl_socket_poll(READABLE, timeout)` と `kl_socket_receive`、`ml_conn_write` は `kl_socket_send` の繰り返し。`ml_conn_close` は `kl_socket_close`。
  host の名前が無い時の ENOENT、時間切れの ETIMEDOUT は今と同じ値。今は getaddrinfo の失敗を全て ENOENT（「the server's name was not
  found」）にしているので、`ml_conn_open` は `kl_socket_resolve` の EAGAIN・EIO も ENOENT に寄せる（画面の文を変えない、review 10）。
  `conn_fill` は `READABLE`（FAILED は events に関わらず来る）を待ち、ready が 0 でなければ読みに進む（今の「poll が 0 より大きければ read」と同じ）。
- `tls.c`: `ml_tls_open(int fd, ...)` を `ml_tls_open(intptr_t handle, ...)` に（`SSL_set_fd` に `(int)handle`、Win32 の OpenSSL も int で受ける）。
  `tls_lock` を `KL_MUTEX_INIT`。
- `sync.c`: thread と mutex を `kl_*` に、wake の pipe を `kl_socket_waker`（ring は `ml_sync_add` と stop）、`sync_wait` の `poll` を
  `kl_socket_poll`（waker と IDLE の各 socket の READABLE。ready が READABLE か FAILED の socket を今の `POLLIN | POLLHUP | POLLERR` と同じに
  扱う）。`conn.fd` を直に見る所（`sync.c:437`・`imap.c:735`）は `conn.socket` を直に見る（p007 で決めた: `imap.c` が既にその形、関数を足さない）。結果を窓へ返すのは今どおり `ready(context)` → `kl_app_post`（WS314）。
- `mail.h` の `struct ml_conn`・宣言を直す。Mailer の source から `<poll.h>`・`<netdb.h>`・`<sys/socket.h>`・`<pthread.h>`・`<fcntl.h>`（wake の
  非 blocking の分）が消える。

### 6.3 thread だけの app

pthread の型と呼び出しを 1 対 1 で写す（`pthread_mutex_lock` → `kl_mutex_lock` など）。直す所:

- 静的な条件変数（Photos の thumbs と media-worker、IME）: `kl_once` で一度だけ `kl_cond_init`（thumbs は start の前にも queue が signal し、
  IME は flush の後に thread を作り直すので、start ごとの init は誤り、review 5）。once の中の init の失敗は印に残し、使う側は今の失敗の扱い
  （thread を作れない時の扱い: IME は同期で書く、Photos は縮小を出さない）に落とす。
- `pthread_cond_timedwait` の絶対の時刻（Video Player）: 相対の `kl_cond_wait_for(MEDIA_WAIT_MS)` に（計算を消す）。
- `nanosleep`（Music・Video Player）: `kl_thread_sleep`。
- `pthread_create` の `void *(*)(void *)` の関数を `void (*)(void *)` に（`return NULL` を消す）。
- thread の join の有無は今のまま（detach する所は今は無い）。優先度は全て `KL_THREAD_PRIORITY_NORMAL`（動きを変えない、review 17）。

### 6.4 確かめ（p008〜p012 の各 Phase と p016）

- build warning 0: zedBSD amd64・arm64・riscv64（移した program と libkeiland）、Linux（`keiland-linux.mk`）、FreeBSD（P1 の guest を overlay して
  backend-test.sh）。
- `plan/tools/keiland-os-boundary/check.sh` の T1・N1 が 0 件（許す行を除く）。
- 既存の host 試験の build を直す: `plan/tools/mail/run-host-*.sh`・`plan/tools/music/host-music-play.sh`・`plan/tools/media/run-host-codec.sh` に
  `platform/thread-posix.c`・`socket-posix.c` を加え（`-std=c89` の avcodec は header が C89 で読めるか確かめる）、`host-mail-backend.c` の
  `imap.conn.fd` を直す（Q1 が q1697 の範囲に足した、§12）。流して PASS を確かめる。
- QEMU（`plan/ws314/tests/guest.sh` の形、cache=unsafe）: Mailer（既存の `plan/tools/mail/fake-mail-server.py`（IMAP4・SMTP、TLS・STARTTLS、
  `--arrivals`）を host で立て、guest の Mailer が接続・IDLE・新着の受け取り・送信）、Music・Video Player の再生の開始・停止、
  Photos の縮小、Settings の storage の page、Terminal の起動（回帰）。画面の PNG と SSH での program の終了の status で判じる（console log は読まない）。

## 7. 解説（p015）

- `docs/howto/keiland-programming.md` 8 章: `08-worker.c`・`08-language.c` を `kl_thread`・`kl_mutex`・`kl_cond` に（`<stdatomic.h>` は残す）。
  「The threads are standard C11 threads」の文を「libkeiland's threads (`keiland_thread.h`)」に、FreeBSD の C11 threads の注意を外す。
- 8 章に節「Talking over the network」と例 `08-network.c` を足す: 窓が network の thread を 1 本起こし、thread は loopback に listen する
  socket と、それに非 blocking で connect する socket を `kl_socket_poll` で同時に待ち（waker で Stop を受ける）、往復した行と時間を
  `kl_app_post` で窓に返す。外の network が無い guest でも動き、`doc-examples.sh` の compile と guest の写真で確かめる。
  規則: socket の待ちは app の thread でしない、結果は post、waker で止める、close は thread の join の後。
- 例の確かめ: `plan/ws314/tests/doc-examples.sh`（compile・link・本文と file の一致、guest の写真）。

## 8. 試験（p002・p003）

| Phase | 試験 | 中身 |
| --- | --- | --- |
| p002 | `plan/ws327/tests/thread-test.c`（Linux の host、clang・gcc、`-fsanitize=thread` の build でも） | 8 thread × 100000 の mutex の加算、条件変数の生産と消費（1 生産・4 消費、数の一致）、`wait_for` の時間切れ（50 ms ± の範囲）、once を 8 thread から（1 回）、key の destroy が thread の終わりに呼ばれる、detach の thread の記録が放される（ASan の build で漏れ 0）、join の EDEADLK、`KL_MUTEX_INIT`・`KL_ONCE_INIT` の静的な使用、名前（Linux は `pthread_getname_np` で読み返す）、`cpu_count >= 1`、各 OS の `PTHREAD_MUTEX_INITIALIZER`・`PTHREAD_ONCE_INIT` の byte が 0 |
| p003 | `plan/ws327/tests/socket-test.c`（同） | address の parse・format（v4・v6・scope・誤り）、resolve（"localhost"・数字・無い名前 → ENOENT（`.invalid`））、TCP の loopback の往復（IPv4・IPv6、blocking と非 blocking）、非 blocking の connect の成功（WRITABLE → result 0）と失敗（閉じた port → FAILED か WRITABLE → ECONNREFUSED）、accept の flags、`kl_socket_poll` の複数（16 socket の中の 1 つだけ readable）、時間切れ、waker（他の thread の ring で FOREVER の poll が返る、鳴りの合流、空にした後は鳴らない）、相手の close で receive 0、send の EPIPE（SIGPIPE で死なない）、UDP の send_to・receive_from、IPV6_ONLY の既定 1、TSan の build で waker と poll の競合 |
| p003 | zedBSD の上（QEMU） | thread-test・socket-test を zedBSD 用に build して guest で走らせる（`plan/ws327/tests/guest-layers.sh`、image は `config-amd64-layers.mk`。2026-10-30 thread 27・socket 53 PASS。zedBSD の image に `/etc/hosts` が無いので script が loopback の行を足す）。p003 の受け入れの条件（zedBSD に固有の振る舞い: 失敗した非 blocking の connect は SYN_SENT の間だけ POLLOUT・CLOSED では POLLERR だけ、SIGPIPE は thread 宛て、IPV6_V6ONLY は bind の前だけ、review 11） |
| p003 | waker の stress（review 1） | N 本の thread が「counter を増やして ring」を繰り返し、FOREVER の poll の側が返るたびに counter を読み、期限内に最終値に届く（届かなければ鳴りが失われた） |

FreeBSD は P1 の guest（`/home/awe/zedBSD-worktrees/p1-ws309i/build/ws309/freebsd/guest/guest.qcow2` を書き込まずに overlay）で
libkeiland の build と同じ試験を走らせる（backend-test.sh の形）。

## 9. KL_VERSION と ABI

- 103: thread（`keiland_thread.h`）。104: network（`keiland_socket.h`）。どちらも Q1 が割り当てた（2026-10-30、次の番号は 105）。
- 足すだけ（既存の型・関数の変更なし）。exports.map は `exports.py` で作り直す。
- `struct kl_mutex` などの大きさ（§3.2）と `struct kl_address`・`struct kl_socket_wait` の形は ABI の一部。

## 10. 範囲の外

- CPU の指定（affinity）、thread の取り消し、読み書きの lock、barrier、semaphore（使う app が無い）。
- TLS、UNIX domain socket、multicast、raw socket、socket の時間切れの option（`SO_RCVTIMEO`、待ちは poll で）。
- libbrowser・libmedia・compositor・`libkeiland-backend-*`・sessiond・printd。
- Win32 の build（interface が移せる事を §5 で示すだけ）。ただし host に mingw-w64 と wine が有るので、`thread-win32.c`・`socket-win32.c` を
  書いて mingw で cross compile まで（p013・p014、ws.md の決定 3。wine での動作確認はしない）。
- libkeiland の `platform.h` の `keiui_*` を `kl_*` の上に作り直すこと（Win32 に移す時に）。

## 11. design-reviewer の指摘と反映（2026-10-30、18 件）

| # | 重さ | 指摘 | 反映 |
| --- | --- | --- | --- |
| 1 | high | waker を「rung を 0 → 読み切り」の順にすると鳴りが永久に失われる。試験で見つからない | 読み切り → rung 0 の順（§4.4、実装もこの順）、約束に「返った後に queue を見る」（§4.3）、stress の試験（§8） |
| 2 | medium | POLLERR を FAILED だけにすると Mailer が reset の socket を読まず busy loop、ready と events の関係が未定 | ready は events との積 + FAILED、POLLHUP・POLLNVAL を定義（§4.3）、Mailer は READABLE か FAILED で読む（§6.2） |
| 3 | medium | 「Mailer は SIGPIPE を無視する」は誤り、TLS の native の write で SIGPIPE | native の I/O は呼び手の責任、kl_thread は SIGPIPE を止める、Mailer の I/O は sync の thread だけ（§3.2・§4.3・§4.4） |
| 4 | medium | `imap.c`・`sync.c` が `conn.fd` を直に見る、既存の host 試験（mail・music・media）が壊れる、fake-mail-server.py が既にある | §1.1・§6.2・§6.4 に追加 |
| 5 | medium | Photos の thumbs も静的な cond、start の前の signal、IME の作り直しで再 init | `kl_once` で init（§1.1・§6.3） |
| 6 | medium | `FlsFree` は生きた値に callback を呼ぶ、NTAPI の規約 | delete の時の destroy は未規定、trampoline（§3.2・§5.1） |
| 7 | medium | Win32 の誤りの表の欠け（EALREADY・ECONNABORTED・WSAEMSGSIZE ほか）、EAGAIN と EWOULDBLOCK、UDP の reset | §5.3・§5.2 |
| 8 | medium | `WSA_FLAG_OVERLAPPED` が無いと送りと受けが直列になる | §5.2 |
| 9 | medium | host の mingw・wine で Win32 を確かめられる | §12 で Q1 に問う |
| 10 | low-medium | 解決の誤りの写しで Mailer の表示が変わる、EAI_NODATA | Mailer は ENOENT に寄せる（§6.2）、`#ifdef EAI_NODATA`（実装済み） |
| 11 | low-medium | zedBSD での socket の確かめが p004 まで遅れる | p003 の受け入れに zedBSD の guest の実行（§8） |
| 12 | low-medium | 全 signal の mask が同期の fault まで止める | fault の signal を mask から外す（§3.2・§3.4）。WS314 の `keiui_thread_*` は同じ形のまま（範囲の外） |
| 13 | low | stack の約束と実装の食い違い、Win32 の reserve | 最小に切り上げ（§3.2・§5.1） |
| 14 | low | Win32 の waker の UDP は落ちうる、port の奪取 | TCP の組と `SO_EXCLUSIVEADDRUSE`（§5.2） |
| 15 | low | check の正規表現の誤検出、libbrowser を集合ごと外すと A1〜A5 が弱まる、§1.2 の追記 | §1.2・§1.3 |
| 16 | low | 0 byte の datagram、負の ms、Linux の accept の保留の誤り、`wait_for(UINT32_MAX)`、Win32 の CPU の数 | §4.3・§3.2・§5.1（負の ms は -1 の他は EINVAL、実装済み） |
| 17 | low | 移した thread の優先度 | 全て NORMAL（§6.3） |
| 18 | low | 104 は未割当、key の大きさの名前 | §9・§3.2 |

## 12. Q1 に問う事と答え（2026-10-30）

答え（Q1）: 1. 104 を network の層に使ってよい（master の次の番号は 105）。2. ユーザーの決定（ws.md の 3）で、Win32 の実装を書き mingw で
cross compile（warning 0）まで（wine の試験はしない）を p006 として行う。3. `plan/tools/mail/run-host-*.sh`・`host-mail-backend.c`、`plan/tools/music/host-music-play.sh`、
`plan/tools/media/run-host-codec.sh` の link と、thread・socket の移しに要る最小の部分の変更を q1697 の範囲に足す（Q1 の明示）。
`host-mail-backend.c` の `imap.conn.fd` を直に poll する所は新しい口に合わせて直してよい。

問い（元の文）:

1. **KL_VERSION 104** の割り当て（network の層）。103 は thread の層に使った。
2. **Win32 の cross の確かめ**（review 9、範囲の変更）: host の `x86_64-w64-mingw32-gcc` と `wine` で `thread-win32.c`・`socket-win32.c` を
   compile（warning 0）し、thread-test・socket-test を wine で走らせる（Windows そのものではない参考の証拠）。推し: p005 の後に別の Phase
   （p006）で。interface を変えずに足せる。
3. **`plan/tools/` の既存の host 試験の修正**（§6.4）: `plan/tools/mail/`・`plan/tools/music/`・`plan/tools/media/` は所有の外。p004 で
   移した source を link し直す修正を担当がしてよいか。

## 13. 実装の Phase の共通の約束（p008〜p015・p017、標準廉価実装手順 §4.6）

実装の担当（Sonnet 5.5 high）は自分の phase.md とこの節だけを手順として読む。この WS は外部の標準仕様の実装ではない（「仕様」は
§3・§4 と header の comment、Win32 は Microsoft の文書）。phase.md と header が食い違えば header に従い、食い違いを報告する。

### 13.1 変えてはいけない物（凍結）

- `userland/desktop/include/keiland/` の全ての header（`keiland_thread.h`・`keiland_socket.h` は BASE）、`libkeiland/exports.map`・`exports.py`、
  `libkeiland/platform/thread-posix.c`・`socket-posix.c`、libkeiland の Makefile 3 本、この design.md、`plan/ws327/tests/` の全て（試験の道具）。
  `sh plan/ws327/tests/check-base.sh <phase>` が branch の分岐点（`git merge-base HEAD main`）と比べて照合し、phase.md の §2 の外の file の変更も
  見つける（design-reviewer #4・#7）。
- HAL・toolchain・共有の `build/`・main の checkout・他の WS の file（`plan/ws*/` の他の WS の試験も読むだけ。`plan/tools/` は phase.md が
  名指す file だけ）。
- header・凍結の file・試験の道具に不備を見つけたら、直さずに止めて、どの file のどこで何が足りないかを報告する。
- 移しで振る舞いを変えない: 待ちの時間・thread の数・join の有無・誤りの値と画面の文・log の文字列は今のまま（例外は phase.md が書く物だけ）。

### 13.2 pthread から kl_* への写し（1 対 1）

| 前 | 後 | 注意 |
| --- | --- | --- |
| `#include <pthread.h>` | `#include <keiland/keiland_thread.h>`（`<keiland/keiland.h>` を既に include する file は足さなくてよい） | 他の用途の include（`<time.h>` など）は、使う物が残る間は残す |
| `pthread_t x;` | `struct kl_thread *x;` | join の後に `x = NULL;` |
| `pthread_mutex_t` ／ `pthread_cond_t` | `struct kl_mutex` ／ `struct kl_cond` | 構造体に埋め込みのまま |
| `PTHREAD_MUTEX_INITIALIZER` | `KL_MUTEX_INIT` | 静的な記憶だけ。destroy しない |
| `PTHREAD_COND_INITIALIZER` | 無い（§13.4 の形で一度だけ `kl_cond_init`） | 静的な条件変数は 0 の byte では使えない（§3.2） |
| `pthread_once_t o = PTHREAD_ONCE_INIT; pthread_once(&o, f)` | `static struct kl_once o = KL_ONCE_INIT; kl_once(&o, f)` | |
| `(void)pthread_mutex_init(&m, NULL)` | `kl_mutex_init(&m)`（int: 0・ENOMEM・EAGAIN） | 今の扱い（無視か誤りで返る）を保つ。phase.md が直す所はそれに従う |
| `pthread_mutex_destroy`・`lock`・`unlock` | `kl_mutex_destroy`・`kl_mutex_lock`・`kl_mutex_unlock` | void を返す。`(void)` の cast を外す |
| `pthread_mutex_trylock(&m)` | `kl_mutex_try(&m)`（0 か EBUSY） | |
| `pthread_cond_init(&c, NULL)` | `kl_cond_init(&c)`（int） | 待ちの時計は単調になる（POSIX の層が `CLOCK_MONOTONIC`） |
| `pthread_cond_destroy`・`wait`・`signal`・`broadcast` | `kl_cond_destroy`・`kl_cond_wait`・`kl_cond_signal`・`kl_cond_broadcast` | void。`(void)` を外す |
| `clock_gettime(CLOCK_REALTIME)` に ms を足して `pthread_cond_timedwait` | `kl_cond_wait_for(&c, &m, ms)`（0 か ETIMEDOUT） | 絶対の時刻の計算を消す |
| `pthread_create(&t, NULL, f, a)` | §13.3 | `f` は `static void f(void *argument)`（`return NULL;` を消す） |
| `pthread_join(t, NULL)` | `kl_thread_join(t)` | 戻り値は今と同じく見ない（`(void)`） |
| `nanosleep` で ms を眠る補助 | `kl_thread_sleep(ms)` | 補助の関数は残して中身を替えてよい |

### 13.3 thread の起動の形

```c
struct kl_thread_options options;

/* The thread: its name (design §6.1), what it runs and its priority. */
memset(&options, 0, sizeof(options));
options.name = "music-play";
options.run = play_run;
options.argument = reader;
options.priority = KL_THREAD_PRIORITY_NORMAL;
error = kl_thread_start(&options, &player->thread);
```

- 名前は §6.1 の表（phase.md にも写す）。優先度は全て NORMAL、`stack_size` は 0（今と同じ）。
- `kl_thread_start` は失敗の時 `*thread` を NULL にして errno の値を返す。今の失敗の扱いをそのまま使う。

### 13.4 静的な条件変数の形（Photos の thumbs・media-worker、IME）

```c
/* Whether the worker's condition was made (0) or why it was not; set once by worker_init. */
static struct kl_once worker_once = KL_ONCE_INIT;
static int worker_wake_error;

static void worker_init(void);
static int worker_ready(void);

/* Makes the worker's condition, once for the program. */
static void
worker_init(void)
{
	/* The condition (a static one cannot be made by its bytes). */
	worker_wake_error = kl_cond_init(&worker.wake);
}

/* Makes the worker's condition the first time: 0, or why it cannot be. */
static int
worker_ready(void)
{
	/* Once for the program; every later call sees the first one's outcome. */
	kl_once(&worker_once, worker_init);
	return worker_wake_error;
}
```

- 条件変数を使う全ての public の入口（start・queue・stop など、signal・wait の前）で `worker_ready()` を呼ぶ。
- 0 でなければ: start はその値を返す（thread を作らない。今の「thread を作れない」の扱い）。signal する所は signal を飛ばす（thread が
  無いので待つ者は無い）。IME は今の「thread が無ければ同期で書く」に落ちる。
- `kl_cond_destroy` は呼ばない（process の終わりまで使う静的な物）。

### 13.5 build と試験の command（worktree の root で）

- zedBSD（3 arch、warning 0 は `-Werror` で build の失敗になる）: `sh plan/ws327/tests/build-zedbsd.sh <phase> <arch> "<program...>"`
  を amd64・arm64・riscv64 で。出力は `build/ws327-<phase>-<arch>/`、log は `build/ws327-<phase>-<arch>.log`、最後の行
  `zedbsd <arch> make=0 warnings=0` を見る。sysroot の build を含み 1 arch 数分〜30 分。3 つを順に（並べない）。各 command に timeout を付ける（script の中で付いている）。
- Linux（libkeiland と、Linux でも build する Settings・IME）: `make -j16 -f userland/desktop/keiland-linux.mk
  KEILAND_LINUX_BUILD=build/ws327-<phase>-linux build/ws327-<phase>-linux/lib/libkeiland.so [build/ws327-<phase>-linux/bin/settings]
  [build/ws327-<phase>-linux/libexec/keiland-ime] > build/ws327-<phase>-linux.log 2>&1; echo $?` と `grep -c 'warning:'` が 0。
- 境界: `python3 -I plan/ws327/tests/check-layers.py <自分の directory...>` が `ws327-check-layers: PASS`（T1・N1 が 0）。
- 凍結と範囲: `sh plan/ws327/tests/check-base.sh <phase>` が `ws327-check-base: PASS`。
- 各 phase.md の host 試験。
- `git diff --check`（check-base.sh が branch の分を見る）。

### 13.6 突然変異の確かめ（必須）

host 試験が通った後、移した code をわざと壊し（git の外で、例えば `cp` で控えを取って編集）、試験が FAIL（か時間切れ）になる事を確かめ、
元に戻して `cmp` で戻った事を確かめる。phase.md が壊し方を挙げる。host 試験の無い所（Photos・Settings・audio）は境界の check で
（pthread の呼び出しを 1 つ戻すと T1 が出る）確かめ、動きは p016 の QEMU で確かめる。

### 13.7 終わりの条件と報告

- 上の command を全て流し、最後の行を自分で読む。規約（`plan/coding-style.md` の §14 の checklist）で自分の diff を見直す。
- commit: 新しい file は先に `git add`、`git commit -m WIP -- <phase.md が挙げる path>`。push しない。rm しない（消す物は報告に書く）。
- phase.md の「結果」の節（日付は host の `date`）: 流した command と最後の行、件数、突然変異の結果、自分で決めた点、未実施、Q1 への所見。
  `plan/ws327/ws.md` は触らない（並べた Phase の merge が表の隣の行で衝突する。Q1 が merge の時に直す、design-reviewer #6）。
- 報告: commit の SHA、command と最後の行、自分で決めた点、未実施。

## 14. p007 の design-reviewer の指摘と反映（2026-10-30、21 件）

| # | 重さ | 指摘 | 反映 |
| --- | --- | --- | --- |
| 1 | high | 6bb8c3c8d の conn.c・mail.h と未移行の sync.c で Mailer が build できず、p007 を main に merge すると main の image が壊れる | P2 が p007 で `sync.c` を移した（zedBSD amd64 の mailer が make 0・warning 0、仮に直した host-mail-sync の写しで 10 件 PASS）。p008 は確かめと host 試験の直しに |
| 2 | medium | p008 の突然変異 3（読みの前の待ちを WRITABLE に）は捕まらない | connect の EINPROGRESS を誤りにする壊し方に。読みの時間切れの試験が無い事は見逃しとして書かせる |
| 3 | medium | p012 の突然変異 1（範囲の変化の signal）は捕まらない事が多い | `keiui_picker_fini` の止めの signal を消す形に |
| 4 | medium | 凍結の照合が sha256 の file 自身を守らず、絶対の hash で他の WS の変更でも FAIL する | `check-base.sh <phase>` を branch の分岐点（`git merge-base HEAD main`）との diff に。`frozen.sha256`・`make-frozen.sh` は使わない（削除を Q1 に頼む） |
| 5 | medium | build-zedbsd.sh の sysroot は `build/<arch>/sysroot` に書かれ、共有の build への link なら壊す。sysroot の status を捨てる | link なら止める、sysroot の status も見る、comment を直した |
| 6 | medium | 並べた Phase が ws.md の隣の行を直して merge が衝突する | 実装の担当は ws.md に触らない（§13.7、各 phase.md） |
| 7 | medium | Phase の範囲の外の file の変更を見つける道具が無い | `check-base.sh <phase>` に Phase ごとの許可の path |
| 8 | medium | arm64・riscv64 の app の基点の build が無い | P2 が移す前の tree（sync.c の後）で全 program × arm64・riscv64 を build（結果は phase007） |
| 9 | medium-low | `kl_socket_close(NULL)` を Mailer が頼るが header・Win32 の骨格が黙る | 骨格と phase014 に「NULL は何もしない」。header の comment は p017 へ持ち越し |
| 10 | low-medium | 振る舞いの変わる所（start の前の signal、IME の作り直し、`kl_cond_wait_for`）に試験が無い | p016 の QEMU に足した、p010・p011 に「未試験」と書かせる |
| 11 | low | `-D_DEFAULT_SOURCE` で thread-posix.c が通らない | phase009 で `-D_GNU_SOURCE` と明記 |
| 12 | low | Music は条件変数で待たない（§6.1 の誤り）、p009 の突然変異 3 が捕まらない | §6.1 を直し、`play_run` の止めの確かめを消す壊し方に |
| 13 | low | doc-lines.py が 1 行多く数え、link の文字と行の食い違いを見ない | `splitlines`、「[lines A-B]」と `#LA-LB` の照合 |
| 14 | low | check-layers が `send`・`recv`・`ppoll`・`<sys/select.h>` などを拾わず、許可の行が `*` で広い | N1 を広げ、libkeiland・IME の許可を `poll`・`poll.h` に絞った。§1.3 の表の名前を直した |
| 15 | low | WSAEHOSTDOWN の写しの食い違い、WSAEINPROGRESS → EINPROGRESS は誤り | §5.3・骨格を EHOSTUNREACH と EALREADY に揃えた |
| 16 | low | `Sleep` などは tick で早く返り「at least」を守れない | `kl_thread_sleep` は `GetTickCount64` で眠り直す（骨格・phase013）。cond と select は p013 の「Q1 へ」 |
| 17 | low | `_beginthreadex` の後に id を書くまでの競合 | `CREATE_SUSPENDED` → id・handle を書く → `ResumeThread`（骨格・phase013） |
| 18 | low | DLL の unload 後の FLS の callback など | phase013 の「Q1 へ」の候補 |
| 19 | low | x86_64 の mingw だけ | build-win32.sh が i686 も compile・link（PASS） |
| 20 | low | `conn.socket` が NULL の idling の account を poll に入れると EINVAL | sync.c は `conn.socket != NULL` の時だけ入れる |
| 21 | low | p015 が 1 回分として大きい | 分けない（例は 08-worker から育てる 1 つ）。重ければ Q1 が 08-network と節を別の起動に分けてよい |
