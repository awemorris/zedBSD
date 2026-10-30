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
| `plan/ws327/phase009/phase.md` の「結果」、`plan/ws327/ws.md` の p009 の行 | 記録 |

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

- source に `userland/desktop/libkeiland/platform/thread-posix.c` を足す（`-std=gnu11` のままで C11 の `<stdatomic.h>` が読める。
  header は script が `$dir/inc/keiland/` に写す物で足りる）。`-lpthread` は残す。

### 4.2 `plan/tools/media/run-host-codec.sh`

- 「2. The decoding」の `cc -std=c89 ...` の前に、層を gnu17 で 1 つの object にする行を足す:
  `cc -std=gnu17 -O1 -g -Wall -Wextra -Werror -D_DEFAULT_SOURCE -fsanitize=address,undefined -fno-omit-frame-pointer -Iuserland/desktop/include -c userland/desktop/libkeiland/platform/thread-posix.c -o "$out/thread-posix.o" || { echo "run-host-codec: FAIL (build)"; exit 1; }`
- `cc -std=c89 ...` の行に `-Iuserland/desktop/include` と `"$out/thread-posix.o"` を足す（`-lpthread` は残す）。
- `-D_DEFAULT_SOURCE` で `thread-posix.c` の `pthread_setname_np`・`gettid` が見えない時は、その object の行だけ `-D_GNU_SOURCE` にする
  （試して決め、結果に書く）。

## 5. 確かめの command（§13.5）

1. `sh plan/tools/music/host-music-play.sh build/ws327-p009-music/host-music-play` → exit 0（各 check の PASS を読む）。
2. `sh plan/tools/media/run-host-codec.sh` → 最後の行 `run-host-codec: PASS`。
3. `sh plan/ws327/tests/build-zedbsd.sh p009 amd64 "music videoplayer"`、同 `arm64`、`riscv64` → 各 `make=0 warnings=0`。
4. `python3 -I plan/ws327/tests/check-layers.py userland/desktop/music/ userland/desktop/videoplayer/ userland/desktop/media-app/` → PASS。
5. `sh plan/ws327/tests/check-base.sh` → PASS。

## 6. 突然変異の確かめ（§13.6）

1. `mu_player_open` の `options.run` を NULL にする → `kl_thread_start` が EINVAL、host-music-play の再生の check が FAIL。
2. `avcodec.c` の `kl_once(&codec_once, codec_load)` を消す → add-in が読まれず run-host-codec が FAIL。
3. Music でもう 1 つ、試験が捕まえる壊し方を探して確かめる（例: `mu_player_close` の `kl_cond_broadcast` を消す → join が戻らず
   timeout）。捕まらなければ、それを「見逃し」として結果に書く。
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
userland/desktop/media-app/avcodec.c plan/tools/music/host-music-play.sh plan/tools/media/run-host-codec.sh plan/ws327/phase009/phase.md
plan/ws327/ws.md`。QEMU の再生の開始・停止は p016。

## 結果

（実装の担当が書く）
