<!-- awesome-plan project=zedbsd record=ws191-design -->

# WS191 の設計: 再生の音の stream を libkeiland の口へ（第 3 版、ws191-p001）

Parent: [WS191](ws.md)。事実の調べと第 1 版への review（blocking 3・should-fix 12・minor 12）は [phase001](phase001/phase.md)。
第 2 版の変更の要点は §12 の対応表（第 1 版の review の ID ごと）、第 3 版は §13（第 2 版の review、blocking 1・should-fix 10・minor 15、phase001 に記録）。

## 1. 目的と境界

- 由来: WS188 p003 の境界の検査で、videoplayer・music・libmedia が `userland/desktop/videoplayer/audio.c` で zedBSD の audiod に直に繋いでいる（`userland/base/audiod/protocol.h` を include、AF_UNIX、SCM_RIGHTS の shm）。ユーザー（2026-10-08）:「libkeilandのaudio streamの口に移す」。Guardrail「Bluetooth と Display も compositor 経由」（app → libkeiland → compositor → libkeiland-backend → OS、「サウンドやWiFiも同様」）。
- ユーザー（2026-10-08 午後）:「サウンドはlibkeiland-backendに入れてください。libkeilandのAPIはkl_audio_がいいです。」→ 音の出力（zedBSD の audiod、Linux の alsa-lib の dlopen、FreeBSD の OSS）は **libkeiland-backend の中**に置く。compositor の本体（`wayland/`）は Wayland の object と backend の間の取り次ぎだけを持ち、OS の操作・音の thread を持たない。libkeiland の公開の API の接頭は **`kl_audio_`**。
- 範囲: **再生（playback）の stream** だけ。録音（capture）・stream ごとの音量・device の選択は外（H1、音量は今の `kl_system_audio_v1` のまま）。
- 完了で `plan/tools/keiland-os-boundary/app-allow.tsv` の audiod の PENDING 3 行（A1・A2・A5、videoplayer/audio.c）を外す。
- 対象の機械: libkeiland・compositor は amd64 だけ（各 package の arch）。Linux・FreeBSD も amd64（aarch64 でも 8 byte の atomic は lock-free）。

## 2. 決定

