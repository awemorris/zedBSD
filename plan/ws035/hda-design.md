<!-- awesome-plan project=zedbsd record=ws035-hda-design -->

# hda ドライバ（設計）

ws035-p022 の成果物。実装は p007（QEMU intel-hda）、実機確認は p008（人が行う）。
audio フレームワーク（p006、[audio-design.md](audio-design.md)）の backend である。
**この設計ではソースを変更しない。** p006 の interface の変更（§3）も p007 で行う。

## 1. 前提

### 1.1 使える土台

| 要るもの | 既存 |
| --- | --- |
| audio フレームワーク | `include/drivers/audio/audio.h`（p006）。`struct drv_audio_ops`、`drv_audio_register`、`drv_audio_interrupt` |
| PCI | `include/drivers/pci/pci.h`。`claim_bar`→`map_bar`、`allocate_irqs`（MSI→INTx）→`establish_irq`、`drv_pci_device_dma()`、`set_service`（publish/unpublish） |
| DMA | `drv_dma_alloc_coherent(dma, size, alignment, &buffer)`。`device_address` が bus address |
| 登録の作法 | i915・venus と同じ。`src/drivers/pci/pci-hda.c`（1ファイル）、`include/drivers/pci/pci-hda.h`、`drv_pci_hda_driver_register()`、`CONFIG_DRIVER_PCI_HDA`、`src/kern/platform/pcat.c` から登録 |

### 1.2 QEMU

host の QEMU 10.0.11 に `intel-hda`（ICH6、8086:2668）・`ich9-intel-hda`（8086:293e）と codec の `hda-output`・`hda-duplex`・
`hda-micro` がある。codec の `mixer=on`（既定）は amp を持ち、音量を QEMU の mixer で掛ける。`mixer=off` は amp を持たない。
audiodev は `wav`（出力だけを WAV に書く）と `none`（入力は正しい速さの無音）を使う。

### 1.3 実機

Latitude 5320（Tiger Lake PCH-LP）の HDA は IOMMU group に eSPI・SMBus・SPI が同居し、**VFIO で渡せない**（p021 §1.2）。
p008 は実機を zedBSD で直接起動し、人が音を聞いて確かめる。エージェントは image と手順を用意する。

## 2. 目標と非目標

### 目標

1. PCI class 0x0403（HD Audio）の controller を1つにつき1つの audio device として登録する（`/dev/dspN`・`/dev/mixerN`）。
2. codec を列挙し、**出力1本と入力1本**の経路を見つけて設定する。
3. 再生と録音の stream を BDL で動かし、fragment ごとに割り込みでフレームワークへ知らせる。
4. 経路上の amp で音量とミュートを扱う（amp が無ければ `set_volume` を持たない）。

### 非目標

- HDMI・DisplayPort の音声（Intel の display codec）。**analog codec だけ**を使う。
- jack の抜き差し検出（unsolicited response）と、speaker と headphone の自動切替え。
- 複数の出力・入力の同時使用、5.1ch、S/PDIF、24 bit の変換。
- DSP（Intel SST・SOF）。BIOS が HDA を DSP 経由にしている機械では動かない（§8）。
- 電源管理（D3、clock gating）。

## 3. p006 の interface の変更

**DMA device は backend が渡す。** p006 は `struct drv_audio_ops` の `constraints` からフレームワークが
`drv_dma_device_create()` で DMA device を作るが、PCI driver は `drv_pci_device_dma(device)` を使う
（xHCI・NVMe・venus・i915 と同じ）。PCI の DMA device は host bridge の address 変換と制約を知っており、
フレームワークが別に作ったものではリングの bus address が正しい保証が無い。

- `drv_audio_register(ops, private_data, dma, &device)` と第3引数に `struct drv_dma_device *` を足す。
  `constraints` は `struct drv_audio_ops` から除く。
- フレームワークは渡された DMA device を**借りる**だけで、作りも壊しもしない（unregister 後に backend が捨てる）。
- 形式の一覧 `formats` は codec の能力で決まるので、backend は **instance ごとに `struct drv_audio_ops` を持つ**
  （p006 は ops を借りるだけなので、変更は要らない）。

