<!-- awesome-plan project=zedbsd record=ws327-p009 -->

# ws327-p009 Music・Video Player・media-app の移行と、その host 試験の link

- 親: [WS327](../ws.md)。設計: [design.md](../design.md) §1.1・§6.1・§6.3・§13。担当: 実装の担当（Sonnet 5.5 high）。依存: p007。
- Status: planned
- Disposition: normal

## 1. 読む物

- `AGENTS.md` §4・§7・§9・§10、`plan/coding-style.md` の全文。
- design.md の §3.2（thread の約束、特に条件変数の時計と `kl_cond_wait_for`）、§6.1・§6.3、§13（共通の約束。この phase.md の一部として従う）。
- header（仕様、変えない）: `userland/desktop/include/keiland/keiland_thread.h`。

## 2. 触ってよい file

| file | 何を |
| --- | --- |
| `userland/desktop/videoplayer/videoplayer.h` | `struct vp_media` の `pthread_t thread`・`pthread_mutex_t lock`・`pthread_cond_t wake` を §13.2 の型に、`<pthread.h>` を `<keiland/keiland_thread.h>` に（Music の `play.h` もこの header を include する） |
| `userland/desktop/videoplayer/media.c` | §3.2 |
| `userland/desktop/videoplayer/main.c` | `player->media.lock` の lock・unlock（12 か所ほど）を `kl_mutex_*` に |
| `userland/desktop/music/play.h`・`play.c` | §3.1 |
| `userland/desktop/media-app/avcodec.c` | §3.3 |
| `plan/tools/music/host-music-play.sh` | §4.1 |
| `plan/tools/media/run-host-codec.sh` | §4.2 |
| `plan/ws327/phase009/phase.md` の「結果」 | 記録（`plan/ws327/ws.md` は触らない: 並べた Phase の merge が衝突するので Q1 が直す） |

Music・Video Player の Makefile は libkeiland を既に `REQUIRE` に持つので変えない。

## 3. 前後の対応（§13.2 の表を全ての `pthread_*` に。以下はその上で直す所）

### 3.1 Music（`play.h`・`play.c`）

| 所 | 前 | 後 |
| --- | --- | --- |
| `play.h` | `#include <pthread.h>`、`struct mu_player` の `pthread_t thread; pthread_mutex_t lock; pthread_cond_t wake;` | `<keiland/keiland_thread.h>`、`struct kl_thread *thread; struct kl_mutex lock; struct kl_cond wake;`（`thread_started` は残す） |
| `mu_player_init` | `memset` の後 `(void)pthread_mutex_init`・`(void)pthread_cond_init` | `(void)kl_mutex_init(&player->lock)`・`(void)kl_cond_init(&player->wake)`（今と同じく誤りを見ない） |
| `mu_player_open` | `pthread_create(&player->thread, NULL, play_run, reader)` | §13.3、名前 `"music-play"`、引数 `reader` |
| `mu_player_close` | `pthread_join(player->thread, NULL)` | `(void)kl_thread_join(player->thread); player->thread = NULL;` |
| `play_run` | `static void *play_run(void *argument)`、`return NULL;` | `static void play_run(void *argument)`、return 無し（前方の宣言も） |
| `play_sleep_ms` | `struct timespec` と `nanosleep` | `kl_thread_sleep(ms)`。`<time.h>` は他に使う物が無ければ外す |

### 3.2 Video Player（`media.c`）

| 所 | 前 | 後 |
| --- | --- | --- |
| 初期化（135 行あたり） | `(void)pthread_mutex_init`・`(void)pthread_cond_init` | `(void)kl_mutex_init`・`(void)kl_cond_init` |
| 起動（193 行あたり） | `pthread_create(&media->thread, NULL, media_run, reader)` | §13.3、名前 `"video-decode"` |
| 終わり（218 行あたり） | `pthread_join` | `(void)kl_thread_join(media->thread); media->thread = NULL;` |
| `media_run` | `static void *`、`return NULL;` | `static void`、return 無し |
| `media_wait_room` | `clock_gettime(CLOCK_REALTIME)` に `MEDIA_WAIT_MS` を足した `until` と `pthread_cond_timedwait` | `(void)kl_cond_wait_for(&media->wake, &media->lock, MEDIA_WAIT_MS)`。`until` とその計算を消す |
| `media_sleep_ms` | `nanosleep` | `kl_thread_sleep(ms)` |
| `media_now_us` の `clock_gettime(CLOCK_MONOTONIC)` | — | 変えない（thread の物ではない） |