| ID | 決定 | 理由 | 他の案 |
| --- | --- | --- | --- |
| D1 | **音のデータは compositor を通さない**。backend が作った ring の共有 memory の fd を compositor が client に渡し、client は ring に直に書き、位置は ring の頭を直に読む。Wayland を通るのは作成と制御（start・stop・flush・drain・破棄）と事象（ready・failed・result・drained・underrun・lost）だけ | 帯域（48 kHz・16 bit・2 ch で 192 KB/s）と遅れを今と同じに保つ。compositor の main loop に音の仕事を載せない。fd を event で渡すのは keymap の先例（`kwl_emit_fd`） | Wayland の message で音を中継（帯域・遅れ・負荷で不可）、client に audiod の socket の fd を渡す（境界の意味が無い） |
| D2 | **ring の配置は Keiland の物**（§4）。公開側は `libkeiland/audio/kl-audio-protocol.h` の offset の定数、backend 側は `keiland-backend.h` の `struct kl_backend_audio_ring`（同じ配置の backend の型、S-3: backend は libkeiland の header を include しない、check.sh B1）。両者の一致は **compositor の `wayland/audio-stream.c` の `_Static_assert`**。zedBSD の backend は audiod の shm の fd をそのまま渡し、`struct kl_backend_audio_ring` と `struct audiod_shm_header` の各 offset の一致・`AUDIOD_VERSION == 1`・format の番号の一致を backend の source の `_Static_assert` で確かめる（M-2） | zedBSD で copy も thread も要らない。Linux・FreeBSD の backend は同じ形の ring を memfd で作る | Keiland 独自の ring を作り zedBSD でも backend の thread が audiod の ring に写す（無駄な copy と thread） |
| D3 | protocol は **新しい global `kl_audio_v1`（版 1）** と子の `kl_audio_stream_v1`（§3）。`kl_system_manager_v1` には足さない（第 1 版の「manager 版 24・request 16・capability 0x20000」は取り下げ） | 公開の名（`kl_audio_`）と揃う。manager の版を他の WS（P1 の WS190 など）と取り合わない（M-5 の衝突が起きない）。stream の接続は小さな global を 1 つ bind するだけ（manager の bind は capabilities などを伴う）。能力の bit は要らない（global が有る＝使える） | manager の request 16（第 1 版） |
| D4 | libkeiland の口は **stream ごとに自分の Wayland の接続**を開く（Q1 の判断 2026-10-08 午後で維持）。`WAYLAND_SOCKET` が環境に有る時は開かず `ENOTSUP`（tree の libwayland の `wl_display_connect` は名前より先に `WAYLAND_SOCKET` を消費する、client.c 69〜90、M-7。それで起動される client は入力方式だけで音を出さない）。接続の名は `WAYLAND_DISPLAY`。**無ければ `ENOTSUP`**（libwayland の既定の `wayland-0` は別の compositor かもしれない。WAYLAND_SOCKET で起動された子は WAYLAND_DISPLAY を消されている、input-method.c 1122〜1123） | libmedia（browser の `<video>`）は display を持たず、音を engine の thread から書く。app の display の thread と分ければ thread の安全を考えずに済む | app の `wl_display` に library の queue で載せる（S-2 の別案。libmedia が困る） |
| D5 | compositor は backend を**待たずに**使う。作成・制御の答えと事象は `kwl_system_tick` から呼ぶ `kwl_audio_tick`（poll は最大 10 ms、main.c 855）で backend から取り出して event にする。backend の fd を poll に入れる口は作らない（M-11: 10 ms の tick で足り、本数は最大 32） | compositor の他の system の口と同じ形（volume.c・machine-wait.c）。制御の遅れは 10 ms 以内 | backend の答えを待つ（main loop が止まる）、poll の口を足す |
| D6 | zedBSD の backend は **stream ごとに audiod への接続 1 本**（今の app の client と同じ単位）。`SOCK_NONBLOCK` の connect（zedBSD の AF_UNIX はすぐ繋がるか、backlog が満杯で `EAGAIN`（unix-socket.c 2161〜2165）→ `failed(UNAVAILABLE)`。他の system の `EINPROGRESS` は書けるようになるのを tick で見る、M-4）→ HELLO → WELCOME → STREAM_CREATE → STREAM_CREATED と SCM_RIGHTS の fd、全部 tick で待たずに | audiod の側は何も変えない。1 本の接続を共有すると 1 つの stream の詰まりが他を巻き込む | compositor の接続 1 本に全部の stream |
| D7 | Linux・FreeBSD の backend（p004）は ring を **memfd** で作り（Linux `memfd_create(MFD_CLOEXEC | MFD_ALLOW_SEALING)`、FreeBSD 13 以降も同じ）、大きさを決めてから `F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL` で封じる（B-1: client が縮めて compositor の thread が SIGBUS で落ちるのを防ぐ）。stream ごとの **pump の thread**（libkeiland-backend の中、§6.4）が ring から Linux は **alsa-lib を dlopen** した PCM の `default`（ユーザーの決定 H2、普通は pipewire-alsa を通って PipeWire）へ、FreeBSD は OSS（`/dev/dsp`）へ書く | ユーザーの決定（H2: alsa-lib を dlopen、無ければ stream を断る）。dlopen なので compositor の link と package の依存は増えない | libpipewire の link、pipewire-pulse の native protocol を自前で、compositor の外の helper process（第 1 版 review B-2 の 3 案、ユーザーが dlopen を選んだ） |
| D8 | 時計（A/V の同期）は **played_position**（device から実際に出た分の見積り、§4）。libkeiland の `kl_audio_stream_position`。**時計の基準点（再生・seek・再開の時の clock_frames）も同じ played から取る**（第 2 版 review should-fix 2: 基準を read から取ると Linux・FreeBSD で pause のたびに delay が積もる）。room の計算は read_position（device 側が ring から取った数）の `kl_audio_stream_consumed` | Linux・FreeBSD は PCM・PipeWire・OSS の buffer の分 read が先行する（S-11）。played は `snd_pcm_delay`・`SNDCTL_DSP_GETODELAY` で引いて埋める。zedBSD の audiod の played は mix の時点の read と同じ値（mix.c 274、M-1）なので zedBSD の振る舞いは今と変わらない | read_position を時計に（Linux・FreeBSD で A/V が buffer の分ずれる） |
| D9 | 限り（S-2・S-4）: **接続の相手の pid ごとに 8 本、compositor の全体で 32 本**（audiod の client の上限 64 の内、compositor の音量の接続などを残す）。pid は `SO_PEERCRED` で接続の時に kernel が記録した物（zedBSD は `struct kern_peercred.pid`、Linux は `struct ucred.pid`、FreeBSD は `LOCAL_PEERCRED` の `cr_pid`）、新しい backend の口 `kl_backend_peer_pid`。format は S16_LE（1）・S32_LE（2）・F32_LE（3）、**channels 1〜2**（audiod の上限、main.c 482）、rate 8000〜192000、buffer_frames は 0（backend の既定）か `2 × period_frames` 以上かつ `rate / 100`（10 ms）以上で、`buffer_frames × frame_bytes ≤ 1 MiB`、period_frames は 0（既定）か buffer の半分以下（両方 0 でない時）。外は `failed(INVALID)`、本数の超過は `failed(TOO_MANY)`。**本数に数えるのは pending・ready の stream だけ**（failed・lost の stream は backend が閉じた物で、client が destroy しなくても数えない） | 資源の乱用を防ぐ。stream ごとの接続（D4）では接続の番号で数えても効かないので、相手の process で数える | 1 client 8 本（第 1 版、D4 と矛盾） |
| D10 | backend の stream が終わった（audiod が落ちた・切った、alsa-lib の PCM の失敗、Linux・FreeBSD では client の位置の不正。**zedBSD の audiod は位置の不正の stream を黙って止め（mix.c 207〜216）何も送らないので、zedBSD では BROKEN は来ない**: libkeiland が正しく書く限り起きない既知の限り）時、compositor は `lost(error)` を送る。libkeiland は lost の後の stream を**黙った sink** にする: 書くのは今までどおり ring へ（client の mapping は残る）、consumed・position は lost の時点から monotonic の時計で rate の速さで進む（running の間だけ、written を超えない）。制御は手元の状態だけ変えて `EPIPE` を返す。player は開き直さず、次の file を開く時に新しい stream を試す（p003） | S-7: 再生中に開き直すと他の thread の時計の読みと munmap が競合し、新しい stream は位置 0 から。黙った sink なら player の時計と書く loop は止まらず、close は今の close の道だけ | lost で player が開き直す（第 1 版） |
| D11 | **libmedia は kl_audio_* を直に呼ばない**。libmedia の音は埋め込む側が渡す出力の関数の表（`struct media_audio_output`、`media_set_audio_output()`）を通る（Q1 の指示 2026-10-08 夜）。**この WS では表を埋める者がいない**: Video Player・Music は libmedia を使わず `vp_audio_*` を直に呼ぶ（videoplayer/main.c 243、music/play.c 72）、libmedia を使うのは libbrowser だけ（libbrowser/Makefile 190）で、browser は埋めない（音無し、H3 は Q1 の決定 (c)、ユーザー「ブラウザはベータ3に移します」）。表は WS121 の host 試験（`plan/ws121/tests/host-engine.c`）が偽の出力で埋めて試す（Q1 経由の依頼） | browser の規則（plan/standards/browser-component.md 4・6）: libbrowser は「直接の link・実行時の呼出しに Wayland/Keiland の窓・protocol の依存を入れない」。libmedia は libbrowser に link されるので、libmedia から kl_audio_* を呼ぶとこれに当たる | libmedia が libkeiland を link（規則 4 に反する）、`dlsym(RTLD_DEFAULT)`（規則 4 の例外が要る）、libmedia から音を外すだけ（第 2 版 review の案、Q1 の指示で表を作る） |

## 3. protocol（`kl_audio_v1`、版 1）

```
kl_audio_v1 (global, version 1; seen by the clients of the compositor's own user, as kl_system_manager_v1)
  request 0 destroy                       the streams made from it stay
  request 1 create_stream(new_id kl_audio_stream_v1, uint format, uint channels, uint rate,
                          uint buffer_frames, uint period_frames)
kl_audio_stream_v1
  request 0 destroy
  request 1 start(uint request)           the device reads the ring from now
  request 2 stop(uint request)            a pause: what is written and not read stays in the ring
  request 3 flush(uint request)           drops what is written and not read (read takes write's value); a running stream
                                          stays running, a stopped one stopped, a drain is ended (stopped)
  request 4 drain(uint request)           plays what is written, then stops; drained comes when it has been heard
  event   0 ready(fd ring, uint bytes, uint capacity_frames, uint period_frames)   once, after the create
  event   1 failed(uint error)            once, instead of ready
  event   2 result(uint request, uint error)   exactly one for each start, stop, flush and drain
  event   3 drained(uint request)         the drain of that request is complete (the stream is stopped)
  event   4 underrun(uint count)          the device found the ring empty while running; count is the total so far
  event   5 lost(uint error)              the stream is gone; no event follows
```

