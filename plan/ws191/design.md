<!-- awesome-plan project=zedbsd record=ws191-design -->

# WS191 の設計: 再生の音の stream を libkeiland の口へ（第 1 版、ws191-p001）

Parent: [WS191](ws.md)。事実の調べは [phase001](phase001/phase.md) の「調べた事実」。

## 1. 目的と境界

- 由来: WS188 p003 の境界の検査で、videoplayer・music・libmedia が `userland/desktop/videoplayer/audio.c` で zedBSD の audiod に直に繋いでいる（`userland/base/audiod/protocol.h` を include、AF_UNIX、SCM_RIGHTS の shm）。ユーザー（2026-10-08）:「libkeilandのaudio streamの口に移す」。Guardrail の「Bluetooth と Display も compositor 経由」（app → libkeiland → compositor → libkeiland-backend → OS、サウンドも同様）。
- 範囲: **再生（playback）の stream** だけ。録音（capture）・stream ごとの音量・device の選択は範囲の外（音量は今の `kl_system_audio_v1` のまま）。
- 完了で `plan/tools/keiland-os-boundary/app-allow.tsv` の audiod の PENDING 3 行（A1・A2・A5、videoplayer/audio.c）を外す。

## 2. 決定

| ID | 決定 | 理由 | 他の案 |
| --- | --- | --- | --- |
| D1 | **音のデータは compositor を通さない**。compositor は stream を作り、ring の共有 memory の fd を client に渡す。client は ring に直に書き、位置は ring の頭を直に読む。制御（開始・停止・flush・drain・破棄）だけが libkeiland → compositor → backend を通る | 帯域（48 kHz・16 bit・2 ch で 192 KB/s）と遅れを今と同じに保つ。compositor の main loop に音の仕事を載せない。fd を event で渡すのは keymap の先例（`kwl_emit_fd`） | compositor が Wayland の message で音を中継する（帯域・遅れ・compositor の負荷で不可）、client に audiod の socket の fd を渡す（境界の意味が無い） |
| D2 | **ring の約束は Keiland の物**として `libkeiland/system/kl-system-protocol.h` に書く（§4）。配置は audiod の shm の頭と同じ offset。zedBSD の backend は audiod の shm の fd をそのまま渡し、offset の一致を backend の source の static assert で確かめる。client は magic を見ない（server の tag）、version・frame_bytes・capacity・memory の大きさを検べる | zedBSD で copy も thread も要らない。Linux・FreeBSD の backend は同じ形の ring を自分で作る | Keiland 独自の ring を作り zedBSD でも backend の thread が audiod の ring に写す（無駄な copy と thread） |
| D3 | protocol: `kl_system_manager_v1` を**版 24** にし、request 16 `get_audio_stream(new_id kl_system_audio_stream_v1, uint format, uint channels, uint rate, uint buffer_frames, uint period_frames)` を足す（§3） | 既存の system の拡張の形（get_* で子の object） | 別の global |
| D4 | libkeiland の口は **stream ごとに自分の Wayland の接続**を開く（`WAYLAND_DISPLAY`）。制御は stream の mutex の下で request を送り、result を待つ（その接続の往復、2 秒で ETIMEDOUT） | libmedia（browser の `<video>`）は窓も display も持たず、音を engine の thread から書く。app の display の thread と分ければ thread の安全を考えずに済む | app の `wl_display` を共有（thread の制約と libmedia が困る） |
| D5 | compositor は backend を**待たずに**使う: 作成・制御の答えは `kwl_system_tick`（poll は最大 10 ms で回る）で受けて event を送る。作成は `ready`（fd 付き）か `failed`、制御は `result` | compositor の他の system の口と同じ形（volume.c・machine-wait.c）。制御の遅れは 10 ms 以内 | backend の答えを待つ（main loop が止まる） |
| D6 | zedBSD の backend は **stream ごとに audiod への接続 1 本**（今の app の client と同じ単位）。HELLO → STREAM_CREATE を非 block で送り、STREAM_CREATED と SCM_RIGHTS の fd を tick で受ける | audiod の側は何も変わらない。1 本の接続を共有すると 1 つの stream の詰まりが他を巻き込む | compositor の接続 1 本に全部の stream |
| D7 | Linux・FreeBSD の backend（p004）は memfd（FreeBSD は `shm_open(SHM_ANON)`）で §4 の ring を作り、stream ごとの **backend の thread** が ring から ALSA PCM（`default`、PipeWire の機械では pipewire-alsa を通る）・OSS（`/dev/dsp`）へ書き、read・played の位置を進める。thread から compositor への知らせ（drained・underrun・失敗）は stream の pipe に 1 byte を書き、tick で読む | ALSA は今の音量の backend（audio-linux.c）が既に使う。PipeWire の library を新しく足さない | libpipewire（依存が増える）、helper の process |
| D8 | 時計（A/V の同期）は今と同じく **read_position**（device 側が ring から取った frame の数）。より正確な `played_position`・`played_time_ns`（CLOCK_MONOTONIC）も口で出す（埋めない backend は 0） | 今の player の計算を変えない | — |
| D9 | 限り: 1 client の stream は 8 本、全体で 32 本。format は S16_LE（1）・S32_LE（2）・F32_LE（3）、channels 1〜8、rate 8000〜192000、buffer は period 以上で 2^20 frame 以下。外は `failed(EINVAL)` | 資源の乱用を防ぐ。数は audiod の番号に合わせる | — |
| D10 | client の切断・stream の破棄で compositor は backend の stream を閉じる（zedBSD: STREAM_DESTROY と接続の close）。audiod が落ちた・再起動した時は backend が知り、compositor は `lost` を送る。client の制御は EPIPE、ring への書き込みは害無く続く（client の mapping は残る）。player は lost で開き直す（p003） | 状態を compositor に溜めない | — |

