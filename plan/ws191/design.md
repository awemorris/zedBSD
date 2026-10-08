<!-- awesome-plan project=zedbsd record=ws191-design -->

# WS191 の設計: 再生の音の stream を libkeiland の口へ（第 2 版、ws191-p001）

Parent: [WS191](ws.md)。事実の調べと第 1 版への review（blocking 3・should-fix 12・minor 12）は [phase001](phase001/phase.md)。
第 2 版の変更の要点は §12 の対応表（review の ID ごと）。

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
| D4 | libkeiland の口は **stream ごとに自分の Wayland の接続**を開く（Q1 の判断 2026-10-08 午後で維持）。`WAYLAND_SOCKET` が環境に有る時は開かず `ENOTSUP`（tree の libwayland の `wl_display_connect` は名前より先に `WAYLAND_SOCKET` を消費する、client.c 69〜90、M-7。それで起動される client は入力方式だけで音を出さない）。接続の名は `WAYLAND_DISPLAY`（無ければ libwayland の既定） | libmedia（browser の `<video>`）は display を持たず、音を engine の thread から書く。app の display の thread と分ければ thread の安全を考えずに済む | app の `wl_display` に library の queue で載せる（S-2 の別案。libmedia が困る） |
| D5 | compositor は backend を**待たずに**使う。作成・制御の答えと事象は `kwl_system_tick` から呼ぶ `kwl_audio_tick`（poll は最大 10 ms、main.c 855）で backend から取り出して event にする。backend の fd を poll に入れる口は作らない（M-11: 10 ms の tick で足り、本数は最大 32） | compositor の他の system の口と同じ形（volume.c・machine-wait.c）。制御の遅れは 10 ms 以内 | backend の答えを待つ（main loop が止まる）、poll の口を足す |
| D6 | zedBSD の backend は **stream ごとに audiod への接続 1 本**（今の app の client と同じ単位）。`SOCK_NONBLOCK` の connect（`EINPROGRESS` は書けるようになるのを tick で見る、M-4）→ HELLO → WELCOME → STREAM_CREATE → STREAM_CREATED と SCM_RIGHTS の fd、全部 tick で待たずに | audiod の側は何も変えない。1 本の接続を共有すると 1 つの stream の詰まりが他を巻き込む | compositor の接続 1 本に全部の stream |
| D7 | Linux・FreeBSD の backend（p004）は ring を **memfd** で作り（Linux `memfd_create(MFD_CLOEXEC | MFD_ALLOW_SEALING)`、FreeBSD 13 以降も同じ）、大きさを決めてから `F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL` で封じる（B-1: client が縮めて compositor の thread が SIGBUS で落ちるのを防ぐ）。stream ごとの **pump の thread**（libkeiland-backend の中、§6.3）が ring から Linux は **alsa-lib を dlopen** した PCM の `default`（ユーザーの決定 H2、普通は pipewire-alsa を通って PipeWire）へ、FreeBSD は OSS（`/dev/dsp`）へ書く | ユーザーの決定（H2: alsa-lib を dlopen、無ければ stream を断る）。dlopen なので compositor の link と package の依存は増えない | libpipewire の link、pipewire-pulse の native protocol を自前で、compositor の外の helper process（第 1 版 review B-2 の 3 案、ユーザーが dlopen を選んだ） |
| D8 | 時計（A/V の同期）は **played_position**（device から実際に出た分の見積り、§4）。libkeiland の `kl_audio_stream_position`。room の計算は read_position（device 側が ring から取った数）の `kl_audio_stream_consumed` | Linux・FreeBSD は PCM・PipeWire・OSS の buffer の分 read が先行する（S-11）。played は `snd_pcm_delay`・`SNDCTL_DSP_GETODELAY` で引いて埋める。zedBSD の audiod の played は mix の時点の read と同じ値（mix.c 274、M-1）なので zedBSD の振る舞いは今と変わらない | read_position を時計に（Linux・FreeBSD で A/V が buffer の分ずれる） |
| D9 | 限り（S-2・S-4）: **接続の相手の pid ごとに 8 本、compositor の全体で 32 本**（audiod の client の上限 64 の内、compositor の音量の接続などを残す）。pid は `SO_PEERCRED` で接続の時に kernel が記録した物（zedBSD は `struct kern_peercred.pid`、Linux は `struct ucred.pid`、FreeBSD は `LOCAL_PEERCRED` の `cr_pid`）、新しい backend の口 `kl_backend_peer_pid`。format は S16_LE（1）・S32_LE（2）・F32_LE（3）、**channels 1〜2**（audiod の上限、main.c 482）、rate 8000〜192000、buffer_frames は 0（backend の既定）か period_frames 以上で、`buffer_frames × frame_bytes ≤ 1 MiB`、period_frames は 0（既定）か buffer 以下。外は `failed(INVALID)`、本数の超過は `failed(TOO_MANY)` | 資源の乱用を防ぐ。stream ごとの接続（D4）では接続の番号で数えても効かないので、相手の process で数える | 1 client 8 本（第 1 版、D4 と矛盾） |
| D10 | backend の stream が終わった（audiod が落ちた・切った、alsa-lib の PCM の失敗、client の位置の不正）時、compositor は `lost(error)` を送る。libkeiland は lost の後の stream を**黙った sink** にする: 書くのは今までどおり ring へ（client の mapping は残る）、consumed・position は lost の時点から monotonic の時計で rate の速さで進む（running の間だけ、written を超えない）。制御は手元の状態だけ変えて `EPIPE` を返す。player は開き直さず、次の file を開く時に新しい stream を試す（p003） | S-7: 再生中に開き直すと他の thread の時計の読みと munmap が競合し、新しい stream は位置 0 から。黙った sink なら player の時計と書く loop は止まらず、close は今の close の道だけ | lost で player が開き直す（第 1 版） |
| D11 | **libmedia は libkeiland を link しない**。libmedia の音は埋め込む側が渡す出力の表（`struct media_audio_output`、関数の表）を通る。Video Player・Music は表を kl_audio_* で埋め、browser は libbrowser の公開の口でその表を渡す（**H3、Q1・ユーザーの判断が要る**） | browser の規則（plan/standards/browser-component.md 4・6）: libbrowser は「直接の link・実行時の呼出しに Wayland/Keiland の窓・protocol の依存を入れない」。libmedia は libbrowser に link されるので、libmedia から kl_audio_*（Keiland の protocol）を呼ぶとこれに当たる（M-10 の依存の増加も消える） | libmedia が libkeiland を link（規則 4 に反する）、`dlsym(RTLD_DEFAULT)` で kl_audio_* を探す（link は無いが実行時の呼出しで規則 4 に反する）、browser の音を後回し（H3 の案 c） |

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
  request 3 flush(uint request)           drops what is written and not read (read takes write's value)
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

状態機械（S-1、compositor が stream ごとに持つ）:

| 状態 | 入り | start・stop・flush | drain | backend の事象 |
| --- | --- | --- | --- | --- |
| pending | create_stream を受けた | `result(STATE)` | `result(STATE)` | ready → stopped（`ready` を送る）、failed → failed（`failed` を送る） |
| stopped | ready、stop、flush、drained | backend へ、答えで `result` | backend へ、受けた時点で `result(NONE)`、→ draining | lost → lost |
| running | start | 同上（stop・flush は → stopped） | 同上 → draining | underrun → `underrun`（間引き）、lost → lost |
| draining | drain | backend へ（drain を取り消す、drained は来ない）、start → running、stop・flush → stopped | 新しい drain が前の drain を置き換える（前の request の drained は来ない） | drained → stopped（`drained(request)`）、lost → lost |
| failed | failed を送った | `result(STATE)` | `result(STATE)` | — |
| lost | lost を送った | `result(GONE)` | `result(GONE)` | — |

- destroy はどの状態でも受け、backend の stream を閉じる。pending の間の destroy の後に backend の ready が来たら、compositor はその fd を閉じる（object が無い）。
- stopped の drain は device を動かして流し切る（audiod の STREAM_DRAIN の振る舞い、main.c 357〜372）。drained の後は stopped で、続けるには start が要る（device.c 510〜513）。
- underrun は stream ごとに **1 秒に高々 1 回**、count は最新の合計（S-6: audiod は空の間 period ごとに UNDERRUN を出す、mix.c 277〜282。毎秒約 100 の event は読まない client の出力の queue を ENOBUFS にする）。
- event を送れない（`kwl_emit_fd`・`kwl_emit` の ENOBUFS・ENOMEM、wire.c 117〜133）時は、その stream の backend を閉じて状態を lost にする（それ以上 event を送らない。client は接続ごと詰まっている）。ready の fd は `kwl_emit_fd` が失敗の時に閉じる。
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
| 208 | u32 | played_sequence（同上） | — |
| 212・216・220 | u32 | underruns・overruns・state（0 stopped、1 running、2 draining） | server |

- 位置は **8 byte の atomic**（読みは acquire、書きは release）。Keiland の code（libkeiland の `libkeiland/audio/audio-ring.h`、Linux・FreeBSD の backend）は `__GCC_ATOMIC_LLONG_LOCK_FREE == 2` を `_Static_assert` し、sequence の語を使わない（M-3・U3: 対象は amd64 だけで、audiod も amd64 では sequence を進めない、protocol.h 209〜212。lock-free でない ABI の build は compile で止まる）。
- played_position と played_time_ns の 2 つの語は別々の atomic で、読み手は played_time_ns を先に、position を後に読む（多少の食い違いは時計の 1 period 以内）。
- client（libkeiland）は ready の後に `fstat` で大きさが `4096 + capacity × frame_bytes`（切り上げ）以上で `bytes` と合うことを確かめてから `MAP_SHARED`（読み書き）で map し（M-12）、頭の version・format・channels・rate・frame_bytes・capacity_frames が求めた値と event の値に合うことを検べる。合わなければ `EPROTO` で閉じる。
- server（Linux・FreeBSD の pump）は頭の server の欄を**自分の手元の値**から使い、共有 memory からは write_position だけを読む。その値を検べる: `write < read` か `write − read > capacity` は BROKEN で lost（audiod の mix.c 207〜216 と同じ規則）。

## 5. libkeiland の口（KL_VERSION 73 の予定、merge で Q1 が揃える。`include/keiland/keiland.h`）

```c
#define KL_AUDIO_FORMAT_S16_LE	1U
#define KL_AUDIO_FORMAT_S32_LE	2U
#define KL_AUDIO_FORMAT_F32_LE	3U
#define KL_AUDIO_EVENT_DRAINED	0x1U
#define KL_AUDIO_EVENT_UNDERRUN	0x2U
#define KL_AUDIO_EVENT_LOST	0x4U

struct kl_audio_format {
	unsigned format;	/* KL_AUDIO_FORMAT_* */
	unsigned channels;	/* 1 or 2 */
	unsigned rate;		/* 8000 to 192000 */
	unsigned buffer_frames;	/* the ring, 0 for the backend's choice */
	unsigned period_frames;	/* 0 for the backend's choice */
};
struct kl_audio_stream;

int kl_audio_stream_open(const struct kl_audio_format *format, struct kl_audio_stream **stream);
	/* 0; ENOTSUP (no Keiland, no kl_audio_v1, WAYLAND_SOCKET set, or a backend without streams), ENODEV,
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
- `written`・`consumed`・`position`・`capacity`: lock 無し、どの thread からでも。position は played の frame の数（time_ns に見積りの時刻）。
- `dispatch`: 非 block（trylock、取れなければ events 0 で 0 を返す）。socket に有る分を読み、溜まった bit を返して消す。**fd の口は出さない**（S-5: 制御の往復の間に libwayland が読んだ event は queue に移り、fd は読めなくなる。今の player は drained を待たない）。
- lost の後（D10）: write は ring に書き続け（害は無い）、consumed・position は黙った sink の値（lost の時点の値から monotonic の時計で running の間だけ rate の速さで、written を超えない）。黙った sink の基準（時刻・位置・running）は mutex の下で書き、読み手は stream の中の sequence の語で 1 つに読む。制御は手元の running を変えて `EPIPE` を返す。
- close: 呼ぶ側の約束 — **他の thread がその stream を使い終わってから**（今の vp_audio_close と同じ。p003 は各 player の close の順を確かめる）。destroy を送り、munmap し、接続を閉じる（compositor では切断で `kwl_compose_quiesce` が走る、objects.c 533。M-8: player は曲・file ごとに開き直さない）。

## 6. compositor と backend

### 6.1 compositor（`wayland/audio-stream.c`、新）

- kind `KWL_AUDIO`（global）と `KWL_AUDIO_STREAM`。stream の record: client、object、pid（接続の最初の create で `kl_backend_peer_pid`、失敗は pid 0 として 1 つの組に数える）、状態（§3）、backend の stream、最後の underrun を送った時刻。
- create_stream: D9 の検べ（値 → INVALID、本数 → TOO_MANY）→ `kl_backend_audio_stream_open`。`kwl_audio_tick`（`kwl_system_tick` から）で全 stream の `kl_backend_audio_stream_next` を空になるまで取り、§3 の状態機械で event にする。client の切断・destroy で backend の stream を閉じる。compositor の終わりで全部閉じる。
- `_Static_assert`: `struct kl_backend_audio_ring` の各 offset と `KL_AUDIO_RING_*`（kl-audio-protocol.h）、`KL_BACKEND_AUDIO_ERROR_*` と `KL_AUDIO_ERROR_*`、`KL_BACKEND_AUDIO_FORMAT_*` と `KL_AUDIO_FORMAT_*` の値。
- log: `KWL AUDIO stream client=N id=M pid=P create format=F channels=C rate=R buffer=B period=Q`、`ready capacity=… bytes=…`、`failed error=…`、`start|stop|flush|drain request=… error=…`、`drained request=…`、`underrun count=…`（間引いた後の物）、`lost error=…`、`closed`。

### 6.2 backend の口（`libkeiland-backend/keiland-backend.h`、新しい節）

```c
struct kl_backend_audio_ring;		/* §4 の配置、_Alignas(64) の 3 つの組 */
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
void kl_backend_audio_stream_close(struct kl_backend_audio_stream *stream);	/* never waits */
int kl_backend_peer_pid(int descriptor, pid_t *pid);
```

- compositor は `kl_backend_audio_stream_supported()` が 0 の時は `kl_audio_v1` を広告しない（libkeiland は `ENOTSUP`）。
- 置き場所: zedBSD は `libkeiland-backend-zedbsd/audio-stream-zedbsd.c`・`peer-zedbsd.c`（`SO_PEERCRED` の `struct kern_peercred`、networkd の先例）、Linux は `libkeiland-backend-linux/audio-stream-linux.c`・`peer-linux.c` に pid、FreeBSD は `libkeiland-backend-freebsd/audio-stream-freebsd.c`・`peer-freebsd.c`（`LOCAL_PEERCRED`）、pump と変換の共通部は `libkeiland-backend/audio/pump.c`（Linux・FreeBSD）。p004 までの Linux・FreeBSD は `libkeiland-backend/unsupported/audio-stream-unsupported.c`（supported 0）。

### 6.3 zedBSD の backend（p002）

- D6 の接続。HELLO の WELCOME で device 0 は `FAILED(NO_DEVICE)`。connect の `ENOENT`・`ECONNREFUSED`、WELCOME の前の EOF（audiod が満員で accept の直後に閉じた、main.c 214〜217）は `FAILED(UNAVAILABLE)`。
- STREAM_CREATED の fd を READY に（fd は compositor へ、backend は持たない）。audiod の serial は backend が振り、serial → request の小さな表を持つ。DONE・ERROR（その serial）は RESULT に。**DRAIN は audiod が DONE を返さない**（main.c 357〜372）ので、送れた時点で RESULT(NONE) を作り、DRAINED（header.serial が drain の serial）を DRAINED(request) に。drain の途中の start・stop・flush で drain の serial を忘れる（来ない DRAINED を待たない）。
- UNDERRUN は合計を覚え、tick ごとに最新を 1 つの UNDERRUN に。REQUEST・OVERRUN・VOLUME_CHANGED は読み捨て。recv は非 block で読める分を全部（AF_UNIX の buffer 64 KiB を溜めない）。send が `EAGAIN` なら control は `EAGAIN`（compositor は `result(UNAVAILABLE)`）。socket の EOF・error は `LOST(GONE)`。
- `_Static_assert`（D2）。audiod の client の数: stream 1 本につき 1。

### 6.4 Linux・FreeBSD の backend（p004）

- open: memfd（D7）の ring を作り封じる → pump の thread を起こし、ready は pump が device を開けた後（Linux: `snd_pcm_open("default", PLAYBACK, SND_PCM_NONBLOCK)` と `snd_pcm_set_params(format, RW_INTERLEAVED, channels, rate, soft_resample 1, 40 ms)`、FreeBSD: `open("/dev/dsp", O_WRONLY | O_NONBLOCK)` と `SNDCTL_DSP_SETFMT`・`CHANNELS`・`SPEED`）。device が F32・S32 を取らない時は pump が S16 に変換する（`audio/convert.c`）。開けなければ FAILED（device が無い NO_DEVICE、PipeWire などに繋がらない UNAVAILABLE、alsa-lib の symbol が無い UNSUPPORTED）。
- pump と main loop の受け渡し（S-10）: 制御は command の queue（mutex）と wake の pipe で pump へ。**read_position・played・state は pump だけが書く**（flush も pump が read = write にする）。pump から main loop への report（READY・RESULT・DRAINED・UNDERRUN・LOST）は report の queue（mutex）に積み、`kl_backend_audio_stream_next` が取り出す（tick で見るので pipe は要らない）。
- pump: 全 signal を `pthread_sigmask` で block。poll（PCM の `snd_pcm_poll_descriptors`・OSS の fd の POLLOUT と wake の pipe）→ running なら `min(avail, write − read)` を書いて read を進め、played = read − delay（`snd_pcm_delay`・`GETODELAY`）と時刻を書く。running で ring が空なら無音を 1 period 書き（device を止めない）、空になった移り変わりで underruns を 1 つ増やして UNDERRUN。stop は `snd_pcm_pause(1)`、できない device では `snd_pcm_drop`（PCM の buffer の ≤ 40 ms を捨てる、U1）、OSS は書くのをやめる。flush は drop と prepare・`SNDCTL_DSP_HALT_OUTPUT` と read = write。drain は ring が空になり delay が 0 になるまで流して DRAINED と stopped。
- close: pump に終わりを頼み wake するだけ（待たない）。pump の終わりを tick で見て `pthread_join`（終わった thread の join は待たない）。PCM の open が長く block しても main loop は止まらない。
- alsa-lib: `dlopen("libasound.so.2", RTLD_NOW | RTLD_LOCAL)` を最初の `supported` で 1 回、使う symbol（`snd_pcm_open`・`_set_params`・`_writei`・`_avail_update`・`_delay`・`_pause`・`_drop`・`_prepare`・`_recover`・`_poll_descriptors_count`・`_poll_descriptors`・`_poll_descriptors_revents`・`_close`・`snd_strerror`）が全部有る時だけ supported 1。alsa-lib・symbol が無ければ supported 0（音量の操作は今のまま、H2）。試験は library の名を compile の時の macro（`KL_BACKEND_ALSA_LIBRARY`）で偽の library に替える。

## 7. app の移行（p003）

- `videoplayer/audio.c` の `vp_audio_*`（口の名と意味は保つ: open・close・start・stop・flush・write・write_position・read_position に clock_position を足す）の中身を `kl_audio_stream_*` に。audiod の header と socket を使わない。videoplayer・music は既に libkeiland を link している。player の時計（media.c 276・music/play.c 276・562 など）は `clock_position`（D8 の played）に、room の計算は read_position（consumed）のまま。zedBSD では played と read は同じ値（D8）で振る舞いは変わらない。
- libmedia（D11、H3 の案 a の時）: libmedia の `media.h` に出力の表 `struct media_audio_output`（open・close・start・stop・flush・write・written・consumed・position の関数と context、S16_LE 2 ch）と `media_set_audio_output()`（今の `media_set_log` と同じ形の process 全体の設定）を足す。engine.c は表を通して書き、表が無ければ音無し（時計は monotonic、今の audiod が無い時と同じ）。libbrowser は公開の `browser.h` に Wayland・Keiland の型を含まない同じ形の表と設定の関数を足して libmedia へ渡し、browser の shell（libkeiland を link 済み）が kl_audio_* で埋める。libmedia は `videoplayer/audio.c` を build しなくなる。
- 時計と close（S-7）: lost で開き直さない（D10）。各 player の close（videoplayer main.c 255、music play.c 85、libmedia engine.c 581）が、書く thread・時計を読む thread の終わりの後であることを確かめる（違えば順を直す）。
- host の build（S-8）: libmedia が `videoplayer/audio.c` を build しなくなるので、`plan/ws074/tests/host-build.sh`（77〜88 行の libmedia の file の並び）と `plan/ws121/tests/run-host-engine.sh`（15 行）から `videoplayer/audio.c` を外す。他の WS の file なので Q1 経由で依頼する（p003 の依存）。
- 境界の許可の表の PENDING 3 行を外し、`check.sh` の A1・A2・A5 が通ること。

## 8. 試験

- host（p002）、`plan/ws191/tests/host-audio-stream.sh`（ASan・UBSan）:
  1. 端から端: libkeiland の `libkeiland/audio/` を **tree の libwayland**（Linux の keiland の build の `lib/libwayland-client.so`、Makefile.linux。S-9: host の libwayland-client は fd の受け取りが tree の物と違う）で link し、server の役の thread（compositor の `wayland/audio-stream.c` と `wire.c` の `kwl_emit_fd`、`WAYLAND_DISPLAY` に試験の socket）と偽の backend（試験が事象を作る）に繋ぐ。確かめる: open → ready の fd・map・頭の検べ（version・format の不一致は EPROTO）、format・channels 3・buffer の超過の EINVAL、同じ pid の 9 本目の EMFILE、制御の result、drain の result と drained、drain の途中の stop で drained が来ない、偽の backend の 100 回の underrun が 1 秒に 1 回以下の event に、lost で EPIPE と黙った sink の position が rate で進む、client の切断・destroy で backend の close、ready の前の destroy で遅れた fd が閉じる（試験の process の開いた fd の数）、`WAYLAND_SOCKET` が有る時の ENOTSUP。
  2. zedBSD の backend: `audio-stream-zedbsd.c` を host で、偽の audiod（`plan/ws100/tests/host-audio.c` の形、socket の path は `AUDIO_SOCKET_PATH` の macro で試験の path、audio-zedbsd.c 37 の先例）に: WELCOME の device 0 → NO_DEVICE、接続できない・accept の直後の close → UNAVAILABLE、STREAM_CREATED の fd → READY、DONE・ERROR → RESULT、DRAIN → すぐの RESULT と DRAINED、UNDERRUN の まとめ、EOF → LOST(GONE)。
- build（p002・p003）: zedBSD amd64 の libkeiland・wayland・videoplayer・music・libmedia・libbrowser（target を名指す）、Linux の keiland（`make keiland-linux`）、どれも warning 0。境界の検査 `sh plan/tools/keiland-os-boundary/check.sh`。
- QEMU（T1、p003 の後）: audiod の入った image（`plan/ws100/tests/config-amd64-audiod.mk` の形、WS191 の tests/ の config.mk）で Music の再生と Video Player の再生: compositor の log の `KWL AUDIO stream … ready` と `start`、player の再生の位置の表示が進む（screenshot 2 枚の差）、QEMU の wav の audiodev に無音でない音が書かれる、player を閉じて `closed`。browser の `<audio>` は H3 が案 a の時に同じ形で。（M-9: audiod の client の数と drain は観測できないので受け入れに入れない）
- Linux・FreeBSD（p004、T1）: 正弦波の試験の client（`plan/ws191/tests/tone.c`、libkeiland の `kl_audio_*` だけを使い 2 秒の 440 Hz を鳴らし、position の進みを出力に書く）を Linux の Debian 13 の QEMU+KVM guest と FreeBSD 15 の guest で。観測は QEMU の wav の audiodev（無音でない、440 Hz の山）と client の出力（position が 2 秒分 ±10 % 進む）。host 試験は pump を偽の alsa-lib（`KL_BACKEND_ALSA_LIBRARY`）で: 書いた frame の数、stop・flush・drain、BROKEN の位置、close が待たない。FreeBSD の build は guest の中の native の build（WS109 の手順）。
- 実機: 5330 で音が出ること（ユーザーの UAT）。

## 9. Phase と受け入れ

| Phase | 内容 | 受け入れ |
| --- | --- | --- |
| p001 | この設計、design-reviewer で blocking 0、Q1 の判定 | review の反映、Q1 の ACK、H3 の決定 |
| p002 | `kl-audio-protocol.h`・libkeiland の `kl_audio_*`（KL_VERSION 73、exports）・compositor の `audio-stream.c`・backend の口と `kl_backend_peer_pid`（3 OS）・zedBSD の backend・Linux・FreeBSD の unsupported・host 試験 | §8 の host 試験 1・2 が PASS、zedBSD amd64・Linux の keiland の build が warning 0 |
| p003 | videoplayer・music の移行、libmedia の出力の表（と H3 の a なら libbrowser・browser）、host の script 2 本の追従（Q1 経由）、境界の表の PENDING を外す、T1 の QEMU | 境界の検査 PASS、build warning 0、§8 の QEMU を T1 が PASS |
| p004 | Linux（alsa-lib の dlopen）・FreeBSD（OSS）の pump と ring、`tone.c` | host 試験（偽の alsa-lib）PASS、Linux・FreeBSD の guest で tone の音を T1 が観測、FreeBSD の native build が warning 0 |

## 10. 人の判断（Q1 経由で、既定の案つき）

| ID | 問い | 状態・既定の案 |
| --- | --- | --- |
| H1 | 録音（capture）と stream ごとの音量を範囲の外にしてよいか | 既定: 外（必要になった時に別 WS） |
| H2 | Linux の再生の経路 | **決定済み**（ユーザー 2026-10-08 午後）: alsa-lib を dlopen、無ければ stream を断る |
| H3 | browser（libbrowser の libmedia）の音の経路。libbrowser は Keiland の protocol に実行時にも依存できない（browser-component.md 4） | 既定: (a) libbrowser の公開の口に Wayland・Keiland の型を含まない音の出力の表を足し、browser の shell が kl_audio_* で埋める（`browser.h` の追加、WS074・WS107 の領域）。他: (b) libmedia が `dlsym(RTLD_DEFAULT)` で kl_audio_* を探す（規則 4 の例外が要る）、(c) browser の音は後の WS へ（この WS の受け入れから browser を外す） |
| D4・D9 | stream ごとの接続と pid ごとの限り | **決定済み**（Q1 2026-10-08 午後） |
| p004 | Linux・FreeBSD の受け入れ | **決定済み**（Q1 2026-10-08 午後）: 正弦波の試験の client |

## 11. 未確認・リスク

- U1: Linux・FreeBSD の stop で PCM の buffer の ≤ 40 ms が捨てられる device（`snd_pcm_pause` が無い物）。played は delay を引くので時計は ≤ 40 ms 進む。体感は p004 の guest と実機で見る。
- U2: compositor の tick が 10 ms より遅れる時（重い合成）の制御の遅れ。player は start・stop の答えを待つので、体感に出るかは QEMU で見る。
- U3: alsa-lib の PCM `default` が pipewire-alsa の時、compositor の process に libpipewire の plugin と thread が読み込まれる（ユーザーの決定 H2 の結果）。signal の扱い（pump は全 signal を block、PipeWire の thread は自分で block するか未確認）と compositor の終わりの順を p004 で確かめる。
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