error（`KL_AUDIO_ERROR_*`、wire の値は OS に依らない、S-12。libkeiland が errno に戻す）:

| 値 | 名 | 意味 | libkeiland の errno |
| --- | --- | --- | --- |
| 0 | NONE | 成功 | 0 |
| 1 | INVALID | 形式・数・状態に合わない値 | EINVAL |
| 2 | NO_DEVICE | 音の device が無い（audiod の WELCOME の device 0 など） | ENODEV |
| 3 | UNAVAILABLE | 音の service が今は無い（audiod が起きていない・満員で切った、PipeWire に繋がらない） | EAGAIN |
| 4 | UNSUPPORTED | この backend は stream を持たない（alsa-lib が無いなど） | ENOTSUP |
| 5 | NO_MEMORY | 資源が足りない | ENOMEM |
| 6 | TOO_MANY | D9 の本数の上限 | EMFILE |
| 7 | STATE | 今の状態では受けられない request（ready の前、lost の後） | EBUSY（lost の後は EPIPE） |
| 8 | GONE | service が落ちた・切れた | EPIPE |
| 9 | BROKEN | client の書いた位置が不正（write が read より前、容量を超える） | EPROTO |
| 10 | FAILED | その他の失敗 | EIO |

状態機械（S-1・第 2 版 review B-1、compositor が stream ごとに持つ）。**状態は backend の result が NONE で返った時に移る**（失敗の result では移らない）。request の result を作るのは backend の report だけで、compositor は受けた時点では送らない（第 2 版 review should-fix 1）。backend の control が `EAGAIN` を返したら compositor が `result(UNAVAILABLE)`、`ENOTCONN` なら `result(GONE)`（LOST の report が後から来る）を送る。

| 状態 | 入り | start | stop | flush | drain | backend の事象 |
| --- | --- | --- | --- | --- | --- | --- |
| pending | create_stream を受けた | `result(STATE)` | `result(STATE)` | `result(STATE)` | `result(STATE)` | ready → stopped（`ready` を送る）、failed → failed（`failed` を送る） |
| stopped | ready、stop、drained、draining の flush | → running | stopped のまま | stopped のまま（ring を空に） | → draining（device を動かして流し切る） | lost → lost |
| running | start | running のまま | → stopped | **running のまま**（ring を空にして続ける。player は running のまま flush して seek する: media.c 663、play.c 523、engine.c 892） | → draining | underrun → `underrun`（間引き）、lost → lost |
| draining | drain | → running（drain を取り消す） | → stopped（drain を取り消す） | → stopped（drain を取り消し ring を空に。zedBSD の backend は STOP と FLUSH を送り、2 つの DONE で 1 つの result） | 新しい drain が前を置き換える（前の request の drained は来ない） | drained → stopped（`drained(request)`）、lost → lost |
| failed | failed を送った | `result(STATE)` | `result(STATE)` | `result(STATE)` | `result(STATE)` | — |
| lost | lost を送った | `result(GONE)` | `result(GONE)` | `result(GONE)` | `result(GONE)` | — |

- 状態に合わない backend の事象（stop の後に先に読まれていた DRAINED、stopped の UNDERRUN など）は捨てる。drain を取り消した後の DRAINED も捨てる（backend は drain の serial を忘れる）。
- destroy はどの状態でも受け、backend の stream を閉じる。backend の close は**まだ取り出されていない READY の fd を閉じる**（§6.2、第 2 版 review should-fix 5。compositor は close の後に report を取らない）。
- stopped の drain は device を動かして流し切る（audiod の STREAM_DRAIN の振る舞い、main.c 357〜372）。drained の後は stopped で、続けるには start が要る（device.c 510〜513）。
- underrun は stream ごとに **1 秒に高々 1 回**、count は最新の合計（S-6: audiod は空の間 period ごとに UNDERRUN を出す、mix.c 277〜282。毎秒約 100 の event は読まない client の出力の queue を ENOBUFS にする）。
- event を送れない（`kwl_emit_fd`・`kwl_emit` の ENOBUFS・ENOMEM、wire.c 117〜133）時は、その stream の backend を閉じ、**その接続を閉じる**（client を fatal にして切る。接続は stream ごとなので他の stream・窓を巻き込まない。第 2 版 review should-fix 6）。libkeiland は自分の接続の EOF・error を `lost(GONE)` と同じに扱い黙った sink に入る（§5）。ready の fd は `kwl_emit_fd` が失敗の時に閉じる。
- 見える範囲（M-6）: `kl_audio_v1` は `kl_system_manager_v1` と同じく **compositor と同じ uid の client だけ**に見せる（`kwl_settings_global_visible` に kind を足す）。login 画面（settings が無い）にも見せない。結果: su した Terminal などから別の uid で起動した player は stream を開けず（`ENOTSUP`）、音無しで再生する（時計は monotonic）。今の audiod に直の client との違いとして記録する。

## 4. ring（Keiland audio ring、版 1）

共有 memory の先頭 4096 byte が頭、その後に `capacity_frames × frame_bytes` byte の音（channel の interleave、4096 の倍数に切り上げ）。

| offset | 型 | 内容 | 書き手 |
| --- | --- | --- | --- |
| 0 | u32 | tag（server の物、client は見ない。zedBSD は audiod の "AUDD"） | server |
| 4 | u32 | version（1） | server |
| 8・12・16 | u32 | format・channels・rate | server |
| 20 | u32 | frame_bytes | server |
| 24・28 | u32 | capacity_frames・period_frames | server |
| 64 | u64 | write_position（client が書いた frame の数、増えるだけ） | client |
| 72 | u32 | write_sequence（Keiland の書き手は使わない、0 のまま） | — |
| 128 | u64 | read_position（device 側が ring から取った数） | server |
| 136 | u32 | read_sequence（同上） | — |
| 192 | u64 | played_position（device から出たと見積もる数） | server |
| 200 | s64 | played_time_ns（その見積りの CLOCK_MONOTONIC） | server |
| 208 | u32 | played_sequence（played の 2 語の seqlock: 書く間は奇数） | server |
| 212・216・220 | u32 | underruns・overruns・state（0 stopped、1 running、2 draining） | server |