### 3.3 media-app（`avcodec.c`）

- `#include <pthread.h>` → `#include <keiland/keiland_thread.h>`。`static pthread_once_t codec_once = PTHREAD_ONCE_INIT;` →
  `static struct kl_once codec_once = KL_ONCE_INIT;`。`(void)pthread_once(&codec_once, codec_load);` → `kl_once(&codec_once, codec_load);`。
- この file の他の `static void *`（`codec_open_library`・`addin_picture` など）は thread ではないので変えない。
- この file は host 試験で `-std=c89` で compile される: 足す code も C89（宣言は block の先頭、`//` を使わない）。

## 4. host 試験の直し

### 4.1 `plan/tools/music/host-music-play.sh`

- 基準: p007 で P2 が `host-music-play.c` の stand-in を今の decoder の API（`app_decoder_*`、backend・frame_us・trim を足す）に直し、
  移す前の tree で `host-music-play: PASS` を確かめた。
- source に `userland/desktop/libkeiland/platform/thread-posix.c` を足す（`-std=gnu11` のままで C11 の `<stdatomic.h>` が読める。
  header は script が `$dir/inc/keiland/` に写す物で足りる）。`-lpthread` は残す。

### 4.2 `plan/tools/media/run-host-codec.sh`

- 「2. The decoding」の `cc -std=c89 ...` の前に、層を gnu17 で 1 つの object にする行を足す:
  `cc -std=gnu17 -O1 -g -Wall -Wextra -Werror -D_DEFAULT_SOURCE -fsanitize=address,undefined -fno-omit-frame-pointer -Iuserland/desktop/include -c userland/desktop/libkeiland/platform/thread-posix.c -o "$out/thread-posix.o" || { echo "run-host-codec: FAIL (build)"; exit 1; }`
- `cc -std=c89 ...` の行に `-Iuserland/desktop/include` と `"$out/thread-posix.o"` を足す（`-lpthread` は残す）。
- 上の object の行は `-D_GNU_SOURCE`（`-D_DEFAULT_SOURCE` では `thread-posix.c` の `pthread_setname_np` が宣言されず FAIL する。
  design-reviewer #11 が確かめた）。上の例の `-D_DEFAULT_SOURCE` をその行だけ `-D_GNU_SOURCE` に替えて書く。

## 5. 確かめの command（§13.5）

1. `sh plan/tools/music/host-music-play.sh build/ws327-p009-music/host-music-play` → exit 0（各 check の PASS を読む）。
2. `sh plan/tools/media/run-host-codec.sh` → 最後の行 `run-host-codec: PASS`。
3. `sh plan/ws327/tests/build-zedbsd.sh p009 amd64 "music videoplayer"`、同 `arm64`、`riscv64` → 各 `make=0 warnings=0`。
4. `python3 -I plan/ws327/tests/check-layers.py userland/desktop/music/ userland/desktop/videoplayer/ userland/desktop/media-app/` → PASS。
5. `sh plan/ws327/tests/check-base.sh p009` → PASS。

## 6. 突然変異の確かめ（§13.6）

1. `mu_player_open` の `options.run` を NULL にする → `kl_thread_start` が EINVAL、host-music-play の再生の check が FAIL。
2. `avcodec.c` の `kl_once(&codec_once, codec_load)` を消す → add-in が読まれず run-host-codec が FAIL。
3. `play_run` の中の止め（`quit`）の確かめを消す → `mu_player_close` の join が戻らず host-music-play が `timeout 60` で FAIL。
   （`kl_cond_broadcast` を消す壊し方は捕まらない: Music の thread は条件変数で待たず `kl_thread_sleep` で回る。）
4. 各々の後に戻し `cmp` で確かめる。Video Player は host 試験が無い: check-layers で（`pthread_mutex_lock` を 1 つ戻すと T1）確かめ、
   再生は p016 の QEMU。

## 7. 注意

- `videoplayer.h` は Music の `play.h` からも include される。両方を一緒に直し、両方を build する。
- `kl_cond_wait_for` は ms の相対の待ちで、偽の起きと時間切れのどちらも呼び手が条件を見直す（今の `media_wait_room` も 1 回待って返る形
  なのでそのまま）。