p006 の fixture（`plan/ws035/tests/audio-framework.c`）もこれに合わせて直す。

## 4. controller

### 4.1 attach

1. BAR0（memory、16 KiB 以上）を `claim_bar`→`map_bar`（`DRV_PCI_MAP_READ|WRITE|NOCACHE`）。memory decode と bus master を有効にする。
2. **Intel の PCH では snoop を強制する**（p008 だけに効く）。vendor 8086 で、config の `DEVC`（0x78）の bit 11（`NSNPEN`、
   no snoop enable）を 0 にする。coherent な DMA は cache 可能な memory なので、snoop しないと controller が古い値を読む。
   QEMU はこれを見ない。**この bit の意味は Intel の PCH datasheet による**。chip ごとの違いは p008 で確かめる。
3. controller の reset: `GCTL.CRST` を 0 にして 0 を読むまで待ち（上限 100 ms）、1 にして 1 を読むまで待つ（上限 100 ms）。
   その後 1 ms 待つ（codec が自分を知らせるのに spec は 521 µs を求める）。
4. `GCAP` を読む: 入力 stream 数 `ISS`（bit 11:8）、出力 stream 数 `OSS`（bit 15:12）、64 bit 可 `64OK`（bit 0）。
   ISS と OSS がともに 0 なら `ENODEV`。
5. CORB と RIRB を作る（§4.2）。
6. `STATESTS`（0x0E）で codec のいる address を知る。1 を書いて消す。
7. codec を列挙し経路を決める（§5）。使える codec が無ければ `ENODEV`。
8. 割り込み: `allocate_irqs(DRV_PCI_IRQ_ALLOW_MSI | DRV_PCI_IRQ_ALLOW_INTX, 1, 1)`→`establish_irq`。
   `INTCTL` は `GIE`（bit 31）と使う2本の stream の bit を立てる。controller の割り込み（`CIE`、CORB/RIRB）は使わない。
9. stream descriptor を1本ずつ reset して止めておく（§6.1）。
10. `set_service` で publish/unpublish を置く。publish が `drv_audio_register`、unpublish が `drv_audio_unregister`
    （`EBUSY` なら PCI の detach は失敗し、再試行できる）。

### 4.2 CORB と RIRB（codec への命令）

- CORB は 256 entry（4 byte）、RIRB は 256 entry（8 byte）。`CORBSIZE`・`RIRBSIZE` の能力 bit を見て 256 が無ければ 16、2 の順に選ぶ。
  1つの 4 KiB の coherent 領域に両方を置く（CORB が先頭 1 KiB、RIRB が 2 KiB 目から。どちらも 128 byte 境界）。
- CORB: `CORBRUN` を 0 にして止まるのを待ち、base を書き、`CORBRP` の reset bit（15）を 1 にして 1 を読み、0 に戻して 0 を読む
  （QEMU と一部の chip は 1 を読み返さない。上限つきで待ち、読めなくても先へ進む）。`CORBWP` を 0 にして `CORBRUN` を 1。
- RIRB: `RIRBWP` の reset bit（15）を 1、`RINTCNT` を 1、`RIRBDMAEN` を 1。**RIRB の割り込みは使わない**。
- 1つの命令: mutex を取り、CORB の次の entry に書いて `CORBWP` を進め、`RIRBWP` がその分だけ進むのを**poll で待つ**
  （上限 10 ms。thread 文脈でだけ使う）。応答の bit 36（unsolicited）が立っているものは読み飛ばす。
  上限を過ぎたら `ETIMEDOUT` を返し、その codec を使わない。
- 命令の形: `codec << 28 | nid << 20 | verb << 8 | payload`（12 bit verb）、または `codec << 28 | nid << 20 | verb << 16 | payload`（4 bit verb）。

使う verb:

| verb | 用途 |
| --- | --- |
| `F00` GET_PARAMETER | vendor id（0x00）、node 数（0x04）、function 種別（0x05）、widget 能力（0x09）、PCM 能力（0x0A）、stream 形式（0x0B）、pin 能力（0x0C）、入力 amp 能力（0x0D）、接続数（0x0E）、出力 amp 能力（0x12） |
| `F02` GET_CONNECTION_LIST | 接続先 |
| `701` SET_CONNECTION_SELECT | selector・pin の入力選択 |
| `705` SET_POWER_STATE | D0 |
| `706` SET_CHANNEL_STREAM_ID | converter に stream tag を結ぶ |
| `2` SET_CONVERTER_FORMAT | converter の形式（stream の `FMT` と同じ値） |
| `3` SET_AMP_GAIN_MUTE / `B` GET | 音量とミュート |
| `707` SET_PIN_WIDGET_CONTROL | pin の出力・入力の有効化 |
| `70C` SET_EAPD_BTL | 外付け amp の有効化（内蔵 speaker） |
| `F1C` GET_CONFIG_DEFAULT | pin の接続（jack・内蔵・無し）と用途 |

### 4.3 割り込み

handler は `INTSTS` を読み、0 なら「自分のではない」と返す（INTx の共有）。stream の bit ごとに `SDnSTS` を読み、
`BCIS`（bit 2）・`FIFOE`（bit 3）・`DESE`（bit 4）を 1 を書いて消す。`BCIS` なら `drv_audio_interrupt(device, capture)`。
`FIFOE`・`DESE` は数えて一度だけ log に出す（stream は止めない。フレームワークは underrun として見る）。

### 4.4 detach

`drv_audio_unregister` が済んだ後（publish の逆）: 全 stream の `RUN` を 0 にして止まるのを待ち、`INTCTL` を 0、
割り込みを外し、CORB/RIRB を止め、`GCTL.CRST` を 0、DMA 領域を返し、BAR を unmap。

## 5. codec と経路

### 5.1 codec の選び方

`STATESTS` の bit の立つ address を順に見る。各 codec で root（nid 0）の node 数から function group を列挙し、
種別が audio（1）のものを使う。**vendor id の上位 16 bit が 0x8086 の codec（Intel の HDMI・DP）は使わない**。
出力経路が見つかった最初の codec を使う。入力経路は同じ codec で探し、無ければ再生だけの device にする
（`capture = 0`）。出力も入力も無ければ次の codec。

function group と経路上の widget に `SET_POWER_STATE` D0 を送る。

### 5.2 経路の探し方

widget の種別（能力の bit 23:20）: 0 出力 converter（DAC）、1 入力 converter（ADC）、2 mixer、3 selector、4 pin。

- **出力**: pin（種別 4）のうち、pin 能力の出力（bit 4）があり、config default の接続（bit 31:30）が「無し」（01）でないもの。
  優先は「内蔵」（10）→「jack」（00）の順にし、同じなら nid の小さい順。その pin から接続一覧を深さ優先でたどり、
  DAC に着く道を探す（深さ 5 まで。mixer・selector を通る）。見つかった道の selector・pin には `SET_CONNECTION_SELECT` で
  その道の入力を選ぶ。**同じ DAC に着く他の出力 pin も有効にする**（内蔵 speaker と headphone jack が同じ DAC を使う機械で、
  どちらからも鳴るように。jack 検出はしない）。pin は `SET_PIN_WIDGET_CONTROL` で出力（bit 6）、headphone 用なら bit 7 も立て、
  EAPD 能力（pin 能力 bit 16）があれば `SET_EAPD_BTL` の bit 1 を立てる。
- **入力**: ADC（種別 1）から接続一覧をたどり、pin 能力の入力（bit 5）がある pin に着く道を探す。優先は「jack の line-in・mic」
  →「内蔵 mic」。pin は入力（bit 5）を有効にする。
- 道の上の amp はすべて mute を外し、gain を 0 dB（能力の offset）にする。音量の対象（§5.3）だけは後から変える。

### 5.3 音量

出力の道で、**DAC に一番近い出力 amp で段数（出力 amp 能力の bit 14:8）が 1 以上のもの**を音量の対象にする。
0..100 を 0..段数へ直線で割り当て、`muted` は mute bit（能力の bit 31）があればそれを、無ければ gain 0 を使う。
左右は別々に書く。対象が無ければ `set_volume` を NULL にする（フレームワークが `ENOTSUP` を返す）。
`get_volume` は最後に書いた値を返す（起動時は 100・100・0 を書く）。