- 位置は **8 byte の atomic**（読みは acquire、書きは release）。Keiland の code（libkeiland の `libkeiland/audio/audio-ring.h`、Linux・FreeBSD の backend）は `__GCC_ATOMIC_LLONG_LOCK_FREE == 2` を `_Static_assert` し、write・read の sequence の語を使わない（M-3・U3: 対象は amd64 だけで、audiod も amd64 では write・read の sequence を進めない、protocol.h 209〜212。lock-free でない ABI の build は compile で止まる）。
- played_position と played_time_ns の組は **played_sequence の seqlock** で書き読む（第 2 版 review should-fix 3: audiod は amd64 でも `audiod_played_store` で played_sequence を進め、2 語を seqlock の中で書く、mix.c 431〜444）。書き手は sequence を奇数にして（release）2 語を書き、偶数に戻す。読み手（libkeiland）は sequence が偶数で前後同じになるまで読み直す。Linux・FreeBSD の pump も同じ形で書く。write・read の位置の sequence は使わない（protocol.h 209〜212 の「進めない」はこの 2 つだけ）。
- client（libkeiland）は ready の後に `fstat` で大きさが `4096 + capacity × frame_bytes`（切り上げ）以上で `bytes` と合うことを確かめてから `MAP_SHARED`（読み書き）で map し（M-12）、頭の version・format・channels・rate・frame_bytes・capacity_frames が求めた値と event の値に合うことを検べる。合わなければ `EPROTO` で閉じる。
- server（Linux・FreeBSD の pump）は頭の server の欄を**自分の手元の値**から使い、共有 memory からは write_position だけを読む。その値を検べる: `write < read` か `write − read > capacity` は BROKEN で lost（audiod の mix.c 207〜216 と同じ規則。zedBSD では audiod が黙って止めるだけで BROKEN は来ない、D10）。

## 5. libkeiland の口（KL_VERSION 73 の予定、merge で Q1 が揃える。`include/keiland/keiland.h`）

```c
#define KL_AUDIO_FORMAT_S16_LE	1U
#define KL_AUDIO_FORMAT_S32_LE	2U
#define KL_AUDIO_FORMAT_F32_LE	3U
#define KL_AUDIO_EVENT_DRAINED	0x1U
#define KL_AUDIO_EVENT_UNDERRUN	0x2U
#define KL_AUDIO_EVENT_LOST	0x4U

/* Adding a field later changes the ABI: a new KL_VERSION and a call that takes the larger struct (minor of the v2 review). */
struct kl_audio_format {
	unsigned format;	/* KL_AUDIO_FORMAT_* */
	unsigned channels;	/* 1 or 2 */
	unsigned rate;		/* 8000 to 192000 */
	unsigned buffer_frames;	/* the ring, 0 for the backend's choice */
	unsigned period_frames;	/* 0 for the backend's choice */
};
struct kl_audio_stream;

int kl_audio_stream_open(const struct kl_audio_format *format, struct kl_audio_stream **stream);
	/* 0; ENOTSUP (no Keiland, no kl_audio_v1, WAYLAND_SOCKET set or WAYLAND_DISPLAY unset, or a backend without streams), ENODEV,
	   EAGAIN, EINVAL, EMFILE, ENOMEM, EPROTO (a ring not as asked), EPIPE (the compositor went), ETIMEDOUT */
void kl_audio_stream_close(struct kl_audio_stream *stream);
int kl_audio_stream_start(struct kl_audio_stream *stream);
int kl_audio_stream_stop(struct kl_audio_stream *stream);
int kl_audio_stream_flush(struct kl_audio_stream *stream);
int kl_audio_stream_drain(struct kl_audio_stream *stream);	/* returns once accepted; drained is an event */
size_t kl_audio_stream_write(struct kl_audio_stream *stream, const void *frames, size_t count);
uint64_t kl_audio_stream_written(const struct kl_audio_stream *stream);
uint64_t kl_audio_stream_consumed(const struct kl_audio_stream *stream);
uint64_t kl_audio_stream_position(const struct kl_audio_stream *stream, int64_t *time_ns);
unsigned kl_audio_stream_capacity(const struct kl_audio_stream *stream);
int kl_audio_stream_dispatch(struct kl_audio_stream *stream, unsigned *events);	/* never waits */
```

- open: 接続（D4）→ registry で `kl_audio_v1` → `create_stream` → `ready` か `failed` を最大 2 秒待つ → §4 の検べと map → fd を閉じる。stream の object は自分の接続の既定の queue だけを使う。
- 制御（start・stop・flush・drain）: stream の mutex の下で request を送り、その request の `result` を最大 2 秒待つ（`ETIMEDOUT`）。待つ間に来た他の event（drained・underrun・lost）は stream の手元の bit に溜める。
- `write`: 書き手は 1 つの thread。room（capacity − (written − consumed)）の分だけ ring に写し、write_position を release で進める。room が 0 の時は非 block の dispatch を 1 回（mutex を **trylock**、取れなければしない）: 読まない client でも underrun・lost の event が compositor に溜まらず、lost を知る（S-6）。
- `written`・`consumed`・`position`・`capacity`: lock 無し、どの thread からでも。position は played の frame の数（time_ns に見積りの時刻）で、played_sequence の seqlock で組を読む（§4）。
- `dispatch`: 非 block（trylock、取れなければ events 0 で 0 を返す）。socket に有る分を読み、溜まった bit を返して消す。**fd の口は出さない**（S-5: 制御の往復の間に libwayland が読んだ event は queue に移り、fd は読めなくなる。今の player は drained を待たない）。
- lost（event、または自分の接続の EOF・error、§3）の後（D10）: write は ring に書き続け（害は無い）、consumed・position は黙った sink の値（lost の時点の値から monotonic の時計で running の間だけ rate の速さで、written を超えない）。黙った sink の基準（時刻・位置・running）は mutex の下で書き、読み手は stream の中の sequence の語で 1 つに読む。制御は手元の running を変えて `EPIPE` を返す（p003 の vp_audio は EPIPE でも running の更新を成功と同じにする: そうしないと pause の後も sink が進む、audio.c 179・193）。
- close: 呼ぶ側の約束 — **他の thread がその stream を使い終わってから**（今の vp_audio_close と同じ。p003 は各 player の close の順を確かめる）。destroy を送り、munmap し、接続を閉じる（compositor では切断で `kwl_compose_quiesce` が走る、objects.c 533。M-8: player は曲・file ごとに開き直さない）。

## 6. compositor と backend

### 6.1 compositor（`wayland/audio-stream.c`、新）

