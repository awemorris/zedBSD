<!-- awesome-plan project=zedbsd record=ws035-audiod-design -->

# audiod の設計（unix socket＋共有メモリ）

Parent: [WS035](ws.md) / Phase: [ws035-p050](phase050/phase.md)
Status: 設計（2026-09-24）。実装は p049（`/dev/dsp` の mmap）と p009（audiod）、client 側は p019（libpulse）

## 1. ユーザーの決定（2026-09-24）

- audiod は**ミキシングを行う process**。負荷を下げるため、音は**共有メモリ**で受け渡し、**streaming の interface は持たない**。
- 共有メモリの fd は **SCM_RIGHTS** で渡す。memfd は無いので、**`shm_open` の直後に `shm_unlink`** して匿名にする。
- client 側は **libpulse の `pa_stream_write()`** がその共有メモリの buffer に書く。
- **`/dev/dsp0` に OSS の mmap interface** を持たせる。mmap に対応していれば audiod はそれを使い、非対応なら audiod が write する。
- 今はゼロコピーの DMA buffer は要らない。概念上の interface と、**コピーありでも動く実装**を最低限とし、ゼロコピーは後で別に目指す。
- audiod は、これを実現する **unix socket の interface** として設計する。

## 2. 全体

```
 client（libpulse）                      audiod                                kernel
 ┌───────────────┐  unix socket（制御）  ┌──────────────┐  mmap か write   ┌───────────┐
 │ pa_stream_*   │◀──────────────────▶│ mix / 配布    │────────────────▶│ /dev/dsp0 │
 │               │  SCM_RIGHTS で fd    │              │                 │ （HDA）    │
 │  shm の ring  │◀═══ 共有メモリ ═══▶│  shm の ring  │                 └───────────┘
 └───────────────┘                      └──────────────┘
```

- **socket に音は流さない**。socket を流れるのは制御（stream の作成・開始・停止・drain・音量）と、少ない通知（「書ける量が増えた」等）だけ。
- 音の標本は stream ごとの**共有メモリの ring** に置く。再生は client が書き audiod が読む、録音は audiod が書き client が読む（どちらも 1 対 1 の lock-free ring）。
- audiod は単一の thread の `poll()` の loop（epoll は使わない）。device の period ごとに全 stream を mix する（pull 型）。

## 3. socket

- path は **`/run/audiod.sock`**（`SOCK_STREAM`）。mode 0666（`/dev/dsp` と同じく、誰でも音を出せる。単一利用者の PC を前提）。
- message は Wayland の wire と同じ形の**長さ付き**: `uint32 type`、`uint32 length`（header を含む byte 数）、`uint32 serial`、本体。
  fd は SCM_RIGHTS の ancillary data で、その message と一緒に届く。
- 1 client は 1 接続。1 接続に複数の stream を持てる（stream id は client が決める 32 bit）。
- 版の合意: 接続直後に client が `HELLO { version }`、audiod が `WELCOME { version, device format, rate, channels, period_frames }`。

### 3.1 要求（client → audiod）

| type | 本体 | 意味 |
| --- | --- | --- |
| `HELLO` | version | 版の合意 |
| `STREAM_CREATE` | id, direction（play/capture）, format, rate, channels, buffer_frames, period_frames | stream を作る。audiod が共有メモリを作って返す |
| `STREAM_START` / `STREAM_STOP` | id | 開始・一時停止（pulse の cork） |
| `STREAM_DRAIN` | id | 書かれた分を全部鳴らし終えたら `DRAINED` を返す |
| `STREAM_FLUSH` | id | ring の未再生分を捨てる |
| `STREAM_VOLUME` | id, left, right, muted | stream ごとの音量（0〜65536、65536 が等倍） |
| `STREAM_DESTROY` | id | 閉じる。共有メモリは両者が unmap した時点で消える |
| `DEVICE_VOLUME` | left, right, muted | 全体の音量（`/dev/mixer0`）。libzdesktop の音量表示・操作が使う |
| `SUBSCRIBE` | mask | 音量・device の変化の通知を受ける |

### 3.2 応答と通知（audiod → client）

| type | 本体 | 意味 |
| --- | --- | --- |
| `WELCOME` | version, device の format・rate・channels・period | |
| `STREAM_CREATED` | id, shm_size ＋ **fd（SCM_RIGHTS）** | 共有メモリを mmap する |
| `ERROR` | serial, errno | 要求の失敗 |
| `REQUEST` | id | 再生: 書ける量が period 以上になった（pulse の write callback の契機）。録音: 読める量が period 以上になった |
| `DRAINED` | id | drain の完了 |
| `UNDERRUN` / `OVERRUN` | id, count | 取りこぼしの報告 |
| `VOLUME_CHANGED` | left, right, muted | 購読した client へ |

`REQUEST` は period ごとに最大 1 回（例: 48 kHz・period 960 frame で毎秒 50 回）。client が十分先まで書いていれば送らない。

## 4. 共有メモリ

### 4.1 作り方と渡し方