## 3. protocol（kl_system_audio_stream_v1、manager 版 24 から）

```
kl_system_manager_v1
  request 16 get_audio_stream(new_id kl_system_audio_stream_v1, uint format, uint channels, uint rate,
                              uint buffer_frames, uint period_frames)        since version 24 (ws191)
kl_system_audio_stream_v1
  request 0 destroy
  request 1 start(uint request)          the device reads the ring from now
  request 2 stop(uint request)           a pause: what is written stays
  request 3 flush(uint request)          drops what is written and not read (the read position takes the write's)
  request 4 drain(uint request)          drained comes when everything written has been played
  event   0 ready(fd ring, uint bytes, uint capacity_frames, uint rate)   once, after the create
  event   1 failed(uint error)            once, instead of ready: EINVAL, ENODEV (no sound device), EAGAIN (the
                                          service is not up), ENOMEM, EMFILE (too many streams)
  event   2 result(uint request, uint error)   one for each start, stop, flush and drain
  event   3 drained
  event   4 underrun(uint count)          the device found the ring empty while running (count so far)
  event   5 lost                          the stream is gone (the service went); requests answer EPIPE
```

capabilities の bit `KL_SYSTEM_CAPABILITY_AUDIO_STREAM`（0x20000）は backend が stream を持つ時だけ（unsupported の backend は無し）。

## 4. ring（Keiland audio ring）

共有 memory の先頭 4096 byte が頭、その後に `capacity_frames × frame_bytes` byte の音（frame は channel の interleave）。offset（byte）:

| offset | 型 | 内容 | 書き手 |
| --- | --- | --- | --- |
| 0 | u32 | tag（server の物、client は見ない） | server |
| 4 | u32 | version（1） | server |
| 8・12・16 | u32 | format・channels・rate | server |
| 20 | u32 | frame_bytes | server |
| 24・28 | u32 | capacity_frames・period_frames | server |
| 64 | u64 | write_position（client が書いた frame の数、増えるだけ） | client |
| 72 | u32 | write_sequence | client |
| 128 | u64 | read_position（device 側が取った数） | server |
| 136 | u32 | read_sequence | server |
| 192 | u64 | played_position | server |
| 200 | s64 | played_time_ns（CLOCK_MONOTONIC） | server |
| 208 | u32 | played_sequence | server |
| 212・216・220 | u32 | underruns・overruns・state（0 stopped、1 running、2 draining） | server |