- kind `KWL_AUDIO`（global）と `KWL_AUDIO_STREAM`。stream の record: client、object、pid（接続の最初の create で `kl_backend_peer_pid`、失敗は pid 0 として 1 つの組に数える）、状態（§3）、backend の stream、最後の underrun を送った時刻。
- create_stream: D9 の検べ（値 → INVALID、本数 → TOO_MANY）→ `kl_backend_audio_stream_open`。`kwl_audio_tick`（`kwl_system_tick` から）で全 stream の `kl_backend_audio_stream_next` を空になるまで取り、§3 の状態機械で event にする。client の切断・destroy で backend の stream を閉じる。compositor の終わりで全部閉じる。
- `_Static_assert`: `struct kl_backend_audio_ring` の各 offset と `KL_AUDIO_RING_*`（kl-audio-protocol.h）、`KL_BACKEND_AUDIO_ERROR_*` と `KL_AUDIO_ERROR_*`、`KL_BACKEND_AUDIO_FORMAT_*` と `KL_AUDIO_FORMAT_*` の値。
- log: `KWL AUDIO stream client=N id=M pid=P create format=F channels=C rate=R buffer=B period=Q`、`ready capacity=… bytes=…`、`failed error=…`、`start|stop|flush|drain request=… error=…`、`drained request=…`、`underrun count=…`（間引いた後の物）、`lost error=…`、`closed`。

### 6.2 backend の口（`libkeiland-backend/keiland-backend.h`、新しい節）

```c
struct kl_backend_audio_ring { … };	/* §4 の配置、_Alignas(64) の 3 つの組（完全な定義、static assert に要る） */
struct kl_backend_audio_stream;
struct kl_backend_audio_stream_format { unsigned format, channels, rate, buffer_frames, period_frames; };
struct kl_backend_audio_stream_report {
	unsigned what;		/* KL_BACKEND_AUDIO_READY, _FAILED, _RESULT, _DRAINED, _UNDERRUN, _LOST */
	unsigned error;		/* KL_BACKEND_AUDIO_ERROR_*（§3 の表と同じ値） */
	uint32_t request;	/* RESULT・DRAINED */
	uint32_t count;		/* UNDERRUN */
	int fd;			/* READY: the ring, the caller owns it */
	uint32_t bytes, capacity_frames, period_frames;	/* READY */
};
int kl_backend_audio_stream_supported(void);	/* 1 when the backend can make streams (Linux: alsa-lib opened once) */
struct kl_backend_audio_stream *kl_backend_audio_stream_open(const struct kl_backend_audio_stream_format *format);
	/* never waits; NULL with errno (ENOMEM) */
int kl_backend_audio_stream_control(struct kl_backend_audio_stream *stream, unsigned what, uint32_t request);
	/* START, STOP, FLUSH, DRAIN; 0, or ENOTCONN (not ready or ended), EAGAIN (the service's queue is full) */
int kl_backend_audio_stream_next(struct kl_backend_audio_stream *stream, struct kl_backend_audio_stream_report *report);
	/* 1 and an item, 0 when nothing more; never waits */
void kl_backend_audio_stream_close(struct kl_backend_audio_stream *stream);	/* never waits; closes a READY fd not taken yet */
void kl_backend_audio_stream_reap(void);	/* joins the pump threads that ended (each tick; zedBSD: nothing) */
void kl_backend_audio_stream_reap_all(void);	/* at the compositor's end: waits for every pump to end and joins it */
int kl_backend_peer_pid(int descriptor, pid_t *pid);
```

- compositor は `kl_backend_audio_stream_supported()` が 0 の時は `kl_audio_v1` を広告しない（libkeiland は `ENOTSUP`）。
- compositor は毎 tick `kl_backend_audio_stream_reap()` を呼び（close の後の pump を join する場所、第 2 版 review should-fix 4）、終わりには全 stream を close して `kl_backend_audio_stream_reap_all()` で待つ（alsa-lib・PipeWire の thread が動いている間に exit の atexit・destructor と競合させない。alsa-lib は dlclose しない）。
- peer: uid は今の `libkeiland-backend/peer/peer-getpeereid.c`（zedBSD・FreeBSD で共有）のまま、pid は getpeereid に無いので OS ごとの file（`peer-zedbsd.c`・`peer-freebsd.c`、Linux は `peer-linux.c` に足す）。
- 置き場所: zedBSD は `libkeiland-backend-zedbsd/audio-stream-zedbsd.c`・`peer-zedbsd.c`（`SO_PEERCRED` の `struct kern_peercred`、networkd の先例）、Linux は `libkeiland-backend-linux/audio-stream-linux.c`・`peer-linux.c` に pid、FreeBSD は `libkeiland-backend-freebsd/audio-stream-freebsd.c`・`peer-freebsd.c`（`LOCAL_PEERCRED`）、pump と変換の共通部は `libkeiland-backend/audio/pump.c`（Linux・FreeBSD）。p004 までの Linux・FreeBSD は `libkeiland-backend/unsupported/audio-stream-unsupported.c`（supported 0）。

### 6.3 zedBSD の backend（p002）

- D6 の接続。HELLO の WELCOME で device 0 は `FAILED(NO_DEVICE)`。connect の `ENOENT`・`ECONNREFUSED`、WELCOME の前の EOF（audiod が満員で accept の直後に閉じた、main.c 214〜217）は `FAILED(UNAVAILABLE)`。
- STREAM_CREATED の fd を READY に（fd は compositor へ、backend は持たない）。audiod の serial は backend が振り、serial → request の小さな表を持つ。DONE・ERROR（その serial）は RESULT に。**DRAIN は audiod が DONE を返さない**（main.c 357〜372）ので、送れた時点で backend が RESULT(NONE) を作り（compositor は作らない、§3）、DRAINED（header.serial が drain の serial）を DRAINED(request) に。drain の途中の start・stop・flush で drain の serial を忘れる（来ない DRAINED を待たない）。**draining の flush は STOP と FLUSH を続けて送り**（audiod の FLUSH は draining を変えない、main.c 374〜389）、2 つの DONE がそろってから 1 つの RESULT（どちらかの ERROR ならその error）。running・stopped の flush は FLUSH だけ（状態は変わらない）。
- audiod の ERROR の errno の写し: EINVAL・ENOENT → INVALID、ENOMEM → NO_MEMORY、EMFILE → TOO_MANY、他は FAILED（STREAM_CREATE の ERROR は FAILED の report、制御の ERROR は RESULT）。buffer 0 は audiod の既定（4 × device の period、main.c 476）を使い、period_frames がそれを超えると audiod は EINVAL（D9 の検べで 0 の buffer と period の組は compositor が通すので、INVALID の FAILED として client に届く）。
- UNDERRUN は合計を覚え、tick ごとに最新を 1 つの UNDERRUN に。REQUEST・OVERRUN・VOLUME_CHANGED は読み捨て。recv は非 block で読める分を全部（AF_UNIX の buffer 64 KiB を溜めない）。send が `EAGAIN` なら control は `EAGAIN`（compositor は `result(UNAVAILABLE)`）。socket の EOF・error は `LOST(GONE)`。
- `_Static_assert`（D2）。audiod の client の数: stream 1 本につき 1。
- close は report の queue に残った READY の fd を閉じる（§3）。