- `kl_mutex_lock` などは void を返す: `(void)` の cast を外す（規約: 戻り値の無い関数を cast しない）。
- Video Player の待ち（`MEDIA_WAIT_MS`）・眠り（`MEDIA_WAIT_MS * 5`）の値は変えない。

## 8. 受け入れ（Q1 が見る）

§5 の 5 つが PASS、§6 の 1・2 が捕まる、規約の見直し、`git commit -m WIP -- userland/desktop/music/ userland/desktop/videoplayer/
userland/desktop/media-app/avcodec.c plan/tools/music/host-music-play.sh plan/tools/media/run-host-codec.sh plan/ws327/phase009/phase.md`（ws.md は Q1）。QEMU の再生の開始・停止は p016。

## 結果

日付: 2026-10-30（host の `date`）。担当: 実装の担当（Sonnet 5.5 high）。

### 流した command と最後の行

- `sh plan/tools/music/host-music-play.sh build/ws327-p009-music/host-music-play` → exit 0、PASS 12 件、最後の行 `host-music-play: PASS`。
- `sh plan/tools/media/run-host-codec.sh` → 最後の行 `run-host-codec: PASS`（各 file の `host-codec: PASS`）。
- `sh plan/ws327/tests/build-zedbsd.sh p009 {amd64,arm64,riscv64} "music videoplayer"` → 各 `zedbsd <arch> make=0 warnings=0`。
- `python3 -I plan/ws327/tests/check-layers.py userland/desktop/music/ userland/desktop/videoplayer/ userland/desktop/media-app/` →
  T1 0・N1 0、`ws327-check-layers: PASS`。
- `sh plan/ws327/tests/check-base.sh p009` → `ws327-check-base: PASS`。

### 突然変異（git の外で壊し、`cmp` で戻した）

1. `mu_player_open` の `options.run` を NULL に（`play_run` が未使用で build が先に落ちるので、到達しない条件の三項で NULL に） →
   host-music-play が `FAIL 9`（exit 1）。捕まった。
2. `kl_once(&codec_once, codec_load)` を、決して真にならない `if` の中に（単に消すと `codec_load` の未使用で build が落ちる） →
   run-host-codec が ASan の SEGV で `run-host-codec: FAIL`。捕まった。
3. `play_run` の `if (stop) break;` を決して真にならない条件に → `mu_player_close` の join が戻らず `timeout 60` で exit 124。捕まった。
4. Video Player: `media.c` の `kl_mutex_lock` を `pthread_mutex_lock` に戻す → check-layers が `T1 FAIL ... media.c:175: pthread_mutex_lock`。捕まった。
   全て元に戻し `cmp` で一致を確かめた。

### 自分で決めた点

- `options` の宣言は `mu_player_open`・`vp_media_open` の先頭に足した。起動の comment は「The thread reads ahead from here: its name, what it runs and its priority.」。
- `mu_player_release` の `pthread_cond_destroy`・`pthread_mutex_destroy` は §13.2 の表で `kl_cond_destroy`・`kl_mutex_destroy`（`(void)` 無し）にした。
- `play.c` は `<time.h>` を外した（他に使う物が無い）。`media.c` は `media_now_us` が `clock_gettime(CLOCK_MONOTONIC)` を使うので `<time.h>` を残した。
- host 試験の `-D_GNU_SOURCE` は phase.md の指示のとおり（`run-host-codec.sh` の層の object の行）。

### 未実施・Q1 への所見

- Video Player に host 試験は無い（check-layers と build のみ）。再生の開始・停止は p016 の QEMU。
- `build-zedbsd.sh` の `sysroot-<arch>` は、worktree の `build/llvm*` が無いと LLVM を worktree の中で build し始める（30 分では終わらない）。Q1 の指示で
  `build/{NoctLang,distfiles,llvm,llvm-build,llvm-source,sources}` を共有への link にしてから build した（`build/<arch>` と `host-noct-state` は実 directory）。
  最初に始まった LLVM の build は止め、`build/llvm-build`・`build/llvm-source` の途中の物は `build/tmp/aborted-llvm/` に移した（消す物: 同 directory 一式）。
  次の実装の担当の起動の手順に、この link を先に作る事を入れると良い。
