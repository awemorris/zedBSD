<!-- awesome-plan project=zedbsd record=ws035-audio-design -->

# audioフレームワークと `/dev/dsp`（設計）

ws035-p021 の成果物。実装は p006（フレームワークと `/dev/dsp`・mixer）で行う。
hdaドライバの設計は p022、実装は p007。**この設計ではソースを変更しない。**

## 1. 前提

### 1.1 今のtreeには音が無い

`src`・`include` に audio・sound・dsp・hda のいずれの名前を持つファイルも無い。
`/dev` に出ている cdev は console、input、memory、system、loop、graphics、gpu だけである。
**ゼロから作る。**

### 1.2 ユーザーの決定（2026-09-23）

- FreeBSD の OSS 風、`/dev/dsp0`。
- **再生と録音の基本機能だけ**。転送（ネットワーク再生）は要らない。
- 互換 libpulse（p019）は `/sbin/audiod`（p009）の上に載せ、Chromium を動かすためのもの。
- 実機の HDA は VFIO で渡せない（IOMMU group 15 に eSPI・SMBus・SPI が同居）。
  **QEMU の intel-hda だけで開発する**。実機は人が確かめる。

### 1.3 使える土台

| 要るもの | 既存 |
| --- | --- |
| cdev | `include/kern/cdev.h` の `struct cdev_ops`（open/close/read/write/ioctl/poll/seek/mmap） |
| DMA | `include/drivers/generic/dma.h`。`drv_dma_alloc_coherent()` が coherent な連続領域を取り、`device_address` を返す |
| 待ち | `waitq`（`src/kern/waitq.c`）。`poll` は `struct cdev_ops` の `poll` |
| 登録の作法 | `drv_gpu_register(ops, private_data, &device)` / `drv_gpu_unregister(device)`。GPU で固めた形 |

## 2. 目標と非目標

### 目標

1. 機種によらない audio フレームワークを1つ置き、ハードウェアの仕事を driver ops に閉じる。
2. `/dev/dsp0` を OSS 風の cdev として公開し、`read()` で録音、`write()` で再生できるようにする。
3. `/dev/mixer0` で音量とミュートを扱う。
4. DMA リングの扱い（位置の報告、underrun・overrun、割り込み周期）をフレームワークが持つ。

### 非目標

- ネットワーク再生・転送（ユーザーの明示）。
- ミックス（複数プロセスの同時再生）は **audiod（p009）の仕事**。kernel は1つの開きだけを受ける（§5.3）。
- MIDI、`/dev/sequencer`、`/dev/audio`（Sun 形式）。
- 実機 HDA の受入（p008、人が行う）。
- サンプル形式の変換。kernel は hardware が受ける形式だけを受ける（§4.3）。

## 3. UAPI

`include/uapi/audio.h`（新規）。OSS の `soundcard.h` に**名前と意味を合わせる**が、
Linux/FreeBSD のヘッダは取り込まず、必要なものだけを独自に宣言する。
ioctl 番号も独自（グループ `'A'`）にする。**OSS のバイナリ互換は狙わない**。
互換が要る相手は互換 libpulse（p019）であって、OSS を直接叩く既存バイナリではない。