### 5.4 形式

DAC の PCM 能力（`F00` 0x0A、無ければ function group の値）から、次の候補のうち codec が受けるものだけを
instance の `formats` に並べる。先頭が既定の形式になる。

| 候補 | HDA の `FMT` |
| --- | --- |
| 48000 Hz、16 bit、2ch | `0x0011` |
| 44100 Hz、16 bit、2ch | `0x4011` |
| 48000 Hz、32 bit、2ch | `0x0041` |

入力の ADC が受けない形式は、capture の prepare で `EINVAL` にする（フレームワークの形式は再生と録音で共通なので、
両方が受けるものを先に並べる）。

## 6. stream

### 6.1 割り当てと reset

入力 stream 0（descriptor の番号 0）を録音、出力 stream 0（番号 `ISS`）を再生に使う。stream tag はどちらも 1
（tag は向きごとに別の空間）。descriptor は `0x80 + 0x20 × 番号`。

reset: `SDnCTL.SRST` を 1 にして 1 を読むまで待ち、0 にして 0 を読むまで待つ（それぞれ上限 10 ms）。

### 6.2 prepare

フレームワークのリング（`fragment_bytes × fragment_count`、1 つの連続領域）を BDL で覆う。

- BDL は driver が attach で取る 128 byte 境界の coherent 領域（16 entry × 16 byte）。stream ごとに1つ。
- entry i は `address = ring の device_address + i × fragment_bytes`、`length = fragment_bytes`、`IOC = 1`。
  fragment ごとに割り込みが来るのは、フレームワークの「割り込み1回で1 fragment」の約束のため。
- stream を reset し、`CBL` = リング長、`LVI` = fragment 数 − 1、`FMT` = 形式、`BDPL`/`BDPU` = BDL の address、
  `SDnCTL` の stream tag（bit 23:20）と `IOCE`・`FEIE`・`DEIE` を書く。
- codec の converter に `SET_CHANNEL_STREAM_ID`（tag << 4、channel 0）と `SET_CONVERTER_FORMAT`（`FMT` と同じ）を送る。
- **リングの address・長さは 128 byte の倍数**でなければならない。フレームワークのリングは 4 KiB 境界・4 KiB × 8 なので満たす。
  満たさなければ `EINVAL`。
- 64OK が 0 の controller で address が 4 GiB 以上なら `EINVAL`（PCI の DMA device の制約で起きないはず）。

### 6.3 start・stop・position

- start: `SDnCTL.RUN` を 1。
- stop: `RUN` を 0 にし、`RUN` が 0 を読むまで待つ（上限 10 ms）。`SDnSTS` を消す。**フレームワークは stop の後で
  hardware がリングに触らないことを前提にする**ので、待ちきれなかったら stream を reset してから戻る。
- position: `SDnLPIB`（リング先頭からの byte 数）。framework の spinlock の下で呼ばれるので MMIO を1回読むだけにする。
  LPIB が QEMU と Intel の chip で十分正確であることは p007・p008 で確かめる（DMA position buffer は使わない）。

## 7. 試験（p007）

### 7.1 host fixture

- 偽の MMIO（register の配列）と偽の codec（CORB に書かれた verb に RIRB で答える）で、controller reset・CORB/RIRB・
  codec 列挙・経路探索・BDL の中身・stream の register の値・割り込みの振り分けを確かめる。
- 経路探索は、QEMU の `hda-duplex`・`hda-output` と、**内蔵 speaker と headphone が同じ DAC を mixer 経由で使う**
  架空の codec（実機の典型）の3つの widget 表で確かめる。Intel HDMI codec が先に居る場合に飛ばすことも。

### 7.2 QEMU

