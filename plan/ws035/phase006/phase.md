<!-- awesome-plan project=zedbsd record=ws035p006 -->

# ws035-p006: audio フレームワークと `/dev/dsp`・mixer

Phase ID: `ws035-p006`
Parent: [WS035](../ws.md)
Status: **cleared**（q340-i01、2026-09-24）
Phase disposition: normal
Queue: q340（q340-i01）
実行: メインセッション
設計: [audio-design.md](../audio-design.md)（p021）

## 作ったもの

| ファイル | 内容 |
| --- | --- |
| `include/uapi/audio.h` | UAPI。group `'A'`。形式・リング・空き・遅延・音量の要求 |
| `include/drivers/audio/audio.h` | `struct drv_audio_ops`、`drv_audio_register`・`drv_audio_unregister`・`drv_audio_interrupt` |
| `src/drivers/audio/audio.c` | フレームワーク。`/dev/dspN`（dev_t `0x000A0000 + 2N`）と `/dev/mixerN`（`+ 2N + 1`） |
| `Makefile` | `KERN_AUDIO_BACKENDS`。backend が1つでも `y` のときだけ `audio.c` を build する（GPU と同じ形）。今は backend が無いので空で、p007 が hda を足す |
| `platform/{amd64,pcat,pc98}/vmunix.mk` | `KERN_AUDIO_SOURCES`/`OBJS` を link に入れる。pc98 は audio を選んだときだけ `drivers/generic/dma.o` も入れる（pc98 は DMA 層を link していなかった） |

## 設計からの差分（理由つき）

1. **置き場所**: 設計は `src/drivers/generic/audio.c` と `include/kern/audio.h` だったが、p002 の refactor（include/drivers を
   src/drivers の階層に揃える）の後なので、GPU（`src/drivers/gpu/gpu.c`・`include/drivers/gpu/gpu.h`）と同じ形の
   `src/drivers/audio/audio.c`・`include/drivers/audio/audio.h` にした。
2. **`struct audio_space` に `reserved` を足した**: 設計のままだと `uint64_t transferred` の offset が i386 で 12、amd64 で 16 になり、
   32 bit と 64 bit のプロセスで layout が違う。`reserved` を挟んで両方 16（全体 32 byte）にした。
3. **無音の流し方**: 設計は「underrun では無音を流し続ける」とだけ書いていた。割り込みハンドラが、hardware が読み終えた範囲を
   lock の外で 0 に埋め、埋め終えた位置（`released`）を公開する。書き手の空きは `released` までに制限するので、
   消去と書き込みは重ならない。書き手が遅れても hardware は古い音ではなく無音を読む。
4. **prepare の時期**: 設計は「最初の SET_FORMAT か read/write で prepare」だったが、**毎回の start の直前**に prepare する。
   止めた後の再開（DRAIN・FLUSH の後）でも hardware を結び直すため。`SET_FORMAT` は形式を覚えるだけ。
5. **`SET_FORMAT` は動作中・未再生データありなら `EBUSY`**（設計に無い取り決め）。
6. **録音は最初の `read()` で始まる**。poll だけでは始まらない（poll は backend を呼べないため）。audiod は非ブロックの read を1回してから poll する。
7. **`DRAIN` の期限**: close は設計どおり 2 秒。`DRAIN` 要求は「残りデータの再生時間＋2 秒」で、切れたら止めて `EIO`。
   signal で中断したら `EINTR` を返し、**止めずに**鳴らし続ける。
8. 向きの違う要求（`O_WRONLY` の read、録音の無い開きの `GET_ISPACE` 等）は `EBADF`。音量の要求は `/dev/mixerN` だけで受ける。
9. 設計 §7 の受け入れに「音量の減衰（`set_volume` の無い driver）」とあるが、§4.5 と制限 5 は `ENOTSUP` を決めている。
   §4.5 に従い `ENOTSUP` を確かめた。
10. unregister は `/dev/dspN` が開いている間と mixer の要求の最中は `EBUSY`（GPU と同じ「成功で handle を消費、EBUSY なら再試行」）。

hardware の位置は設計 R1 のとおり単調増加の 64 bit で、割り込みの回数を下限にし、`position()` で端数を埋める。
`position()` が下限より1 fragment 未満だけ後ろなら「割り込みの直前の値」とみなし、1周先とは数えない。
割り込みを取りこぼしたときは `position()` の分を足す。

## 検証