```c
#define KERN_AUDIO_IOC_GROUP	'A'

/* 形式。hardware が受けるものだけを並べる。 */
#define KERN_AUDIO_FORMAT_S16_LE	1U
#define KERN_AUDIO_FORMAT_S32_LE	2U

struct audio_format {
	uint32_t format;        /* KERN_AUDIO_FORMAT_* */
	uint32_t channels;      /* 1 または 2 */
	uint32_t rate;          /* Hz */
	uint32_t reserved;
};

/* リングの形。フレームワークが決め、アプリは読むだけ。 */
struct audio_buffer_info {
	uint32_t fragment_bytes;   /* 割り込み1回で進む量 */
	uint32_t fragment_count;   /* リング全体の fragment 数 */
	uint32_t bytes_per_frame;
	uint32_t reserved;
};

/* 今どこまで進んだか。OSS の audio_buf_info に相当する。 */
struct audio_space {
	uint32_t fragments;     /* すぐ書ける（読める）fragment 数 */
	uint32_t fragment_bytes;
	uint32_t bytes;         /* すぐ書ける（読める）byte 数 */
	uint64_t transferred;   /* この開きで hardware が運んだ総 byte 数 */
	uint32_t underruns;     /* 再生で間に合わなかった回数 */
	uint32_t overruns;      /* 録音で捨てた回数 */
};

#define KERN_AUDIO_GET_FORMAT	_IOR(KERN_AUDIO_IOC_GROUP, 1, struct audio_format)
#define KERN_AUDIO_SET_FORMAT	_IOWR(KERN_AUDIO_IOC_GROUP, 2, struct audio_format)
#define KERN_AUDIO_GET_BUFFER	_IOR(KERN_AUDIO_IOC_GROUP, 3, struct audio_buffer_info)
#define KERN_AUDIO_GET_OSPACE	_IOR(KERN_AUDIO_IOC_GROUP, 4, struct audio_space)
#define KERN_AUDIO_GET_ISPACE	_IOR(KERN_AUDIO_IOC_GROUP, 5, struct audio_space)
#define KERN_AUDIO_DRAIN	_IO(KERN_AUDIO_IOC_GROUP, 6)
#define KERN_AUDIO_FLUSH	_IO(KERN_AUDIO_IOC_GROUP, 7)
/*
 * 今この瞬間に write() したデータが鳴り始めるまでに hardware が運ぶべき byte 数、
 * すなわち「書き込み位置 - 再生位置」。リングに溜まっている未再生の量である。
 * hardware の内部 FIFO や codec の遅延は含まない（kernel は知らない）。
 */
#define KERN_AUDIO_GET_DELAY	_IOR(KERN_AUDIO_IOC_GROUP, 8, uint32_t)

/* mixer。0..100 の百分率で、hardware の目盛には触らせない。 */
struct audio_volume {
	uint32_t left;
	uint32_t right;
	uint32_t muted;
	uint32_t reserved;
};

#define KERN_AUDIO_GET_VOLUME	_IOR(KERN_AUDIO_IOC_GROUP, 16, struct audio_volume)
#define KERN_AUDIO_SET_VOLUME	_IOW(KERN_AUDIO_IOC_GROUP, 17, struct audio_volume)
```

**`GET_DELAY` を入れた理由**: 互換 libpulse は「今書いた音がいつ鳴るか」を返さねばならない
（`pa_stream_get_latency`）。これが無いと Chromium の音声と映像が合わない。
kernel が hardware の位置を知っているので、遅延を byte 数で返すのが一番正確である。
p019 がこれを時間へ直す。**範囲は上のコメントのとおり**で、hardware 内部の遅延は含まない（R4）。

## 4. フレームワーク

### 4.1 置き場所

| ファイル | 内容 |
| --- | --- |
| `src/drivers/generic/audio.c` | フレームワーク。cdev、リング、format、mixer、driver の登録 |
| `include/kern/audio.h` | `struct drv_audio_ops` と登録・解除、割り込みからの通知 |
| `include/uapi/audio.h` | 上の UAPI |

`src/drivers/generic/` は機種によらないドライバの置き場で、graphics の共通層（p005）も同じ場所に入る。

### 4.2 driver ops

```c
struct drv_audio_ops {
	/* この方向を hardware が持つか。0 なら cdev はその方向を拒む。 */
	unsigned playback;
	unsigned capture;

	/* 受ける形式。フレームワークはこの範囲でしか SET_FORMAT を通さない。 */
	const struct audio_format *formats;
	unsigned format_count;

	/*
	 * リングを hardware へ結び付ける。buffer はフレームワークが
	 * drv_dma_alloc_coherent() で取った 1 つの連続領域で、fragment_bytes ×
	 * fragment_count の大きさがある。hardware はこれを巡回して読む（書く）。
	 */
	int (*prepare)(void *private_data, int capture,
		       const struct audio_format *format,
		       const struct drv_dma_buffer *buffer,
		       uint32_t fragment_bytes, uint32_t fragment_count);

	/* 動かす・止める。stop は hardware が触るのをやめるまで戻らない。 */
	int (*start)(void *private_data, int capture);
	void (*stop)(void *private_data, int capture);

	/*
	 * hardware が今どこを読んで（書いて）いるかを、リングの先頭からの
	 * byte 数で返す。フレームワークはこれで空きを数える。
	 */
	uint32_t (*position)(void *private_data, int capture);

	/* mixer。無ければ NULL で、SET_VOLUME は ENOTSUP になる（§4.5）。 */
	int (*get_volume)(void *private_data, struct audio_volume *volume);
	int (*set_volume)(void *private_data, const struct audio_volume *volume);

	/* DMA の制約。フレームワークがリングを取るときに使う。 */
	struct drv_dma_constraints constraints;
};

int drv_audio_register(const struct drv_audio_ops *ops, void *private_data,
		       struct drv_audio_device **device);
int drv_audio_unregister(struct drv_audio_device *device);

/* 割り込みから呼ぶ。fragment が 1 つ進んだことを知らせる。 */
void drv_audio_interrupt(struct drv_audio_device *device, int capture);
```

`drv_gpu_register` と同じ形（`private_data` を取り、不透明な handle を返し、解除が handle を消費する）
にする。**GPU で確立した形を踏襲する**ので、新しい流儀を作らない。