### 6.4 Linux・FreeBSD の backend（p004）

- open: memfd（D7）の ring を作り封じる → pump の thread を起こし、ready は pump が device を開けた後（Linux: `snd_pcm_open("default", PLAYBACK, SND_PCM_NONBLOCK)` と `snd_pcm_set_params(format, RW_INTERLEAVED, channels, rate, soft_resample 1, 40 ms)`、FreeBSD: `open("/dev/dsp", O_WRONLY | O_NONBLOCK)` と `SNDCTL_DSP_SETFMT`・`CHANNELS`・`SPEED`）。device が F32・S32 を取らない時は pump が S16 に変換する（`audio/convert.c`）。開けなければ FAILED（device が無い NO_DEVICE、PipeWire などに繋がらない UNAVAILABLE、alsa-lib の symbol が無い UNSUPPORTED）。
- pump と main loop の受け渡し（S-10）: 制御は command の queue（mutex）と wake の pipe で pump へ。**read_position・played・state は pump だけが書く**（flush も pump が read = write にする）。pump から main loop への report（READY・RESULT・DRAINED・UNDERRUN・LOST）は report の queue（mutex）に積み、`kl_backend_audio_stream_next` が取り出す（tick で見るので pipe は要らない）。
- pump: 全 signal を `pthread_sigmask` で block。poll（PCM の `snd_pcm_poll_descriptors`・OSS の fd の POLLOUT と wake の pipe）→ running なら `min(avail, write − read)` を書いて read を進め、played = read − delay（`snd_pcm_delay`・`GETODELAY`）と時刻を書く。running で ring が空なら無音を 1 period 書き（device を止めない）、空になった移り変わりで underruns を 1 つ増やして UNDERRUN。**書いた無音の frame の数を別に数えて delay から引き**（played = read − (delay − 未再生の無音)）、played は前の値より小さくしない（単調）。**draining では無音を足さない**（第 2 版 review should-fix 7）。stop は `snd_pcm_pause(1)`、できない device では `snd_pcm_drop`（PCM の buffer の ≤ 40 ms を捨てる、U1）、OSS は書くのをやめる。flush は drop と prepare・`SNDCTL_DSP_HALT_OUTPUT` と read = write。drain は ring が空になってから、PCM の `avail ≥ buffer の大きさ` か XRUN の状態（OSS は GETODELAY が 0）で終わりとして DRAINED と stopped。flush は running・stopped を変えない（draining の flush は drain を取り消して stopped、§3）。
- close: pump に終わりを頼み wake するだけ（待たない）。pump の終わりを tick で見て `pthread_join`（終わった thread の join は待たない）。PCM の open が長く block しても main loop は止まらない。
- `snd_pcm_open` は backend の mutex で順に呼ぶ（古い alsa-lib の設定の読み込みが thread 安全でない恐れ）。
- alsa-lib: `dlopen("libasound.so.2", RTLD_NOW | RTLD_LOCAL)` を最初の `supported` で 1 回、使う symbol（`snd_pcm_open`・`_set_params`・`_writei`・`_avail_update`・`_delay`・`_pause`・`_drop`・`_prepare`・`_recover`・`_poll_descriptors_count`・`_poll_descriptors`・`_poll_descriptors_revents`・`_close`・`snd_strerror`）が全部有る時だけ supported 1。alsa-lib・symbol が無ければ supported 0（音量の操作は今のまま、H2）。試験は library の名を compile の時の macro（`KL_BACKEND_ALSA_LIBRARY`）で偽の library に替える。

## 7. app の移行（p003）

- `videoplayer/audio.c` の `vp_audio_*`（口の名と意味は保つ: open・close・start・stop・flush・write・write_position・read_position に clock_position を足す）の中身を `kl_audio_stream_*` に。audiod の header と socket を使わない。videoplayer・music は既に libkeiland を link している。player の時計（media.c 276・music/play.c 276・562 など）**と時計の基準点（media.c 193・673、music/play.c 194・531、engine.c 309・538・902 の clock_frames）**は `clock_position`（D8 の played）に、room の計算は read_position（consumed）のまま。外から直に触られている `struct vp_audio` の field（`created`・`rate`: engine.c 985〜990、media.c 275・278、play.c 271）も名と意味を保つ。start が `EPIPE`（黙った sink）でも `running` の更新は成功と同じにする（D10）。zedBSD では played と read は同じ値（D8）で振る舞いは変わらない。
- libmedia（D11、H3 は (c)）: libmedia の `media.h` に出力の表 `struct media_audio_output`（open・close・start・stop・flush・write・written・consumed・position の関数と context、S16_LE 2 ch）と `media_set_audio_output()`（今の `media_set_log` と同じ形の process 全体の設定）を足す。engine.c は表を通して書き、表が無ければ音無し（時計は monotonic、今の audiod が無い時と同じ）。この WS では表を埋める app が無い（D11）。表の試験は WS121 の `plan/ws121/tests/host-engine.c` に偽の出力を足して行う（Q1 経由の依頼）。ベータ 3 への申し送り: libmedia は要素・file ごとに stream を開く（engine.c 620・581、libbrowser/page/media.c 659〜690）ので、browser では D9 の pid ごと 8 本（全部のタブで 1 つの pid）と切断ごとの quiesce（M-8）に当たる。browser は表を埋めない（音無し、ベータ 3 で `browser.h` に Wayland・Keiland の型を含まない同じ形の表を足して shell が埋める、後の WS）。libmedia は `videoplayer/audio.c` を build しなくなる。
- 時計と close（S-7）: lost で開き直さない（D10）。各 player の close（videoplayer main.c 255、music play.c 85、libmedia engine.c 581）が、書く thread・時計を読む thread の終わりの後であることを確かめる（違えば順を直す）。
- host の build（S-8）: libmedia が `videoplayer/audio.c` を build しなくなるので、`plan/ws074/tests/host-build.sh`（77〜88 行の libmedia の file の並び）と `plan/ws121/tests/run-host-engine.sh`（15 行）から `videoplayer/audio.c` を外す。他の WS の file なので Q1 経由で依頼する（p003 の依存）。
- 境界の許可の表の PENDING 3 行を外し、`check.sh` の A1・A2・A5 が通ること。

## 8. 試験