audiod が作る: `shm_open("/audiod-<pid>-<counter>", O_RDWR|O_CREAT|O_EXCL, 0600)` → **すぐ `shm_unlink`** → `ftruncate(size)` → `mmap` →
`STREAM_CREATED` と一緒に fd を SCM_RIGHTS で送り、自分の fd は閉じる（mapping は残る）。名前は一瞬しか存在せず、他の process からは開けない。

client が作らず audiod が作るのは、大きさと配置を audiod が決め、client が不正な大きさの object を渡す余地を無くすため。

### 4.2 配置

```
offset 0      struct audiod_shm_header（4096 byte、1 page）
offset 4096   ring: capacity_frames × frame_bytes（page の倍数に切り上げ）
```

```c
struct audiod_shm_header {
	uint32_t magic;            /* 'AUDD' */
	uint32_t version;
	uint32_t format;           /* KERN_AUDIO_FORMAT_S16_LE など、uapi/audio.h と同じ番号 */
	uint32_t channels;
	uint32_t rate;
	uint32_t frame_bytes;
	uint32_t capacity_frames;  /* ring の大きさ */
	uint32_t period_frames;
	/* 64 byte 境界で分け、書き手の違う値を同じ cache line に置かない */
	_Alignas(64) uint64_t write_position;   /* 再生: client、録音: audiod が進める（frame 単位、単調増加） */
	_Alignas(64) uint64_t read_position;    /* 再生: audiod、録音: client が進める */
	_Alignas(64) uint64_t played_position;  /* 再生: device が実際に鳴らした位置（audiod が書く） */
	int64_t played_time_ns;                 /* その位置の CLOCK_MONOTONIC */
	uint32_t underruns;
	uint32_t overruns;
	uint32_t state;                         /* running・stopped・draining。audiod が書く */
};
```

- 位置は**単調増加の 64 bit**（kernel の audio の ring と同じ考え方）。ring の中の位置は `position % capacity_frames`。
  空き = `capacity - (write - read)`。
- 書き手は標本を書いてから `write_position` を **release** で進め、読み手は **acquire** で読む（`__atomic_store_n`・`__atomic_load_n`）。lock は無い。
- audiod は client の値を信用しない: `write - read` が `capacity` を超える・戻るなどの矛盾は、その stream の `ERROR` として閉じる。

### 4.3 client が共有メモリを縮めたら

client は RW で mmap するので、`ftruncate` で object を縮められる。audiod が縮んだ範囲を読むと SIGBUS になる。
Linux は memfd の seal で防ぐが、zedBSD には無い。

- **当面**: audiod は ring を読む区間を `sigsetjmp`／SIGBUS の handler で囲み、SIGBUS を起こした stream を閉じる。
  audiod 自体は止まらない。
- **後で**: kernel に `F_ADD_SEALS`（`F_SEAL_SHRINK`・`F_SEAL_GROW`）を足す（別 Phase の提案）。audiod は作成直後に seal し、handler を外せる。

## 5. mix と device

### 5.1 device の開き方

audiod は `/dev/dsp0` を開き、`KERN_AUDIO_SET_FORMAT` で**device の既定の format**（HDA では S16_LE・2ch・48000 Hz）にする。
stream の format・rate・channels が違えば audiod が変換する（§5.3）。

`KERN_AUDIO_GET_CAPS`（p049 で追加）に mmap の bit があれば mmap の経路、無ければ write の経路を使う。

### 5.2 period ごとの処理（再生）

1. device の位置を知る（mmap: `KERN_AUDIO_GET_OPTR`、write: `KERN_AUDIO_GET_OSPACE`）。書けるのが period 1 つ分以上になるまで poll で待つ
   （`/dev/dsp` は period ごとに POLLOUT になる）。
2. 全 stream の ring から period 分を読み、32 bit の整数に広げ、stream の音量を掛けて足す。無い分は無音。足りなかった stream は `underruns` を増やす。
3. 全体を飽和させて S16 にし、device へ出す:
   - **mmap**: device の ring の、hardware の位置より先の period にそのまま書く。**copy は mix の結果の 1 回だけ**。
   - **write**: 手元の buffer に作って `write()` する（kernel が DMA の ring へ copy）。
4. 各 stream の `read_position` を進め、`played_position`・`played_time_ns` を更新し、必要なら `REQUEST` を送る。

### 5.3 変換

- channel: mono → 両方へ、stereo → mono は平均、それ以外の数は最初は拒否（`ERROR`）。
- format: S16_LE と S32_LE と FLOAT32_LE を受ける（uapi には S16_LE と S32_LE しか無いので、FLOAT32_LE は audiod と共有メモリの format としてだけ定義する。VLC・Chromium は float を使う）。
- rate: 最初は**線形補間**。音質の良い resampler は後で。

### 5.4 録音

audiod は device の録音を period ごとに読み、録音 stream の ring へ（変換して）書く。複数の client に同じ音を配る。
`read` 側（client）が遅れて ring が満ちたら、古い分を捨てて `overruns` を増やす。

## 6. `/dev/dsp` の mmap interface（p049）

OSS の mmap の考え方に合わせ、`include/uapi/audio.h` に足す:

| 追加 | 意味 |
| --- | --- |
| `KERN_AUDIO_GET_CAPS` | 能力の bit（`AUDIO_CAP_MMAP`、`AUDIO_CAP_TRIGGER` 等） |
| `mmap(dsp_fd, 0, ring_bytes, prot, MAP_SHARED)` | OSS と同じく **`PROT_WRITE` を含めば再生、`PROT_READ` だけなら録音**の ring を出す。system は write-only で開いた device を mmap させないので、再生は `O_RDWR` で開く（`O_RDWR` は hardware にある方向だけを取る。p049 で実装時に確定） |
| `KERN_AUDIO_GET_OPTR` / `KERN_AUDIO_GET_IPTR` | OSS の `count_info` 相当: 累計の byte 数、完了した fragment の数、ring の中の位置 |
| `KERN_AUDIO_SET_TRIGGER` | mmap のときの開始・停止（書き始めてから鳴らす） |

- mmap の中では `write()`・`read()` は使えない（`EBUSY`）。
- 実装は 2 段（ユーザーの指示どおり、コピーありを先に作り、ゼロコピーはその後に別の Phase で目指す）:
  1. **p049・コピーあり**: kernel が DMA とは別の page の ring を user に mmap させ、fragment ごとの割込みで、
     次に鳴る fragment をその ring から DMA の ring へ copy する（録音は逆向き）。
  2. **p056・ゼロコピー**: DMA の ring そのものを user に mmap させる（kernel の `vm_device` の mapping で、GPU と同じ仕組み）。
     DMA の buffer は物理連続・page 境界・coherent（HDA は snoop を強制済み）なので、HAL を変えずにできる見込み。
     interface（§6 の表）は 1 と 2 で同じにし、audiod・client は変えずに済むようにする。
- 鳴り終わった fragment を無音で埋める今の underrun の守りは、mmap でも続ける（書き手が遅れたとき古い音を繰り返さない）。
- 後で libc に `<sys/soundcard.h>`（OSS の名前）を足し、VLC 等の OSS の client が使えるようにする（p018 の範囲）。

## 7. libpulse（p019）との対応

| libpulse | audiod |
| --- | --- |
| `pa_context_connect` | 接続、`HELLO` |
| `pa_stream_connect_playback` / `_record` | `STREAM_CREATE`、fd を mmap |
| `pa_stream_begin_write` | ring の空きの先頭を指す pointer を返す（**client 側のゼロコピー**） |
| `pa_stream_write` | ring へ copy し、`write_position` を進める（begin_write の pointer なら copy しない） |
| write callback | `REQUEST` の受信 |
| `pa_stream_peek` / `pa_stream_drop` | 録音の ring の読みの位置 |
| `pa_stream_cork` | `STREAM_STOP` / `STREAM_START` |
| `pa_stream_drain` / `flush` | `STREAM_DRAIN` / `STREAM_FLUSH` |
| `pa_stream_get_latency` / `get_time` | header の `write_position`・`played_position`・`played_time_ns` から計算 |
| `pa_stream_set_volume`（`pa_context_set_sink_input_volume`） | `STREAM_VOLUME` |
| sink の音量 | `DEVICE_VOLUME` |

## 8. 失敗の扱い

- client の切断: その client の stream を全部閉じ、共有メモリを unmap する。他の stream は続く。
- `/dev/dsp` が無い（HDA が無い機種）: audiod は起動するが、再生は無音で捨て、`WELCOME` で device 無しを知らせる。device が後から現れたら開く。
- device の error: 開き直す。開けなければ無音で続ける。

## 9. 自己レビュー（敵対的な見直し）

| 疑い | 検討 | 結論 |
| --- | --- | --- |
| client が ring の位置を壊す | 位置の矛盾は検出して stream を閉じる（§4.2） | 他の stream・audiod に波及しない |
| client が共有メモリを縮めて audiod を SIGBUS にする | handler で閉じる。後で seal（§4.3） | 当面の対策あり |
| `REQUEST` の通知が多すぎる | period ごとに最大 1 回、十分書かれていれば送らない | 毎秒 50 回程度 |
| audiod が遅れて device が underrun | device の ring は 8 fragment、audiod は 1 period 先を埋める。kernel は鳴り終わった所を無音にする | 途切れても雑音は出ない |
| 名前つきの shm が一瞬残る | `O_EXCL` と作成直後の unlink。名前に pid と counter | 他の process が開く窓はほぼ無い |
| float の標本を kernel が知らない | audiod の中で変換し、device へは device の format で出す | kernel は変えなくてよい（float を受けるのは audiod） |

## 10. 実装の順序

1. p049: `/dev/dsp` の mmap（コピーあり）。
2. p009: audiod（socket、共有メモリ、mix、mmap と write の両経路、録音）。試験 client は libpulse の前に小さな C の client を作る。
3. p019: libpulse の互換（§7）。
4. p056: `/dev/dsp` の mmap のゼロコピー（DMA の ring を直接 mmap）。
5. 後で: seal、resampler、`<sys/soundcard.h>`。
