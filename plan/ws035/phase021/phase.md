<!-- awesome-plan project=zedbsd record=ws035p021 -->

# ws035-p021: 設計 — audioフレームワークと `/dev/dsp`

Phase ID: `ws035-p021`
Parent: [WS035](../ws.md)
Status: cleared（q323-i05、2026-09-23。設計 [audio-design.md](../audio-design.md)、レビュー [review.md](review.md)）
Phase disposition: normal
Queue: q323（q323-i05）
実行: メインセッション（設計・敵対的レビューとも）

## 目的

OSS 互換寄りの audio フレームワークと `/dev/dsp0`・`/dev/mixer0` を設計する。
実装は p006、hda の設計は p022、実装は p007。**この Phase ではソースを変更しない。**

## 調べた事実

- **treeに音は無い**。audio・sound・dsp・hda の名前を持つファイルは `src`・`include` に一つも無い。
  ゼロから作る。
- 使える土台: `struct cdev_ops`（open/close/read/write/ioctl/poll/mmap）、
  `drv_dma_alloc_coherent()`（coherent な連続領域と `device_address`）、
  `waitq_sleep()`（条件 spinlock と世代を突き合わせる）、
  `drv_gpu_register()` の登録の作法。
- 実機の HDA（`8086:51c8`）は **IOMMU group 15 に eSPI・SMBus・SPI と同居**しており、
  単独で VFIO へ渡せない（ws035-p001 の調査）。QEMU の intel-hda だけで開発する。

## 決めたこと

1. UAPI は `include/uapi/audio.h`。OSS に**名前と意味を合わせる**が、ヘッダは取り込まず、
   ioctl 番号も独自（グループ `'A'`）。**OSS のバイナリ互換は狙わない**。
   互換が要る相手は互換 libpulse（p019）である。
2. `GET_DELAY` を入れる。互換 libpulse の `pa_stream_get_latency` に要る。
   定義は「書き込み位置 − 再生位置」で、hardware 内部の遅延は含まない。
3. フレームワークは `src/drivers/generic/audio.c`、driver ops は `struct drv_audio_ops`。
   `drv_gpu_ops` と同じ形にする。
4. 形式の変換をしない。`SET_FORMAT` は driver が挙げた形式に**完全一致**するものだけ通す。
5. **`/dev/dsp0` は1つの開きだけを受ける。** 混ぜるのは audiod（p009）の仕事。
   kernel に mixer を置くと、音量・優先度・遅延の方針を kernel が持つことになる。
6. リングは 4096 byte × 8 fragment（48 kHz・S16・2ch で約 170 ms）。
7. underrun では止めず、無音を流して計数する。止めると再開に数十 ms の隙間ができる。

## 成果物

- [`plan/ws035/audio-design.md`](../audio-design.md): 設計（前提、UAPI、driver ops、
  リングと lock、cdev の振る舞い、p006・p022・p007 への分け方、制限8件）。
- [`review.md`](review.md): 敵対的レビュー。**7件を指摘し、6件を設計へ反映、1件は設計を変えて解消**。

レビューで見つかった主なもの:

- **R1**: `position()` の巻き戻りだけで総量を数えると、アプリが1周ぶん止まったときに
  空きを誤る。録音では**上書き済みの領域を読む**。→ 割り込み回数を一次の情報にした。
- **R2/R3**: 割り込みから waitq を起こす lock と、copy 中に lock を落とす段取りが未定義。
  再生は安全だが**録音は上書きされうる**。→ lock の範囲を決め、録音は copy 後に
  再確認して、踏まれていたら捨てて overrun にする規則にした。
- **R5**: `set_volume` の無い driver でフレームワークが書き込み時に減衰させると、
  **音量を下げても 170 ms は大きいまま鳴る**。→ `ENOTSUP` で断り、audiod に任せる。
- **R6**: 「半分埋まったら start」だけだと、**短い音が鳴らない**。
  → `DRAIN`・`close()` でも start する。
- **R7**: `EBUSY` が解けないと audiod の再起動が失敗する。
  → close は stop の完了まで戻らず、DRAIN 待ちに上限2秒を置く。

## 受け入れ

- 設計に、UAPI・driver ops・リングと lock の規則・cdev の振る舞い・p006 の受け入れ条件・制限がある。
- 敵対的レビューを行い、指摘と対応を残した。
- ソースを変更していない。