- host（p002）、`plan/ws191/tests/host-audio-stream.sh`（ASan・UBSan）:
  1. 端から端: libkeiland の `libkeiland/audio/` を **tree の libwayland**（試験の `BUILD` の中で `libwayland/Makefile.linux` の source の並びから build する。共有の `build/keiland-linux` に頼らない。S-9: host の libwayland-client は fd の受け取りが tree の物と違う）で link し、server の役の thread（compositor の `wayland/audio-stream.c` と `wire.c`（外への依存は `kwl_dispatch` だけ）、偽の display・registry・`kwl_create`・`kwl_settings_global_visible`: `plan/ws131/tests/host-system.c` の偽の server の形、`WAYLAND_DISPLAY` に試験の socket）と偽の backend（試験が事象を作る）に繋ぐ。確かめる: open → ready の fd・map・頭の検べ（version・format の不一致は EPROTO）、format・channels 3・buffer の超過の EINVAL、同じ pid の 9 本目の EMFILE、制御の result（1 つの request に 1 つ）、**running の flush の後 start 無しで consumed が進む**、drain の result と drained、drain の途中の stop・flush で drained が来ない、backend の control の EAGAIN が result(UNAVAILABLE)、偽の backend の 100 回の underrun が 1 秒に 1 回以下の event に、lost で EPIPE と黙った sink の position が rate で進む、client の切断・destroy で backend の close、event を送れない時に接続が閉じ libkeiland が黙った sink に入る、played の seqlock の組、`WAYLAND_SOCKET` が有る時・`WAYLAND_DISPLAY` が無い時の ENOTSUP。
  2. zedBSD の backend: `audio-stream-zedbsd.c` を host で、偽の audiod（`plan/ws100/tests/host-audio.c` の形、socket の path は `AUDIO_SOCKET_PATH` の macro で試験の path、audio-zedbsd.c 37 の先例）に: WELCOME の device 0 → NO_DEVICE、接続できない・accept の直後の close → UNAVAILABLE、STREAM_CREATED の fd → READY、DONE・ERROR → RESULT、DRAIN → すぐの RESULT と DRAINED、draining の flush → STOP と FLUSH の 2 つの DONE で 1 つの RESULT、UNDERRUN の まとめ、EOF → LOST(GONE)、close が未取り出しの READY の fd を閉じる（開いた fd の数）。
- build（p002・p003）: zedBSD amd64 の libkeiland・wayland・videoplayer・music・libmedia・libbrowser（target を名指す）、Linux の keiland（`make keiland-linux`）、どれも warning 0。境界の検査 `sh plan/tools/keiland-os-boundary/check.sh`。
- QEMU（T1、p003 の後）: audiod の入った image（`plan/ws100/tests/config-amd64-audiod.mk` の形、WS191 の tests/ の config.mk）で Music の再生と Video Player の再生: compositor の log の `KWL AUDIO stream … ready` と `start`、player の再生の位置の表示が進む（screenshot 2 枚の差）、QEMU の wav の audiodev に無音でない音が書かれる、player を閉じて `closed`。browser の `<audio>` は H3 (c) で範囲の外。（M-9: audiod の client の数と drain は観測できないので受け入れに入れない）
- Linux・FreeBSD（p004、T1）:
  - Linux: 正弦波の試験の client（`plan/ws191/tests/tone.c`、libkeiland の `kl_audio_*` だけを使い 2 秒の 440 Hz を鳴らし、position の進みを出力に書く）を Linux の Debian 13 の QEMU+KVM guest の Keiland の上で（WS105 の guest は compositor が動く）。
  - FreeBSD: compositor が動くのは i915 の passthrough の専用 guest だけ（plan/ws109/ws.md 15〜17）なので、**compositor 無しの backend 単体の試験 program**（`plan/ws191/tests/tone-backend.c`、`kl_backend_audio_stream_*` を直に呼び ring に正弦波を書く）を普通の FreeBSD 15 の guest で。
  - 観測: QEMU の wav の audiodev（無音でない、440 Hz の山）と program の出力（position が 2 秒分 ±10 % 進む）。今の guest はどちらも `-audiodev none`（plan/tools/keiland-linux/guest.py 94、plan/ws109/guest-plan.json 29〜30）なので、**wav の audiodev と音の device を足す tools の変更を Q1 に依頼する（p004 の依存）**。
  - host 試験は pump を偽の alsa-lib（`KL_BACKEND_ALSA_LIBRARY`）で: 書いた frame の数、stop・flush（running のまま）・drain、無音の埋めの後の played が単調で drain が終わる、BROKEN の位置、close が待たず reap で join、close が未配達の READY の fd と memfd を閉じる。FreeBSD の build は guest の中の native の build（WS109 の手順）。
- 実機: 5330 で音が出ること（ユーザーの UAT）。

## 9. Phase と受け入れ

| Phase | 内容 | 受け入れ |
| --- | --- | --- |
| p001 | この設計、design-reviewer で blocking 0、Q1 の判定 | review の反映、Q1 の ACK（H3 は決定済み） |
| p002 | `kl-audio-protocol.h`・libkeiland の `kl_audio_*`（KL_VERSION 73、exports）・compositor の `audio-stream.c`・backend の口と `kl_backend_peer_pid`（3 OS）・zedBSD の backend・Linux・FreeBSD の unsupported・host 試験 | §8 の host 試験 1・2 が PASS、zedBSD amd64・Linux の keiland の build が warning 0 |
| p003 | videoplayer・music の移行、libmedia の出力の表（browser は埋めない、H3 (c)）、host の script 2 本の追従（Q1 経由）、境界の表の PENDING を外す、T1 の QEMU | 境界の検査 PASS、build warning 0、§8 の QEMU を T1 が PASS |
| p004 | Linux（alsa-lib の dlopen）・FreeBSD（OSS）の pump と ring、`tone.c`・`tone-backend.c` | host 試験（偽の alsa-lib）PASS、Linux・FreeBSD の guest で tone の音を T1 が観測（wav の audiodev の tools の変更は Q1 経由）、FreeBSD の native build が warning 0 |
| p005 | 規約の全文の見直し（WS が変えた C の code を plan/coding-style.md の全文と照らす） | style-check と全文の読みで違反 0、build warning 0、host 試験の再実行 PASS |

## 10. 人の判断（Q1 経由で、既定の案つき）