| 構成 | 確かめること |
| --- | --- |
| `ich9-intel-hda` ＋ `hda-output,audiodev=w,mixer=off` ＋ `-audiodev wav,id=w,path=out.wav,out.frequency=48000,out.channels=2,out.format=s16` | 試験プログラムが書いた pattern が WAV の中に**そのまま**（bit 一致で）並ぶ。先頭と末尾の無音は許す |
| 同じで `mixer=on` | 音量 50 で WAV の振幅が小さくなる。mute で 0 |
| `ich9-intel-hda` ＋ `hda-duplex,audiodev=n` ＋ `-audiodev none,id=n` | 録音の read が止まらず、1 秒で 48000 frame ± 5% 届く（中身は無音） |
| `intel-hda`（ICH6） | 再生が同じく bit 一致（2つの controller で動くこと） |
| MSI を切る（`msi=off`） | INTx で同じ再生が通る |

試験プログラムは `plan/ws035/tests/` に置き、`ZEDBSD_EXTRA_FILES` でゲストへ入れる。WAV の照合は host の script で行う。

## 8. 制限（設計時点）

1. **HDMI・DP の音声は出ない。** Intel の display codec は使わない。
2. **jack の検出をしない。** 同じ DAC につながる出力 pin を全部有効にするので、headphone を挿しても speaker が鳴り続けうる。
3. **DSP 経由の機械では動かない。** BIOS 設定で HDA が Intel SST・SOF の DSP の後ろにある機械（class 0x0401 や、
   HDA の codec が見えない構成）では codec が見つからず `ENODEV` で attach しない。
4. 出力・入力は1本ずつ。stream 0 番どうしを使う。
5. **位置は LPIB。** chip によっては LPIB が実際の再生位置からずれる（Linux が DMA position buffer を併用する理由）。
   p008 で音飛びがあれば見直す。
6. snoop の強制は Intel の PCH の `DEVC` bit 11 だけ。AMD 等の controller の snoop 設定は扱わない。
7. 電源管理をしない。suspend・resume は `ENOTSUP`。

## 9. 敵対的レビュー（設計時点の自己レビュー）

設計を書いた後、誤り・欠落・矛盾を探した。見つけたものと扱い:

| # | 指摘 | 扱い |
| --- | --- | --- |
| H1 | p006 のフレームワークが自分で DMA device を作ると、PCI の address 変換が効かない | §3 で interface を変える（p007 で実装） |
| H2 | フレームワークは stop 後に hardware がリングに触らないことを前提にするが、`RUN` が 0 にならない chip では約束が破れる | §6.3: 待ちきれなければ stream reset してから戻る |
| H3 | QEMU の `wav` は入力を持たないので、`hda-duplex` で録音を試すと stream が進まない | §7.2: 録音は `none` の audiodev で試す（無音だが速さは正しい） |
| H4 | `mixer=on` の QEMU は音量を掛けるので bit 一致を見られない | §7.2: bit 一致は `mixer=off`、音量は `mixer=on` の別構成 |
| H5 | 形式の一覧はフレームワークでは再生・録音で共通だが、DAC と ADC の能力が違いうる | §5.4: 両方が受けるものを先に並べ、ADC が受けない形式は capture の prepare で `EINVAL` |
| H6 | `CORBRP` の reset bit を1と読み返さない chip で attach が止まる | §4.2: 上限つきで待ち、読めなくても進む |
| H7 | INTx を他の device と共有する場合、handler が自分のでない割り込みを取る | §4.3: `INTSTS` が 0 なら「自分のではない」 |
| H8 | 内蔵 speaker は EAPD を立てないと鳴らない機械がある | §5.2: EAPD 能力のある出力 pin で立てる |
| H9 | Intel PCH は snoop を切っていると coherent DMA が古い値を読む | §4.1 の 2。QEMU では確かめられないので p008 の確認項目 |
| H10 | `position()` は spinlock の下で呼ばれるので、CORB の命令（poll で待つ）を使えない | §6.3: LPIB の MMIO 読みだけ |
| H11 | 128 byte 境界の要求をフレームワークのリングが満たすか | §6.2: 4 KiB 境界・4 KiB の倍数なので満たす。満たさない値には `EINVAL` |
| H12 | 64 bit DMA が使えない controller | §6.2: 64OK が 0 で 4 GiB 以上なら `EINVAL` |