位置の読み書きは 8 byte の atomic（acquire・release）。8 byte の atomic が lock-free でない ABI（i386）では sequence の語（奇数の間は書き途中）で 2 つの半分を 1 つに読む。audiod の `audiod_position_load`・`_store` と同じ規則を libkeiland（`libkeiland/system/audio-ring.h`、static inline）と Linux・FreeBSD の backend に持つ（audiod の header は include しない）。

## 5. libkeiland の口（KL_VERSION 73 の予定（main は 72 を使用済み、2026-10-08 夕）、merge で Q1 が確かめる）

```c
struct kl_audio_stream;
struct kl_audio_stream_options {
	unsigned format;        /* KL_AUDIO_FORMAT_S16_LE 1, _S32_LE 2, _F32_LE 3 */
	unsigned channels;
	unsigned rate;
	unsigned buffer_frames;
	unsigned period_frames;
};
int kl_audio_stream_open(const struct kl_audio_stream_options *options, struct kl_audio_stream **stream);
    /* 0; ENOTSUP (no Keiland, or a compositor before version 24), ENODEV (no sound device), EAGAIN, EINVAL,
       EMFILE, EPIPE (the compositor went), ETIMEDOUT, ENOMEM */
void kl_audio_stream_close(struct kl_audio_stream *stream);
int kl_audio_stream_start(struct kl_audio_stream *stream);
int kl_audio_stream_stop(struct kl_audio_stream *stream);
int kl_audio_stream_flush(struct kl_audio_stream *stream);
int kl_audio_stream_drain(struct kl_audio_stream *stream);      /* sends; drained comes as an event */
size_t kl_audio_stream_write(struct kl_audio_stream *stream, const void *frames, size_t count);
                                                                /* into the ring, as many as there is room for */
uint64_t kl_audio_stream_written(const struct kl_audio_stream *stream);
uint64_t kl_audio_stream_read(const struct kl_audio_stream *stream);
int kl_audio_stream_played(const struct kl_audio_stream *stream, uint64_t *frames, int64_t *time_ns);
unsigned kl_audio_stream_capacity(const struct kl_audio_stream *stream);
unsigned kl_audio_stream_rate(const struct kl_audio_stream *stream);
int kl_audio_stream_fd(const struct kl_audio_stream *stream);  /* readable when an event came */
int kl_audio_stream_dispatch(struct kl_audio_stream *stream, unsigned *events);
    /* KL_AUDIO_EVENT_DRAINED 1, _UNDERRUN 2, _LOST 4; never waits */
```

- 書き手は 1 つの thread（write・written・read・played は lock 無し）。制御と dispatch は stream の mutex の下（どの thread からでもよい）。
- open は接続 → registry で manager（版 24 以上）→ `get_audio_stream` → `ready` か `failed` を待つ（2 秒）→ ring を mmap（`MAP_SHARED`）して検べ、fd を閉じる。

## 6. compositor と backend

- compositor: 新 `wayland/audio-stream.c`。client ごとの stream の表（D9 の数）、request の検べと backend への受け渡し、`kwl_system_tick` で backend の答え・事象を受けて event（`ready` は `kwl_emit_fd`）、client の切断・destroy で閉じる。log: `KWL AUDIO stream client=N id=M create format=… channels=… rate=… buffer=…`、`ready capacity=…`、`failed error=…`、`start|stop|flush|drain result=…`、`lost`、`closed`。
- backend の口（`libkeiland-backend/keiland-backend.h`）:
  - `struct kl_backend_audio_stream *kl_backend_audio_stream_open(const struct kl_backend_audio_stream_format *format)`（待たない、NULL は ENOMEM 等）
  - `int kl_backend_audio_stream_update(stream, struct kl_backend_audio_stream_report *report)`（ready と fd・failed・result・drained・underrun・lost を取り出す、待たない）
  - `int kl_backend_audio_stream_control(stream, unsigned what, uint32_t request)`（start・stop・flush・drain）
  - `void kl_backend_audio_stream_close(stream)`
  - `int kl_backend_audio_stream_supported(void)`