| 検証 | 結果 |
| --- | --- |
| host fixture `plan/ws035/tests/run-audio-framework-test.sh`（通常＋ASan/UBSan、12 試験） | **PASS** |
| UAPI の layout（`-m32`・`-m64`、`audio-uapi-layout.c`） | PASS。4 構造体の大きさ、`transferred` の offset 16 |
| amd64・pcat・pc98 の `vmunix`（audio を選んだ構成 `plan/ws035/tests/config-*-audio.mk`） | PASS、warning 0（`-Werror`） |
| 同じ3機種の CI 構成（audio なし） | PASS。`audio.o` を build しない |
| `audio.o` の未定義 symbol が各機種の object と builtins で全部解決するか | 全部解決（amd64 27、pcat・pc98 30） |
| audio を選んだ kernel の `vmunix` の symbol | audio の symbol は 0。backend が無いので `--gc-sections` が全部落とす |
| GPU framework の host fixture（Makefile を触ったので回帰） | PASS |

fixture は本物の `audio.c` を stub と偽 driver に対して build する。偽 driver の「hardware」は再生リングを読んでログに取り、
録音リングに数える pattern を書き、fragment ごとに `drv_audio_interrupt()` を呼ぶ。眠った thread の代わりに hardware を
進める（別の CPU の割り込みに相当）。確かめたこと:

- 登録: 不完全な ops の拒否、`dsp0`/`mixer0`・`dsp1` の番号と dev_t、番号の再利用。**driver が無ければ node は無い**。
- リング: 空き（fragment 数も）、半分で start、`GET_DELAY`、fragment 途中の位置、リングの端をまたぐ書き込み、
  非ブロックの `EAGAIN`、ブロックする書き込みが hardware の進みで完了。**再生された byte 列が書いた pattern と一致**。
- underrun: 1回の途切れで1回だけ数える、無音が鳴る、stop しない、次の書き込みがすぐ鳴る、2回目も数える。
- 録音: 最初の read で start、`POLLIN`、1周遅れで overrun 1回と半リング後ろへの飛び、**copy 中に hardware が追い越した
  copy は返さずに読み直す**（R3）、ブロックする read、`EINTR`、`FLUSH` 後の再 start。
- 形式: 完全一致だけ、丸めない（44100 は `EINVAL` で今の形式が返る）、`reserved` の拒否、フレームの大きさ（S32 ステレオ 8、
  S16 モノ 2）で半端な長さを `EINVAL`、データがあれば `EBUSY`、開き直すと既定の形式。
- 開き: 2つ目は `EBUSY`、close で解ける、mixer は同時に開ける、向きの無い開きは `ENODEV`、unregister は開いている間 `EBUSY`、
  unregister 後の古い mixer は `ENODEV` で、その close で device が解放される。
- DRAIN・FLUSH: 1 fragment 未満の音を DRAIN が鳴らして無音で埋め止める、再 start、FLUSH で捨てる、`EINTR` で止めない、
  止まった hardware で DRAIN が期限後に `EIO`、**close は 2 秒で戻り、次の open ができる**。健全な close は書いた分を鳴らしきる。
- 位置: 割り込みの取りこぼし、割り込みより少し後ろの位置、単調、止めた stream への割り込み。
- poll: 空きで `POLLOUT`、割り込みが `poll_notify()` を呼ぶ。
- 音量: `set_volume` が無ければ `GET` は 100/100/0、`SET` は `ENOTSUP`。有れば値を渡す、101・muted 2 は `EINVAL`、
  読み取り専用の mixer は `EBADF`。
- start の失敗: 受けた byte 数を返し、リングが満ちたら眠らずに `EIO` を返す（自己レビューで見つけた眠りっぱなしを直した）。

fixture の stub は、backend の `prepare`/`start`/`stop` と `poll_notify()` が spinlock を持たずに呼ばれること、
`position()` が spinlock の下で呼ばれることを assert する。

## 確かめていないこと

- **実 hardware・ゲストでの動作**。backend が無いので、`/dev/dsp0` が出ないことはゲストで起動して確かめず、
  kernel に audio の code が1つも link されないこと（symbol 0）と fixture で確かめた。実動作は p007（QEMU intel-hda）で見る。
- 複数 CPU で割り込みと read/write が同時に走る場合。fixture は単一 thread で、割り込みは眠りの間か copy の間に入れた。
- rpi4・x68k・arm64・sparcv9 は link list に入れていない（backend が無い。必要になったら入れる）。

## 後の変更（p007）

- `drv_audio_register` は DMA device を第3引数で受ける（backend の `drv_pci_device_dma()`）。`constraints` は ops から除いた。
- DRAIN と close は最後の byte の後に無音の fragment を2つ流してから止める（QEMU の codec buffer で末尾が欠けたため）。
詳細は [p007](../phase007/phase.md)。