### 4.3 形式の交渉

`SET_FORMAT` は `ops->formats` に**完全一致するもの**だけを通す。近いものへ丸めない。
変換はしない（§2 の非目標）。合わないときは `EINVAL` を返し、`GET_FORMAT` で
今の設定を返す。アプリ（audiod）が一覧を見て選ぶのが筋で、kernel がリサンプラを持つと
そこが音質と遅延の責任を負うことになる。

QEMU の intel-hda が受けるのは 48000 Hz / S16_LE / 2ch が中心なので、p007 はそこから始める。

### 4.4 リングと空きの数え方

- リングは `drv_dma_alloc_coherent()` で取る 1 つの連続領域。
  大きさは `fragment_bytes × fragment_count`。既定は 4096 byte × 8（48 kHz・S16・2ch で約 170 ms）。
- 再生: アプリの `write()` が「書き込み位置」を進め、hardware が `position()` で追いかける。
  空き = リング長 − (書き込み位置 − 再生位置)。
- 録音: hardware が書き、アプリの `read()` が追いかける。
- **位置は単調増加の 64 bit で持つ。** リング内 offset を前回と比べて巻き戻りを数える方法は
  使わない（R1）。アプリが 1 周ぶん以上止まると巻き戻りは 1 回しか見えず、総量が足りなくなる。
  録音では、1 周ちょうど進んだ場合に巻き戻りが見えず、**上書き済みの領域を読む**。
- **割り込みの回数が一次の情報である。** `drv_audio_interrupt()` は fragment ごとに来るので、
  呼ばれた回数 × `fragment_bytes` が hardware の進んだ量の下限になる。
  `position()` はその中の端数を埋めるためだけに使い、総量がこの下限を下回らないようにする。
  割り込みを取りこぼした場合に備え、`position()` の offset が割り込み由来の総量から見て
  1 fragment 以上先なら、その差も足す。

### 4.4.1 lock と copy の順序（R2・R3）

位置・計数・世代を守るのは **1 つの spinlock**（`device->lock`）である。
`waitq_sleep()` が条件 lock と世代を突き合わせるので、条件の更新と世代の前進は
この lock の下で行う。割り込み文脈から取れるよう、**この lock の下では copy も割り当ても行わない**。

- `read()`/`write()` は lock を取って「どの範囲を copy してよいか」を決め、
  **lock を落としてから copy し**、また取って位置を進める。
- 再生では、copy 先は hardware がまだ読んでいない範囲なので、lock を落としていても安全である。
- **録音では安全でない**（R3）。copy の最中に hardware が 1 周してその範囲を上書きしうる。
  copy し終えてから lock を取り直し、その間に hardware が copy 対象の範囲へ入っていたら、
  **その `read()` を捨てて `overruns` を 1 増やし、読み直させる**（copy したデータは返さない）。
  壊れた音を返すより、落としたと言う方がよい。
- `drv_audio_interrupt()` は lock を取り、位置と計数を更新し、`waitq_wake_all()` を呼んで落とす。

### 4.5 音量

`ops->set_volume` があれば hardware に任せる。**無ければ `SET_VOLUME` を `ENOTSUP` で断る**（R5）。
`GET_VOLUME` は「調整なし」を表す left=100・right=100・muted=0 を返す。

フレームワークが書き込み時に減衰させる案は採らない。リングには最大 170 ms ぶん溜まっているので、
**音量を下げても 170 ms は大きいまま鳴る**。つまみとして使えないうえ、
audiod（p009）が自分のミックスの段で音量を扱うと二重に減衰する。
audiod ならまだ書いていないデータに効く。QEMU の intel-hda には codec の音量があるので、
p007 は `set_volume` を実装する。

## 5. `/dev/dsp0` の振る舞い

### 5.1 open

- `O_WRONLY` は再生、`O_RDONLY` は録音、`O_RDWR` は両方。
  hardware が持たない方向を求めたら `ENODEV`。
- **開けるのは1つだけ**。2つ目は `EBUSY`（§5.3）。
- open の時点ではリングを取らない。最初の `SET_FORMAT` か最初の `write()`/`read()` で
  既定の形式を使って `prepare()` する。

### 5.2 read / write

- `write()` はリングの空きへ copy し、書けた byte 数を返す。空きが無ければ、
  非ブロックなら `EAGAIN`、ブロックなら空くまで待つ。
- `start()` の条件は、**リングの半分が埋まった**、または **`DRAIN` が呼ばれた**、
  または **`close()` された**、のいずれかである（R6）。半分だけを条件にすると、
  リングの半分（約 85 ms）に満たない短い音を書いて閉じたときに**何も鳴らない**。
  書かれた量が 1 fragment に満たない場合は、末尾を無音で 1 fragment ぶんまで埋めてから動かす。
  hardware は fragment 単位でしか進められないためである。
  1 fragment で動かし始めると、すぐ underrun する。