| ID | 問い | 状態・既定の案 |
| --- | --- | --- |
| H1 | 録音（capture）と stream ごとの音量を範囲の外にしてよいか | 既定: 外（必要になった時に別 WS） |
| H2 | Linux の再生の経路 | **決定済み**（ユーザー 2026-10-08 午後）: alsa-lib を dlopen、無ければ stream を断る |
| H3 | browser（libbrowser の libmedia）の音の経路。libbrowser は Keiland の protocol に実行時にも依存できない（browser-component.md 4） | **決定済み**（Q1 2026-10-08 夜、ユーザー「ブラウザはベータ3に移します」）: (c) browser の音は後の WS（ベータ 3）。この WS では libmedia に出力の表だけを作り、browser は埋めない。元の案: (a) libbrowser の公開の口に Wayland・Keiland の型を含まない音の出力の表を足し、browser の shell が kl_audio_* で埋める（`browser.h` の追加、WS074・WS107 の領域）。他: (b) libmedia が `dlsym(RTLD_DEFAULT)` で kl_audio_* を探す（規則 4 の例外が要る）、(c) browser の音は後の WS へ（この WS の受け入れから browser を外す） |
| D4・D9 | stream ごとの接続と pid ごとの限り | **決定済み**（Q1 2026-10-08 午後） |
| p004 | Linux・FreeBSD の受け入れ | **決定済み**（Q1 2026-10-08 午後）: 正弦波の試験の client |

## 11. 未確認・リスク

- U1: Linux・FreeBSD の stop で PCM の buffer の ≤ 40 ms が捨てられる device（`snd_pcm_pause` が無い物）。played は delay を引くので時計は ≤ 40 ms 進む。体感は p004 の guest と実機で見る。
- U2: compositor の tick が 10 ms より遅れる時（重い合成）の制御の遅れ。player は start・stop の答えを待つので、体感に出るかは QEMU で見る。
- U3: alsa-lib の PCM `default` が pipewire-alsa の時、compositor の process に libpipewire の plugin と thread が読み込まれる（ユーザーの決定 H2 の結果）。pump は全 signal を block するので、pump から作られる PipeWire の thread もその mask を継ぐ（signal の半分は解ける）。compositor の終わりの順（reap_all の後に exit）を p004 で確かめる。
- U4: FreeBSD 15 の OSS が AFMT_F32_LE・AFMT_S32_LE を受けるか（受けなければ pump の S16 への変換）。
- U5: libkeiland の stream の接続は compositor の「client」になり、client の一覧・ping・切断の処理（quiesce）の対象になる。窓の無い client が ping の対象にならないことを p002 で確かめる（xdg_wm_base を bind しないので来ない見込み）。

## 12. 第 1 版の review への対応

| review | 対応 |
| --- | --- |
| B-1 SIGBUS | D7 の memfd の封（SHRINK・GROW・SEAL）、§4 の pump は手元の値と write の検べ |
| B-2 ALSA の前提 | H2 をユーザーに聞き直し、alsa-lib の dlopen に決定（D7・§6.4） |
| B-3 p004 の受け入れ | Q1 の判断: `tone.c` と wav の audiodev（§8・§9） |
| S-1 drain・状態 | §3 の状態機械、drained(request)、ready の後の object 無しで fd を閉じる、送れない時は lost |
| S-2 限りの単位 | D9 の pid ごと 8・全体 32（`kl_backend_peer_pid`） |
| S-3 backend の include | D2: `struct kl_backend_audio_ring` と compositor の static assert |
| S-4 channels・buffer | D9: channels 1〜2、1 MiB |
| S-5 fd の口 | §5: fd の口を出さない、dispatch は trylock |
| S-6 underrun の嵐 | §3: 1 秒に 1 回、write の中の非 block の dispatch |
| S-7 lost と時計 | D10 の黙った sink、§5 の close の約束 |
| S-8 host の build | §7: libmedia が audio.c を build しなくなり、2 本の script から外す（Q1 経由） |
| S-9 host の libwayland | §8 の 1: tree の libwayland で |
| S-10 pump の書き手 | §6.4: command・report の queue、位置は pump だけ、close は待たず tick で join、signal の block |
| S-11 read の先行 | D8: 時計は played |
| S-12 errno | §3 の `KL_AUDIO_ERROR_*` |
| M-1 played | D8 に書いた |
| M-2 版 | D2: `AUDIOD_VERSION == 1` の static assert |
| M-3・U3 sequence | §4: lock-free の static assert、sequence を使わない |
| M-4 connect | D6・§6.3 |
| M-5 manager の版 | D3: 別の global にして消えた |
| M-6 見える範囲 | §3 の末尾 |
| M-7 WAYLAND_SOCKET | D4 |
| M-8 quiesce | §5 の close |
| M-9 観測できない物 | §8 の QEMU から外した |
| M-10 libmedia の依存 | D11・H3 |
| M-11 poll | D5: tick のまま |
| M-12 fstat | §4 |

## 13. 第 2 版の review への対応（第 3 版）

| review | 対応 |
| --- | --- |
| B-1 flush の意味 | §3 の表を start・stop・flush・drain の列に分け、flush は running・stopped を変えない、draining の flush は drain を取り消して stopped（zedBSD は STOP と FLUSH）、状態は result NONE で移す、想定外の事象は捨てる。§8 に running の flush の試験 |
| should-fix 1 drain の result の重複 | result は backend の report だけ、control の EAGAIN・ENOTCONN の写し（§3・§6.3） |
| 2 時計の基準点 | D8・§7: 基準点も played |
| 3 played_sequence | §4: played の組は seqlock（audiod と同じ）、keiland-backend.h の注記は p002 で直す |
| 4 `_reap` | §6.2: `_reap` と `_reap_all` |
| 5 遅れた READY の fd | §3・§6.2・§6.3: backend の close が閉じる、試験は backend 単体と pump へ |
| 6 送れない時 | §3: 接続を閉じる、§5: libkeiland は EOF・error を lost(GONE) に |
| 7 pump の無音 | §6.4: 無音の数を delay から引く、played は単調、draining は無音無し、drain の終わりの判定 |
| 8 FreeBSD の環境 | §8: FreeBSD は backend 単体の program、wav の audiodev の tools の変更は Q1 経由（p004 の依存） |
| 9 規約の Phase | §9 に p005 |
| 10 zedBSD の BROKEN | D10・§4: zedBSD では来ない既知の限り |
| minor | D11 の誤りを直し表の試験は WS121 の host 試験で、libmedia の stream の本数の申し送り（§7）、本数は pending・ready だけ（D9）、zedBSD の EAGAIN（D6）、audiod の ERROR の写し（§6.3）、buffer の下限（D9）、EPIPE と running（D10・§7）、vp_audio の field（§7）、WAYLAND_DISPLAY 無しは ENOTSUP（D4）、§8 の 1 の作り方、alsa の open の mutex（§6.4）、signal の mask（U3）、書き間違い（D7、ws.md の目標、phase.md の KL_VERSION は下書きの節として残す）、peer の置き場所（§6.2）、kl_audio_format の版（§5） |