- zedBSD: `libkeiland-backend-zedbsd/audio-stream-zedbsd.c`（新）。D6 の接続、HELLO の WELCOME の device 0 は failed(ENODEV)、STREAM_CREATED の fd を ready に、DONE・ERROR を result に、DRAINED・UNDERRUN を事象に、REQUEST と VOLUME_CHANGED は読み捨て、socket の close は lost。§4 の offset と `struct audiod_shm_header` の offset の一致を `_Static_assert`。
- Linux・FreeBSD（p004）: D7。それまでの build は `unsupported/audio-stream-unsupported.c`（supported 0、open は NULL）。

## 7. app の移行（p003）

`videoplayer/audio.c` の `vp_audio_*` の中身を `kl_audio_stream_*` に置き換える（口の名と意味は保つ: open・close・start・stop・flush・read_position・write_position・write）。audiod の header と socket を使わない。videoplayer・music・libmedia の Makefile に libkeiland の link を足す（videoplayer・music は既に link、libmedia は新しく）。lost は player が開き直す。境界の許可の表の PENDING 3 行を外し、検査（`check.sh` の A1・A2・A5）が通ること。

## 8. 試験

- host（p002）: libkeiland の ring の読み書き（i386 の sequence の形も、-m32 が使えれば）と口の検べ（fake の compositor）、compositor の audio-stream.c と zedBSD の backend を fake の audiod（`plan/ws100/tests/host-audio.c` の形）で: 作成 → ready の fd、format の拒否、制御の result、drained・underrun、audiod の切断で lost、client の切断で STREAM_DESTROY、数の上限。端から端（libkeiland → compositor → backend）は `plan/ws131/tests/host-system.sh` の形に audio stream の段を足す。
- QEMU（T1、p003 の後）: audiod の入った image（`plan/ws100/tests/config-amd64-audiod.mk` の形）で Music か Video Player で音のある file を再生し、compositor の log に `KWL AUDIO stream … ready`、player の時計が進む（再生の位置の表示）、停止・seek・終わりで drain、player を閉じて `closed`。audiod の client の数が player の分だけ（compositor の接続 1 本 + 音量の 1 本）。
- 実機: 5330 で音が出ること（ユーザーの UAT）。

## 9. Phase と受け入れ

| Phase | 内容 | 受け入れ |
| --- | --- | --- |
| p001 | この設計、design-reviewer、Q1 の判定 | review の反映、Q1 の ACK |
| p002 | libkeiland（KL_VERSION 73）・protocol（manager 24）・compositor の audio-stream.c・zedBSD の backend・unsupported の backend・host 試験 | §8 の host 試験 PASS、zedBSD・Linux・FreeBSD の build warning 0 |
| p003 | videoplayer・music・libmedia の移行、境界の表の PENDING を外す、T1 の QEMU | 境界の検査 PASS、§8 の QEMU |
| p004 | Linux（ALSA）・FreeBSD（OSS）の backend の thread と ring | host 試験、Linux・FreeBSD の QEMU+KVM guest で再生（T1） |

## 10. 人の判断（Q1 経由で、既定の案つき）

| ID | 問い | 既定の案 |
| --- | --- | --- |
| H1 | 録音（capture）と stream ごとの音量を範囲の外にしてよいか | 外（必要になった時に別 WS） |
| H2 | Linux の再生を ALSA PCM の `default` にし、PipeWire の library は足さない（pipewire-alsa を通る）でよいか | 可 |

## 11. 未確認・リスク

- U1: audiod の STREAM_STOP の「書いた物は残る」と ALSA・OSS の pause の違い（p004 で `snd_pcm_pause` が無い device は drop、read_position の扱いを決める）。
- U2: compositor の tick が 10 ms より遅れる時（重い合成）の制御の遅れ。player は start・stop を待つので体感に出るかは QEMU で見る。
- U3: i386 の build で libkeiland の 8 byte の atomic（zedBSD i386 で使う clang の `__atomic` の lock-free）。