- `read()` は hardware が書いた分を copy する。
- どちらも `bytes_per_frame` の倍数でなければ `EINVAL`。半端なフレームを許すと、
  次の書き込みでチャネルがずれる。

### 5.3 1つの開きだけを受ける理由

複数のプロセスを混ぜるのは **audiod（p009）の仕事**である。kernel に mixer を置くと、
音量・優先度・遅延の方針を kernel が持つことになり、それは方針であって機構ではない。
`/dev/dsp0` を開くのは audiod だけ、アプリは互換 libpulse で audiod と話す、という段にする。

ただし **audiod が動いていないときに `/dev/dsp0` を直接開けること**は残す。
試験と、音を出す最小の確認がそれでできる。

**`EBUSY` は close で必ず解ける**（R7）。`close()` は `stop()` が hardware の停止を
確かめるまで戻らない（`struct drv_audio_ops` の `stop` の契約）。
加えて `DRAIN` 待ちの `close()` には**上限 2 秒**を置き、超えたら残りを捨てて止める。
そうしないと hardware が壊れたときに close が返らず、fd が永久に解放されず、
audiod を再起動しても `EBUSY` のままになる。

### 5.4 poll

`POLLOUT` は再生の空きが 1 fragment 以上あるとき、`POLLIN` は録音が 1 fragment 以上たまったとき。
`drv_audio_interrupt()` が waitq を起こす。

### 5.5 DRAIN と FLUSH

- `DRAIN`: 書いた分が全部鳴るまで待ってから `stop()`。`close()` も同じことをする。
- `FLUSH`: 書いた分を捨てて即 `stop()`。

### 5.6 underrun

再生位置が書き込み位置に追いついたら、フレームワークは**無音を流し続ける**（`stop()` しない）。
`underruns` を 1 増やし、`GET_OSPACE` で見えるようにする。止めてしまうと、
次の `write()` で再び `start()` が要り、そこで数十 ms の隙間ができる。

## 6. mixer

`/dev/mixer0` は `GET_VOLUME` / `SET_VOLUME` だけの小さな cdev。
`/dev/dsp0` が使用中でも開ける（音量は再生とは別の話である）。
複数の開きを許す。

## 7. p006 と p022・p007 への分け方

### p006（フレームワークと `/dev/dsp`・mixer）

`src/drivers/generic/audio.c`、`include/kern/audio.h`、`include/uapi/audio.h`。
**hardware は無いので、host fixture の偽 driver で確かめる。**

受け入れ:

- host fixture: 偽 driver（`position()` を試験が進める）に対して、
  リングの空きの計算（1 周をまたぐ場合を含む）、underrun・overrun の計数、
  形式の一致判定、`bytes_per_frame` の倍数でない要求の拒否、2 つ目の open の `EBUSY`、
  `DRAIN`・`FLUSH`、poll の起床、音量の減衰（`set_volume` の無い driver）。
- amd64・pcat・pc98 の kernel が warning 0。audio を外した build で symbol が消えること。
- `/dev/dsp0` が出ないこと（driver が無いので登録されない）を確かめる。

### p022（hda の設計）→ p007（hda の実装）

QEMU の intel-hda に対して、codec 列挙、stream の DMA（BDL）、再生・録音を作る。
この設計の `struct drv_audio_ops` を満たす。

### p009（audiod）→ p019（互換 libpulse）

audiod が `/dev/dsp0` を1つ開き、複数のクライアントを混ぜる。
`GET_DELAY` を `pa_stream_get_latency` へ直す。

## 8. 制限（設計時点）

1. **形式の変換をしない。** hardware が 48 kHz しか受けなければ、44.1 kHz の音源は
   audiod が直す。kernel は直さない。
2. **1つの開きだけ。** 混ぜるのは audiod。
3. 実機の HDA は VFIO で渡せない（IOMMU group 15 に eSPI・SMBus・SPI が同居）。
   QEMU だけで開発し、実機は人が確かめる（p008）。
4. `/dev/audio`（Sun 形式、µ-law）と MIDI は作らない。
5. **`set_volume` を持たない driver では音量を変えられない**（`ENOTSUP`）。
   その場合の音量は audiod が扱う。録音側の音量調整はしない。
6. **録音は overrun のときに、読みかけの内容を返さず捨てる**（R3）。
   壊れた音より欠落を選ぶ。
7. 遅延は byte 数で返す。時刻の同期（PTP のような）は扱わない。
8. リングの大きさは固定（4096×8）。OSS の `SNDCTL_DSP_SETFRAGMENT` に相当する
   調整は入れない。必要なら p019 の実測で決め直す。
