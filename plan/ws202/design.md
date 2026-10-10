# WS202 の設計: libavcodec なしの H.264＋AAC の mp4 と .m4a の再生（ws202-p001）

**第 2 版**（2026-10-10、設計の担当）。第 1 版（同日）に design-reviewer の [review-001](review-001.md) の H-01〜H-07・M-01〜M-13・
L-01〜L-11・L-13 を織り込んだ（L-12 は Q1 が WS083 の側で直した）。指摘と直した節の対応は §16。範囲は [ws.md](ws.md) とユーザーの
2026-10-11 の指示。実装は別のセッションが行う。

この文書の「事実」は main `e8adcdf89` の file を読んだ物で、file を添える。「確かめた」は第 2 版で host（scratchpad の中、tree に何も
書かない）で走らせて確かめた物（§15 に command と結果）。「決定」は D 番号、「人の判断」は H・J 番号（§13）、確かめていない物は U 番号（§14）。

## 0. 読んだ物と前提

- 規則: `AGENTS.md`（zedBSD の規則）、`plan/guardrail.md`、`plan/coding-style.md`（全文、ANSI C の基準、§12 試験だけの環境変数の禁止）、
  `plan/master-design-policy.md`。
- 手本: `plan/ws083/ws.md`・`design.md`・`phase004/phase.md`、`plan/ws199/ws.md`。
- libmedia: `userland/desktop/libmedia/`（`media.h`・`media-decoder.h`・`media-private.h`・`decoder.c`・`avcodec.c`・`avcodec-layout.h`・
  `bitstream.c`・`engine.c`・`exports.map`・`Makefile`）。
- 容器の reader: `userland/desktop/mediafile/`（`mediafile.h`・`mediafile-private.h`・`mp4.c`・`ts.c`、他に mkv・ogg・avi）。
- app: `userland/desktop/videoplayer/`（`videoplayer.h`・`main.c`・`media.c`・`audio.c`）、`userland/desktop/music/`（`play.h`・`play.c`・`main.c`）、
  `plan/tools/aat/scenarios/helpers_music.py`。
- Vulkan Video: `userland/desktop/libvulkan/instance.c`・`dispatch.c`・`video.c`、`userland/tests/vkvideo-probe/`（`main.c`・`h264.[ch]`・`dpb.[ch]`・
  `frame.[ch]`）、`src/drivers/gpu/i915/render/video.c`・`worker.c`、`docs/reference/vulkan-video.md`、`include/libc/vulkan/vk_video/`。
- 外部 package: `userland/packages/multimedia/libavcodec/Makefile`（FFmpeg 9.0.2、LGPL、`--disable-hwaccels`）、`build/distfiles/ffmpeg-9.0.2.tar.xz`
  （SHA-256 `8c385028…e96e002e`、Makefile の値と一致を確かめた）。
- 試験: `plan/tools/media/`、`plan/tools/guest/test-image.sh`、`tests/scenarios/apps/{videoplayer,music}/`、`plan/ws083/tests/`（`make-streams.sh`・`streams/`）、
  `config/ci/config-amd64.mk`、`/home/awe/zedbsd-media/`（tree の外の 1080p の試料）。
- host の道具: ffmpeg 7.1.5（Debian 13）、libx264。AAC の encoder は ffmpeg の `aac`（AAC-LC）だけで、**HE-AAC を作れる encoder（libfdk_aac）は無い**
  （確かめた）。host の Vulkan は llvmpipe だけで **Vulkan Video は無い**（確かめた）。

## 1. 今の形（事実）

### 1.1 libmedia

- 3 つの層: 容器の reader（`mediafile`、自前）→ decoder（`decoder.c` の back end の表）→ 再生の engine（`engine.c`、browser の `<video>` 用）。
  Video Player（`videoplayer/media.c`）と Music（`music/play.c`）は engine を使わず、`media_file_*` と `media_decoder_*` を直に使う。
- back end の表 `decoder_backends[]`（`decoder.c`）は今 `media_avcodec_ops` 1 つ。表は**全 container の track** に使われる。表の順に open を試し、
  最初に受けた back end が decode する。誰も受けなければ最初の back end の問題（`MEDIA_PROBLEM_*`）を返す。
- `avcodec.c` は libavcodec 等を dlopen し、header 無しで呼ぶ。picture は AVFrame の参照、表示は libswscale で BGRA へ。video の picture の時刻は
  「送った packet の pts を整列して、出てくる picture に小さい順に当てる」（`avcodec.c` の `codec_pending_add` と `addin_receive`）。表示順は decoder
  （POC）が決めるので、decode の順の時刻しか持たない container（AVI、ctts の無い mp4）でも正しい。
- `addin_receive` は frame を置き換え、`addin_sound` はその frame だけを変換する。player が `media_decoder_sound` を呼ばなかった frame は次の receive で
  暗黙に捨てられる（`avcodec.c` 468〜615）。音の変換は線形補間で、最初の 2 channel だけ。
- `bitstream.c` は avcC の長さ付きの NAL を Annex B に、AAC の raw に ADTS の header を付ける。
- `MEDIA_PROBLEM_MISSING`・`_VERSION`・`_FORMAT`。`exports.map` は `media_decoder_*`・`media_frame_*` 等を wildcard で出す。NEEDED は libc だけ。

### 1.2 容器の reader

- `mediafile/mp4.c` は**既にある**（WS122 p003、ws177-p027）。stsd（最初の entry）・stts・ctts・stsc・stsz/stz2・stco/co64・stss、fragmented
  （mvex・moof・traf・tfhd・tfdt・trun・trex）を open の時に全部読む。edts の最初の edit で pts をずらす。seek は最初の video track の、時刻以前の最後の
  sync sample へ、他の track はその dts 以前の最後の sample へ。avcC・esds の ASC を private data に。`avc1`・`avc3`・`mp4a` を知る。
- 足りない物: pasp・colr を読まない。edts の最初の edit の長さ（表示の終わり）を持たない。stsd の 2 つ目以降の entry を読まない。
- `ts.c` の AAC は ADTS の header 付きの frame を 1 packet ずつ、private data 無し、codec 名 `adts` で渡す（`ts.c` 975・1490〜1499）。TS の H.264 は
  avcC 無しの Annex B。mp4 の `avc3` は avcC に SPS・PPS が無くてよい。mkv は CodecPrivate に avcC・ASC。AVI の H.264 は普通 private data 無しの Annex B。

### 1.3 app

- Video Player: window の thread（`main.c`）と media の thread（`media.c`）。decoder の open は `vp_media_open` の中、**window の thread** で行う
  （`media.c` 97〜103）。media の thread が読み・decode し、picture を 8 枚の ring に置き、音を `vp_audio_write`（48 kHz・stereo・16 bit）に書く。
  時計は音、無ければ monotonic。window は時刻の来た picture を `vp_media_take` で取り（遅れた物は捨てる）、`media_frame_scale` で canvas の BGRA に描く。
  `time < skip_before` の frame は `media_decoder_sound` を呼ばずに捨てる（`media.c` 608〜616）。log `VIDEOPLAYER FRAMES shown=%u time_ms=%lld` は
  1 枚目と 100 枚ごと（`main.c` 770〜771）。
- Music: `play.c` の thread が sound の track を decode して `vp_audio_write`。`main.c` は起動の時に `media_codec_load()` を呼び、失敗で
  「Playing needs libavcodec (the libavcodec package).」。その時の `MUSIC CODEC load error=` の行（`avcodec.c` の `media_log`）を AAT の helper
  （`plan/tools/aat/scenarios/helpers_music.py` 78・87）が待つ。
- release の image・CI の構成（`config/ci/config-amd64.mk`）は libavcodec を入れる。

### 1.4 Vulkan Video（WS083）

- zedBSD の libvulkan は i915（Gen12、VCS0・MFX）の上で `VK_KHR_video_queue`・`VK_KHR_video_decode_queue`・`VK_KHR_video_decode_h264`・
  `VK_KHR_synchronization2` を出す（2026-10-11 から既定で ON）。family 1 が video decode。
- **instance の apiVersion は 1.0 だけを受ける**（`libvulkan/instance.c` 59〜66、1.1 以上は `VK_ERROR_INCOMPATIBLE_DRIVER`）。video の family の問いは
  `vkGetPhysicalDeviceQueueFamilyProperties2KHR` で、instance の拡張 `VK_KHR_get_physical_device_properties2` が要る（`dispatch.c` は有効にした拡張の
  関数だけを返す）。probe は 1.0 とこの拡張で instance を作る（`vkvideo-probe/main.c` 289〜302）。
- 範囲: Baseline・Main・High、8 bit、4:2:0、progressive、4096x4096・36864 MB、DPB 17・参照 16、`maxLevelIdc` は
  `STD_VIDEO_H264_LEVEL_IDC_5_1`（列挙の値 **14**、`render/video.c` 132〜133）。i915 の実行器は SPS の level_idc を読むが比べない（`render/video.c` 1860、
  他に level の検べは無い。grep で確かめた）。
- 出力の picture は NV12・OPTIMAL・usage は DECODE_DST と DPB だけ（SAMPLED・TRANSFER_SRC は無い、WS083 HD5）。host visible の memory に bind し、
  `vkGetImageSubresourceLayout` の offset と pitch で Intel Tile Y を de-tile して読む（zedBSD の約束）。memory type は 1 つで HOST_CACHED。
- 規則違反の submit（空の slot を参照する等）は submit 全体が DEVICE_LOST（`render/video.c` 17〜25・660〜684）。hang した kernel session は quarantine され、
  video の submit・video session の作成を拒む（同 661〜672、WS083 R-S3）。video の「止まった」印は device 全体。engine の reset と回復は WS083 p007 で、
  実機は未確認。
- kernel の video の hardware context は device に **8 個**（`worker.c` 78 `I915_WORKER_VIDEO_CONTEXTS`）。kernel session（= libvulkan の instance）ごとに 1 つ。
- i915 の worker は 1 本で同期。1080p の 1 decode は 4.2〜5.2 ms（5330）。decode と desktop の描画は互いに待つ（WS083 design の U8）。
- Venus（QEMU）・他の GPU には video の family が無い。
- `vkvideo-probe`: Annex B を読み（`h264.c`: SPS・PPS・scaling list・slice header・POC type 0・2・MMCO）、DPB（`dpb.c`: slot の割り当て、sliding window、
  MMCO 1〜4・6）を持つ。**MMCO 5 と frame_num の gap は扱わず止まる**（`dpb.c` 15〜16・82〜85・346、`h264.c` 786〜788）。表示順は IDR の間で POC で並べる
  （全部を貯める）。

## 2. 目標と範囲

- **目標 A**: libavcodec の無い image で、Video Player が H.264（Baseline・Main・High、8 bit 4:2:0 progressive）＋AAC-LC の mp4 を、音つき・同期・
  seek・終わりまで再生する。H.264 は Vulkan Video（5330 の i915）で decode する。
- **目標 B**: libavcodec の無い image で、Music が .m4a（AAC-LC）を libmedia の自前の decoder で再生する。
- **回帰させない**（第 2 版で足した）: 自前の back end は表の先頭に入り全 container の H.264・AAC を受けるので、libavcodec の入った image で今再生できる
  file（TS・mkv・AVI の H.264・AAC、ctts の無い mp4）を悪くしない（§3.1、§10.2 の回帰の試験、J1）。
- 範囲の外: H.264 の CPU の decoder（H1）、HE-AAC の SBR・PS の再現（H3）、他の codec、encode、interlaced、10 bit、AAC の Main・LTP・SSR・ER 系、
  DRM、streaming、GPU の image の共有の表示（H6）。

## 3. 全体の構成

```
mediafile (mp4・mkv・ts・avi …) ──packet──▶ decoder.c の back end の表（2 段で試す、§9.1）
                                              1. vkvideo   H.264 → Vulkan Video（libvulkan を dlopen）→ de-tile → NV12 の picture
                                              2. aac       AAC-LC（ASC か ADTS）→ 自前の decoder → float PCM
                                              3. avcodec   他の全て（libavcodec が入っていれば）
                                            ◀──picture（NV12・自前の scaler で BGRA へ）／sound（16 bit stereo、呼び手の rate）
```

新しい file（`userland/desktop/libmedia/`、全部 Zlib、自前）:

| file | 中身 |
| --- | --- |
| `bits.c` | bit の reader（MSB から、`ue(v)`・`se(v)`、overrun の印、RBSP の emulation prevention の除去） |
| `picture.c` | 自前の back end の picture（NV12 linear、参照の数え、空きの list）と scaler（NV12 → BGRA） |
| `sound.c` | 音の出力の変換（downmix、polyphase の resampler、16 bit への丸め、切り詰め） |
| `aac.h`・`aac.c` | AAC の back end（ops、ASC と ADTS、frame の構文、Huffman の復号） |
| `aac-tools.c` | 逆量子化、PNS、M/S、intensity、TNS |
| `aac-filterbank.c` | IMDCT（FFT による）、窓（sine・KBD）、窓の列、overlap-add |
| `aac-tables.c` | Huffman の codebook 12 個、scalefactor band、TNS の max band（生成した表、§6.7） |
| `h264.h`・`h264.c` | SPS（VUI を含む）・PPS・slice header・POC（type 0・1・2、MMCO 5 を含む） |
| `h264-dpb.c` | DPB の slot の計画と marking（gap・MMCO 5 を含む）、表示順（POC の bumping） |
| `vkvideo.h`・`vkvideo.c` | Vulkan Video の back end（ops、session、parameters、decode、読み出し、表示順、時刻） |
| `vkvideo-device.c` | 共有の instance・device・queue（process に 1 つ、作り直せる）、関数の表 |

D1: 既存の `struct media_decoder_ops` に back end を足す形にし、app の使う口はほぼ変えない（§9）。engine・Video Player・Music は同じ表を通る。

D2: vkvideo-probe の `h264.c`・`dpb.c`・`frame.c` は同じ project の Zlib の code なので、libmedia へ写して直す。probe は WS083 の試験の道具として今のまま。
**probe に無い物（MMCO 5、frame_num の gap、POC type 1、VUI）は新しい仕事**で、probe との一致の試験では確かめられない（§10.2 で別に試す）。

### 3.1 container ごとの入力の形（第 2 版で足した、H-05）

| container・形 | AAC | H.264 |
| --- | --- | --- |
| mp4・m4a（`mp4a`＋esds、`avc1`＋avcC） | ASC（private data）。1 packet = 1 raw_data_block | avcC の SPS・PPS で open の時に分かる。1 packet = 1 AU（長さ付きの NAL） |
| mp4 の `avc3` | — | avcC に SPS・PPS が無いことがある → 下の「in-band だけ」 |
| mkv（`A_AAC`・`V_MPEG4/ISO/AVC`） | CodecPrivate の ASC | CodecPrivate の avcC |
| TS（codec 名 `adts`、H.264 の Annex B） | 各 packet が ADTS の header＋raw_data_block。header を読んで外す（§6.1） | private data 無し → 「in-band だけ」 |
| AVI | private data の ASC があれば mp4 と同じ。無く ADTS でもなければ track の rate・channels から LC と見なす（2 段目だけ） | 普通 private data 無しの Annex B → 「in-band だけ」 |

- D16（H.264 の「in-band だけ」の track）: 1 段目（degraded 0）では FORMAT を返して libavcodec に譲る。2 段目（degraded 1、libavcodec が受けなかった時）では
  受け、最初の in-band の SPS・PPS と I まで session を作らず、それまでの AU は捨てる。SPS が範囲の外（§5.8）と分かった時は send が EINVAL を返し、
  log に理由を 1 回出す（open の後なので PROFILE の notice にはならない。制限として記す）。
- AAC の ADTS は 1 段目で受ける（全 packet に header があり、最初の packet で形が分かる。track の rate・channels は `ts.c` が ADTS の header から入れる）。
  `number_of_raw_data_blocks_in_frame` > 0（1 つの ADTS に複数の block）は各 block の前の CRC を読み飛ばしながら全部を decode する。

## 4. mp4（ISO BMFF）の demuxer

D3: 新しく書かない。`mediafile/mp4.c` に足りない 3 点だけを足す。

| 足す物 | 中身 | 使う所 |
| --- | --- | --- |
| pasp | visual sample entry の子の `pasp`（hSpacing・vSpacing）を `media_track` の `sar_num`・`sar_den`（0 は未知）に | H.264 の VUI に SAR が無い時 |
| colr | `colr` の `nclx`（primaries・transfer・matrix・full range）と QuickTime の `nclc`（primaries・transfer・matrix、full range は無い）を `colour_matrix`・`full_range`（-1 未知）に | VUI に色が無い時 |
| 表示の終わり | edts の最初の実の edit の segment の長さから `end_us`（0 は未知） | AAC の末尾の切り詰め（§6.5） |

- avcC・esds を見つけた後も、続く子の pasp・colr を読む（今は configuration で return している）。
- D4: **fragmented mp4 は扱う**（既に読める）。段階的な読みはしない（open の時に全 moof を読む）。open の時間は U1。
- stsd の 2 つ目以降の entry は扱わない（制限）。

### 4.1 時刻（pts・dts）と priming

- mp4.c の `pts_us`（ctts と edit の shift の後）と `dts_us` を使う。ただし H.264 の表示順は container の pts ではなく POC で決める（§5.6）。
- ffmpeg の AAC の encoder の .m4a は `elst` の media_time が **1024**（priming、確かめた §15 E2・E4）。iTunes の file は 2112 で、`iTunSMPB` の metadata にも
  書く物がある。D17: edts だけを使い、`iTunSMPB` は読まない（edts の無い iTunes の file は頭の priming 分（約 46 ms）の無音が残り、末尾の padding も残る。
  制限として docs に書く）。

## 5. H.264: Vulkan Video の back end

### 5.1 流れ

1. open（track、degraded）: codec が H.264 でなければ FORMAT。avcC に SPS があれば parse し、範囲（§5.8）の外なら PROFILE。avcC に SPS が無ければ D16。
   共有の device（§5.4）が無い・video の family が無いなら DEVICE。open で作るのは device の参照と parser だけで、session・image は最初の decode の前
   （media の thread）に作る（L-01: open は window の thread で走る）。
2. send（packet）: `media_bitstream_convert` で Annex B に。NAL を読み、SPS（7）・PPS（8）を更新、slice（1・5）を集めて 1 AU = 1 picture。SEI（6）・
   AUD（9）・filler（12）・SPS ext（13）・14・15・20 は飛ばす。data partition（2〜4）は EINVAL（PROFILE の SPS の stream は open で断れている）。
   冗長 slice（`redundant_pic_cnt` > 0）は捨てる。並べ替えの待ち行列が満ちていれば EAGAIN（player は receive で取り出してから送り直す）。
3. 1 picture の decode: DPB の計画（§5.3）→ 参照の slot の検べ（D18）→ bitstream の buffer に slice を start code 付きで写す（32 byte 揃え）→ 記録 →
   queue の mutex の中で submit・fence を待つ → result status → 出力の slot を de-tile して NV12 の picture に（§5.5）→ marking → 表示順の待ち行列へ（§5.6）。
4. receive: 待ち行列の bumping で出る picture に、送った packet の pts の整列の最小を当てて出す（§5.6）。
5. flush（seek）: 待ち行列と pts を捨て、DPB を空にし、次の decode を RESET にし、次の I まで捨てる（§5.7）。
6. close: queue を idle にし、decoder の object を壊す。共有の device の参照を 1 減らす。

D5: decode は media の thread で同期に行う（i915 の submit は decode の終わりまで戻らない）。

D18: submit の前に、計画の参照の slot が全部「picture を持つ」ことを検べ、持たない slot を参照に積まない（積むと i915 が submit 全体を DEVICE_LOST にする）。
検べで外れた picture は decode せずに捨てる（log に数える）。

### 5.2 parser（`h264.c`）

vkvideo-probe の `h264.c` を元に、次を変える・足す。

- 入力は 1 AU の Annex B。`struct h264_parser`（SPS・PPS の表、POC の状態）に 1 AU ずつ渡し、`struct h264_picture`（`StdVideoDecodeH264PictureInfo`、slice の
  offset・size、slice type、marking、`redundant_pic_cnt`）を返す。
- SPS: 今の物＋**VUI**（aspect_ratio_info と 255 の明示の SAR、video_full_range_flag・matrix_coefficients、bitstream_restriction の max_num_reorder_frames・
  max_dec_frame_buffering。hrd は構文の通り読んで捨てる）。D19（L-03）: VUI の中の読み誤り（途中で切れた VUI）は VUI だけを捨て、SPS は使う
  （probe は SPS ごと拒む）。frame_cropping は既にある。
- POC: type 0・2 に **type 1**（8.2.1.2）を足し、**MMCO 5**（8.2.1: tempPicOrderCnt、MMCO 5 の後の prevPicOrderCntMsb・Lsb・prevFrameNumOffset・
  prevFrameNum の reset）を追う。
- PPS: 今の物。FMO（num_slice_groups_minus1 > 0）は PROFILE。ASO（Baseline の slice の順の入れ替え）は検べず hardware に渡す（扱えるかは U9、result
  status の ERROR で分かる）。
- 誤った NAL はその AU を捨てて EINVAL。emulation prevention の除去は `bits.c`。
- D6: SPS・PPS の内容が変わった時・新しい id が増えた時は parameters の object を作り直す。古い object は今の decode の fence の後に壊す。

### 5.3 DPB（`h264-dpb.c`）

- vkvideo-probe の `dpb.c`（slot の割り当て、短期・長期、sliding window、MMCO 1〜4・6）を写し、次を**足す**（probe に無い）:
  - frame_num の gap（8.2.5.2）: 欠けた frame_num ごとに「non-existing」の frame を短期の参照として sliding window に入れる（slot は使わず、参照の list に
    載せず、表示しない）。`gaps_in_frame_num_value_allowed_flag` が 0 の stream で gap が来た時（壊れた file、seek の後に leading の参照を捨てた時）も同じ処理で
    DPB を保つ（規格の「意図しない欠け」の扱い、8.2.5.2 の注）。
  - MMCO 5（8.2.5.4.6）: 全部の参照を unused に、表示順の待ち行列を全部出す（§5.6）。
- slot の数 = `max_num_ref_frames + 1`（≦ 17）。D7: 出力は decode の直後に CPU の NV12 へ写す（§5.5）ので、slot は参照の有無だけで再利用できる。
- 表示順の深さ: VUI の `max_num_reorder_frames` があればそれ。無ければ profile 66（Baseline・Constrained Baseline、B の slice が無い）は 0、他は level の
  MaxDpbMbs（表 A-1）÷ picture の MB 数（≦ 16）。

### 5.4 Vulkan の object（`vkvideo-device.c`・`vkvideo.c`）

D8: libvulkan は **dlopen** し、`vkGetInstanceProcAddr`・`vkGetDeviceProcAddr` で関数の表を埋める。libmedia の NEEDED は libc のまま。一度読んだら
**dlclose しない**（L-02、process の終わりまで）。名は image の物を確かめる（U7）。

D9（第 2 版で直した、H-01・M-01）: 共有の device は **mutex と参照の数**で遅延に作る（`pthread_once` を使わない。作り直せるように）。

- instance: **apiVersion `VK_API_VERSION_1_0`**、instance の拡張 `VK_KHR_get_physical_device_properties2`（probe の `probe_instance` と同じ、
  `vkvideo-probe/main.c` 289〜302）。queue family は `vkGetPhysicalDeviceQueueFamilyProperties2KHR`（KHR の名で引く）と
  `VkQueueFamilyVideoPropertiesKHR` の chain で問う（probe の `probe_video_family`）。
- device: video の family（`VK_QUEUE_VIDEO_DECODE_BIT_KHR`、H.264 decode）のある最初の physical device、拡張 `VK_KHR_synchronization2`・
  `VK_KHR_video_queue`・`VK_KHR_video_decode_queue`・`VK_KHR_video_decode_h264`。queue への submit は mutex で守る。
- 「video の無い機械」の判定は 1 回だけ行い、process の中で覚える。
- 「壊れた」（DEVICE_LOST・ETIMEDOUT）時: 印を立て、新しい open には DEVICE を返す。参照の数が 0 になった時に device と **instance**（= kernel session、
  quarantine された物）を壊し、印を消す。次の open は新しい instance（新しい kernel session）を作る。kernel の video の engine が止まったままなら
  （WS083 p007 の回復が実機で動かない時）新しい session の video の作成が失敗し、DEVICE になる（libavcodec があればそちら）。
- kernel の video の context は device に 8 個（§1.4）。D9 で 1 process は 1 つを使う。9 個目の process の session の作成の失敗は
  `MEDIA_PROBLEM_BUSY`（§9.1）で「GPU の video の decoder が他の program に使われている」と分けて言う。どの Vulkan の結果で分かるかは U10。
- capability: `vkGetPhysicalDeviceVideoCapabilitiesKHR` を SPS の profile（stdProfileIdc、PROGRESSIVE）で問う。
- D20（M-03、level）: level では断らない。実際の制約で判断する: coded の大きさ ≦ maxCodedExtent、MB の数 ≦ 36864、`max_num_ref_frames` ≦
  maxActiveReferencePictures、slot の数 ≦ maxDpbSlots。Vulkan に渡す SPS の `level_idc` は SPS の level_idc を列挙に写し（表: 10→1_0(0)、11→1_1(1)、
  12→1_2、13→1_3、20→2_0(4)、21、22、30→3_0(7)、31、32、40→4_0(10)、41、42、50→5_0(13)、51→5_1(14)、52→5_2(15)、60→6_0(16)、61、62→6_2(18)。
  level 1b（level_idc 9、または 11 と constraint_set3 の Baseline・Main）は 1_0）、`maxLevelIdc` を越える時は `maxLevelIdc` に丸める。i915 は level を
  比べない（§1.4）。丸めが規格の VUID に触れないかは U11。
- session: maxCodedExtent = SPS の coded の大きさ、maxDpbSlots = slot の数、maxActiveReferencePictures = max_num_ref_frames、NV12。
- image: slot ごとに NV12・OPTIMAL・DST|DPB・profile list、HOST_VISIBLE に bind して map したまま。
- bitstream の buffer: 1 MiB から 2 倍ずつ（最大 64 MiB）。map したまま。
- result status の query pool（`queryResultStatusSupport` が TRUE の時）。
- SPS の大きさ・profile・参照の数の変化: queue を idle にし、session・image を作り直す。
- D21（M-02、memory の上限）: 1 decoder の slot の image の合計が 256 MiB を越える SPS は PROFILE（4K の NV12 の Tile Y は約 12.6 MB、17 slot で約 214 MB で
  収まる。8K 級を断る）。

### 5.5 decode した picture の表示: CPU への写し（D10）

- fence の後、出力の slot の image の PLANE_0・PLANE_1 を `vkGetImageSubresourceLayout` の offset・pitch で読み、Tile Y を de-tile して linear の NV12
  （crop の窓だけ）を picture の buffer に写す。式は probe の `frame.c` の `frame_tile_y_offset` と同じ。
- 表示は今と同じく window の thread が `media_frame_scale` で canvas の BGRA に描く。scaler は自前（§7.2）。
- 理由: decode の picture は SAMPLED・TRANSFER_SRC を持たない（WS083 HD5）。Video Player は canvas（CPU）に描く。1080p で約 3 MB の読み（U2）。
- 制限: de-tile は zedBSD の libvulkan の約束に頼る。他の Vulkan の実装では TRANSFER_SRC の経路が要る（Future）。
- H6（人の判断）: GPU で描く経路を足すか。

### 5.6 表示順と時刻（第 2 版で直した、H-04）

- D22: **表示順は H.264 の規則で決める**: decode した picture を POC とともに待ち行列に入れ、待ち行列の長さが §5.3 の深さを超えたら POC の最小を出す
  （bumping）。IDR・MMCO 5 の picture の前に、待ち行列を全部出す。drain（packet NULL）で全部出す。
- 時刻は今の add-in と同じ: 送った packet の `pts_us` を整列した列に入れ、出る picture に小さい順に当てる。decode の順の時刻しか持たない container
  （AVI、ctts の無い mp4）でも正しく付く。同じ pts の捨ては要らない。
- 待ち行列の上限は 17。満ちた時の send は EAGAIN（§5.1）。

### 5.7 seek と先頭の picture（第 2 版で直した、H-02）

- flush の後は、次の I picture（IDR、または全 slice が I の非 IDR の picture）まで AU を捨てる。`mp4_seek` は stss の sync sample へ移す。x264 の open GOP では
  stss の I は**非 IDR**（nal_unit_type 1）で、leading の B（I より POC が小さい）が続く（確かめた §15 E3）。
- 非 IDR の I から始めた時: DPB は空から始め、`prevRefFrameNum` をその I の frame_num に、POC の状態をその I から（type 0 は prevPicOrderCntMsb = 0・
  prevPicOrderCntLsb = 0 として I の POC を作る）。leading の B と、D18 で参照の slot が無い P・B は decode せずに捨てる。捨てた参照の B の分の frame_num の
  欠けは §5.3 の gap の処理で埋まり、次の P は gap の non-existing の frame を参照に持たないので decode できる（host 試験で確かめる、§10.2）。
- 表示の側の `skip_before` は今の player・engine のまま。

### 5.8 扱う範囲

| 項目 | 扱い |
| --- | --- |
| profile | 66（Constrained Baseline・Baseline）、77（Main）、100（High）。他（High 10・4:2:2・4:4:4・Extended・SVC・MVC）は PROFILE |
| 形 | 8 bit、4:2:0、frame_mbs_only_flag = 1。interlaced は PROFILE |
| 大きさ | §5.4 の D20 |
| entropy | CAVLC・CABAC |
| B frame | 扱う（B pyramid を含む） |
| 参照 | 短期・長期、MMCO 1〜6、frame_num の gap、weighted、scaling list |
| slice | 1 picture に 256 まで（越えると skip）。冗長 slice は捨てる。ASO は U9 |
| 表示順 | §5.6（深さ ≦ 16） |

### 5.9 失敗の扱い

| 事象 | 扱い |
| --- | --- |
| result status が ERROR | その picture を出さない。log（最初の 8 回と 100 回ごと）。連続 30 枚で EIO（player は FAILED） |
| submit が DEVICE_LOST | decoder を失敗（EIO）、共有の device を「壊れた」に（§5.4）。J6: libavcodec への切り替えはしない（推し） |
| fence の ETIMEDOUT（L-07） | DEVICE_LOST と同じ扱い。`vkDeviceWaitIdle` が成功すれば decoder の object を壊す。失敗すれば object を壊さずに持ち（GPU が使い続けている恐れ）、device を壊す時に一緒に放す |
| ENOMEM | player は FAILED |
| 参照の無い P・B | 捨てる（§5.7・D18） |

### 5.10 Vulkan Video の無い機械

- vkvideo は DEVICE を返し、libavcodec が入っていればそれが decode する。無ければ notice（§9.3）。
- **H1（人の判断）**: CPU の H.264 decoder を WS202 に入れるか（推し: 入れない、§13）。

## 6. AAC: 自前の decoder

### 6.1 入力（ASC と ADTS）

- ASC: audioObjectType（5 bit、31 は escape）、samplingFrequencyIndex（15 は 24 bit の明示）、channelConfiguration。
  - AOT 2（LC）: GASpecificConfig の frameLengthFlag は 0 でなければ PROFILE。channelConfiguration 0 は ASC の中の PCE。
  - **用語（第 2 版で直した、M-11）**: (1) 階層の明示の signalling = AOT 5（SBR）・29（PS）で始まり、extensionSamplingFrequency と中の AOT（2 でなければ
    PROFILE）を持つ。(2) **後方互換の明示の signalling** = AOT 2 の GASpecificConfig の後の `syncExtensionType` 0x2b7・extension AOT 5・sbrPresentFlag
    （と 0x548 の PS）。(3) **暗黙の signalling** = ASC に何も無く、FIL の中の SBR の extension（EXT_SBR_DATA）だけ。(1)・(2) の sbrPresentFlag 1 を
    「HE-AAC」と呼ぶ。扱いは §6.6。
  - AOT 1・3・4・17 以上は PROFILE。
- ADTS（§3.1）: syncword 0xFFF、profile（+1 が AOT、LC 以外は PROFILE）、rate の index、channel_configuration、protection_absent（0 なら 16 bit の CRC を
  読み飛ばす）、number_of_raw_data_blocks_in_frame。open の時は private data が無いので、最初の packet の header で config を決め、以後の header は形が
  同じかだけ検べる（変われば作り直す）。
- 無い時: §3.1 の表。

### 6.2 frame の構文（raw_data_block）

ISO/IEC 14496-3 の構文を読む。

- 要素: SCE・CPE・CCE・LFE・DSE・PCE・FIL・END。DSE・FIL は長さの分を飛ばす。PCE は channel の並びを更新する。CCE は読んで捨てる（log に 1 回）。
- ICS: global_gain、ics_info、section_data、scale_factor_data、pulse_data（long だけ）、tns_data、gain_control_data_present（1 なら失敗）、spectral_data。
- CPE: common_window、ms_mask_present（0・1・2）と ms_used。

### 6.3 道具（`aac-tools.c`）

1. spectral の Huffman の復号。
2. pulse を量子化値に足す。
3. 逆量子化: `sign(q) · |q|^(4/3) × 2^(0.25·(sf − 100))`。
4. PNS（NOISE_HCB の band）: 一様乱数の band を energy に合わせる。CPE で ms_used の noise の band は左右に同じ雑音（相関）。
5. M/S: ms_used の band で L = M + S、R = M − S。**ただし（第 2 版、M-04）右の channel の codebook が INTENSITY_HCB・INTENSITY_HCB2・NOISE_HCB の band には
   掛けない**（intensity の band では ms_used は intensity の符号の反転の印、PNS の band では相関の雑音の印）。
6. intensity: 右の channel の INTENSITY_HCB・HCB2 の band を、左の値 × `2^(-0.25·position)` と符号（codebook と ms_used）から作る。
7. TNS: 各窓の filter（order ≦ 12 の long、≦ 7 の short）。
8. filterbank（§6.4）。

### 6.4 filterbank（`aac-filterbank.c`）

- IMDCT: N = 2048・256、N/4 点の複素 FFT による。
- 窓: sine と KBD（α = 4・6、I0 の級数で計算）。窓の列 4 種、前の window_shape、overlap-add。計算は float。

### 6.5 出力（`sound.c`）

- decoder の出力: channel ごと 1024 sample の float（±1.0 = 16 bit の ±32768）。
- 先頭の切り詰め（D12）: 時刻 0 より前の sample を落とす（sample の単位、edts の media_time 分）。
- 末尾の切り詰め: `media_track.end_us` があればそれより後を落とす（ffmpeg の mov の reader は先頭だけ切り、末尾は切らない: §15 E4。比べる時に注意、§10.2）。
- downmix（stereo へ）: mono は左右に同じ。3〜8 channel は PCE の matrix_mixdown か ITU-R BS.775 の係数で、和の最大が 1 を越えない正規化。LFE は捨てる。
  channel の並び（channelConfiguration 6）: bitstream は SCE(C)・CPE(L,R)・CPE(Ls,Rs)・LFE。
- resample（D13、第 2 版で直した M-06・L-05）: core の rate から **`media_decoder_sound` の rate 引数**（player は 48 kHz）へ。入力と出力が同じ rate なら通さない。
  違う時は polyphase の windowed sinc: **64 tap**、Kaiser、位相 256 で**位相の間を線形補間**。目標: 通過域 0〜0.41×入力の rate（44.1 kHz で 18 kHz）で ±0.1 dB、
  阻止域（入力の Nyquist より上）で −80 dB 以下。表は 1 回だけ作る（§6.9）。
- 16 bit への丸め（四捨五入と飽和）。

### 6.6 HE-AAC

- **H3（人の判断）**: 推し (a): SBR・PS は再現せず core の AAC-LC だけを鳴らす。HE-AAC（§6.1 の (1)・(2)）は 2 段目（degraded 1）だけで受ける（libavcodec が
  あればそちらが 1 段目で受ける）。(3) の暗黙の signalling は ASC から分からないので、1 段目で LC として受け、core の rate で鳴らす（FIL の SBR は飛ばす）。
- 試験（M-11）: host の script が LC の .m4a の esds の ASC を (1)・(2) の形に書き換えた file を作り、core が元の LC と同じに鳴ること、1 段目で断り 2 段目で
  受けることを確かめる（§10.2）。

### 6.7 表と license

- 計算で作る表: `|q|^(4/3)`、`2^(0.25·x)`、sine・KBD の窓、FFT の twiddle、resample の sinc。
- spec の data の表: Huffman の codebook 1〜11 と scalefactor、scalefactor band の offset、TNS の max band、rate の表。
- **H5（人の判断）**: 出典。推し (a): FFmpeg 9.0.2 の tarball の `libavcodec/aactab.c` から**値だけ**を host の script で読み、zedBSD の形で生成する。
  確かめた事実（§15 E6）: aactab.c は C の配列の初期化子で、`codes1`〜`codes11`・`bits1`〜`bits11`（spectral）、`ff_aac_scalefactor_code`・`_bits`（121）、
  `swb_offset_1024_*`・`swb_offset_128_*`、`ff_aac_num_swb_1024`・`_128`、`ff_tns_max_bands_1024`・`_128` の形で読める。file は LGPL-2.1+（Oded Shimon 他）。
  guardrail の「値は事実」は WS141・WS037 だけの決定なので、WS202 にはユーザーの決定が要る。
- 参照する仕様: ISO/IEC 14496-3、13818-7、14496-12・14・15、ITU-T H.264（2021-08）、Khronos Vulkan の video の章と `vk_video`、ITU-R BS.775、BT.601・709・2020。

### 6.8 性能

AAC-LC の 48 kHz stereo は 1 秒 94 frame。1 frame ≒ 30〜60 µs の見込み（U4）。

### 6.9 共有の表の初期化（M-13）

計算で作る表（`|q|^(4/3)`、窓、twiddle、Huffman の 2 段の表、sinc）は process に 1 つで、`pthread_once` で 1 回作る（作り直しは要らない）。2 つの thread が
同時に初めての open をしても競合しないことを TSan の host 試験で確かめる。

## 7. 共通の部品

### 7.1 picture（`picture.c`）

- `struct media_picture`: 表示の大きさ、SAR、色、NV12 の 2 plane と pitch、参照の数、属する pool。
- D11（第 2 版で直した、M-02）: pool は**空きの list の上限**だけを持つ（上限は表示の深さ＋4）。取得は空きがあればそれ、無ければ malloc（尽きない）。返却は
  空きが上限未満なら list へ、以上なら free。decoder が close した後も、window が持つ picture は使える（pool は参照の数で生きる）。mutex は pool に 1 つ。
  同時に生きる枚数の最大は、深さ＋1（出す直前）＋player の ring 8＋window 2（`player->picture` と取り替えの間の `taken`）＋decode 中 1 ＝ 深さ＋12
  （engine は ring 4＋shown 1 で 深さ＋7）。1080p で深さ 4 なら 16 枚 × 3.1 MB ≒ 50 MB（slot の image と別）。

### 7.2 scaler（NV12 → BGRA）

- `media_frame_scale` の約束は今のまま。
- 色: VUI → colr → 既定（高さ > 576 は BT.709、他は BT.601）。SAR は VUI → pasp → 1:1（L-03: bitstream の値が encoder の意図で、pasp は container の写しなので
  VUI を先にする）。limited・full、固定小数点。
- bilinear（16.16、行の表を cache）、1:1 の速い道。目標（U5）: 1080p → 1080p 8 ms 以下、→ 720p 10 ms 以下。超えるなら 2 thread に。

### 7.3 bit reader（`bits.c`）

MSB から読む reader。`ue(v)`・`se(v)`、overrun の印。emulation prevention の除去は別の関数（slice の header だけ）。

## 8. A/V の同期・seek・終わり

### 8.1 同期

- 今の Video Player の形を保つ。picture の時刻は §5.6、音の時刻は packet の pts（切り詰めの後）。
- log: 今の `VIDEOPLAYER FRAMES shown=N time_ms=T` を保ち `late=M` を**足す**（L-04）。1 枚目・100 枚ごとに加えて、一時停止・終わり・close の時にも 1 行出す。

### 8.2 seek（第 2 版で直した、M-12）

- 流れは今のまま（`media_file_seek` → flush → `skip_before` → 時計の合わせ直し）。
- 今の player は `time < skip_before` の音の frame を丸ごと捨てるので、最初に書く sample は目標より**後**で、音は絵より最大 1 frame **遅れる**（8 kHz で 128 ms、
  22.05 kHz で 46 ms、48 kHz で 21 ms）。
- D14（改）: `int media_decoder_trim(struct media_decoder *, int64_t before_us)` を足す（§9.1）。flush の後に player が呼ぶ。自前の AAC は before_us より前の sample を
  sample の単位で落とし（frame の途中から始まる）、0 を返す。avcodec は ENOTSUP を返し、player は今の frame の単位の捨てを続ける。

### 8.3 終わり

- ENODATA で両 decoder に NULL（drain）。vkvideo は待ち行列を全部出し、AAC は末尾の切り詰めの後の音を出す。

### 8.4 一方だけ decode できる時

- video を decode できない時は今の通り開くのを失敗にし、notice を出す（D15）。音を decode できない時は絵だけ再生する。

### 8.5 音の約束（第 2 版で足した、H-06）

- D23: `media-decoder.h` に約束を書く: 「`media_decoder_sound` は直前の `media_decoder_receive` の frame の分だけを変換する。次の receive は変換されなかった残りを
  捨てる」。自前の AAC もこれに従う（add-in と同じ）。capacity は player の 8192 frame で、1024 sample の frame は 8 kHz → 48 kHz でも 6144 で収まる。
  96 kHz → 48 kHz は 512。rate の比で capacity を越える組み合わせ（入力 < 6 kHz）は ASC の rate の表に無い。
- 変換されなかった frame を捨てた時は resampler の履歴を空にする（意図した欠けなので、続きの連続は要らない）。

## 9. API と app の変え方

### 9.1 `media-decoder.h`・`decoder.c`

| 変更 | 中身 |
| --- | --- |
| 問題の追加 | `MEDIA_PROBLEM_DEVICE 4`（自前の decoder が扱う codec だが、この機械の GPU が decode できない）、`MEDIA_PROBLEM_PROFILE 5`（profile・形を扱わない）、`MEDIA_PROBLEM_BUSY 6`（GPU の video の decoder が足りない、§5.4） |
| 問題の選び方 | 誰も受けない時: 自前が DEVICE・PROFILE・BUSY を言い、libavcodec が MISSING・VERSION ならその自前の問題。自前が FORMAT なら libavcodec の問題。全部 FORMAT なら FORMAT |
| 2 段の試し | ops の open に `int degraded`。1 段目は degraded 0、誰も受けなければ degraded 1。自前の back end が 2 段目だけで受ける物: HE-AAC（H3 (a)）、in-band だけの H.264（D16）、ASC も ADTS も無い AAC |
| 音の約束 | §8.5 の D23 を comment に |
| 切り詰め | `int media_decoder_trim(struct media_decoder *, int64_t before_us)`（§8.2） |
| back end の名 | `const char *media_decoder_backend(const struct media_decoder *)`（"libmedia"・"vulkan-video"・"libavcodec"） |
| 縦横比 | `void media_frame_aspect(const struct media_frame *, int *num, int *den)` |
| 表 | `{ &media_vkvideo_ops, &media_aac_ops, &media_avcodec_ops }`（J1 の答えで container を絞る時は、自前の back end の open が track の container の名を見て FORMAT を返す） |
| `media_codec_load`・`_reason` | 残す。app は起動の門に使わない |

- 試験のための口を export しない（M-10）: 試験は source を直に compile して内部の関数（`aac_*`、`h264_*`、`vkvideo_*` の static でない内部の名）を呼ぶ。
  `media_decoder_*` の名の試験の口は作らない（`exports.map` の wildcard で export されるため）。

### 9.2 libavcodec の入った build（H2）

推し (a): 両方を持ち、自前が先、release は libavcodec のまま。試験の image は libavcodec を入れずに自前の経路を確かめる。§3.1 の回帰の試験を受け入れの条件にする。

### 9.3 Video Player

- notice: DEVICE「This computer's GPU cannot decode H.264 video, and FFmpeg's libavcodec is not installed.」、PROFILE「This video's format (for example interlaced or
  10-bit H.264) is not supported.」、BUSY「The GPU's video decoder is in use by other programs. Close one and try again.」、MISSING「This video's codec needs FFmpeg's
  libavcodec, which is not installed.」。
- 縦横比、log（`video=<codec>/<backend> audio=<codec>/<backend>`、§8.1）、flush の後の `media_decoder_trim`。
- open の時間（L-01）: vkvideo の open は device の参照と parser だけ（§5.1）。最初の open は instance と device を作る。p012 で open の時間を測り、200 ms を越えるなら
  open を media の thread へ移す（player の変更、その時の Phase で）。

### 9.4 Music

- 起動の `media_codec_load()` の門を外す。曲を開けない時に問題から文を出す。log `MUSIC PLAY open codec=aac backend=libmedia container=… duration_ms=…`。
- AAT の helper（M-08）: `helpers_music.py` の `MUSIC CODEC load error=` の待ちを、`MUSIC PLAY open codec=aac backend=` の行の確かめに変える（WS の外の file、Q1 へ差分）。
- seek の後の `media_decoder_trim`。

### 9.5 engine（browser の `<video>`）

表を通るので自動で自前の decoder を使う。browser の process が Vulkan の instance を 1 つ作り、video の context を 1 つ使う。**J2（人の判断）**: WS074 を止めている間、
engine だけ自前の H.264 を避けるか（推し: 避けない）。

## 10. 試験

規則: 細かい修正ごとに回帰を回さない。実装の担当は build（warning 0）と変えた所の host 試験だけ。QEMU・実機は T1 へ。

### 10.1 試験の stream（`plan/ws202/tests/make-streams.sh`、p002）

host の ffmpeg 7.1.5・libx264 で合成の素材から作る。tree に入れるのは小さい物だけ（合計 1 MiB 未満）。版と引数を script の頭に書く。

| 名 | 中身 | 確かめる物 |
| --- | --- | --- |
| `aac-lc-stereo-44k.m4a` | 4 s、sine の sweep と和音、128 kb/s、`-aac_pns 0 -aac_is 0` | 基本、priming（edts 1024） |
| `aac-lc-mono-22k.m4a` | 3 s、mono 22.05 kHz、64 kb/s、`-aac_pns 0` | mono、resample |
| `aac-lc-51-48k.m4a` | 3 s、5.1（`-ch_layout 5.1`）、320 kb/s、`-aac_pns 0 -aac_is 0` | 多 channel、downmix、LFE |
| `aac-short.m4a` | 3 s、impulse（aevalsrc）、`-aac_pns 0` | EIGHT_SHORT・START・STOP |
| `aac-ms-tns.m4a` | 4 s、stereo の音楽に似た素材、`-aac_pns 0 -aac_is 0 -aac_ms 1 -aac_tns 1` | M/S・TNS（波形の比べ） |
| `aac-is-pns.m4a` | 4 s、雑音＋音、48 kb/s、`-aac_pns 1 -aac_is 1 -aac_ms 1` | PNS・intensity と M/S の除外（band の比べ） |
| `aac-lc-8k.m4a`・`aac-lc-96k.m4a` | 各 2 s、`-aac_pns 0` | rate の両端 |
| `aac-adts.ts` | `aac-lc-stereo-44k` の音を TS に（`-c:a copy -f mpegts`） | ADTS の入力（§3.1） |
| `h264-<名>.mp4`（WS083 の 6 本と同じ素材・同じ x264 の引数） | x264 で**直に mp4 へ** encode（`-fps_mode passthrough`） | probe と同じ picture の情報（Annex B は同じ mp4 から `-c copy -bsf:v h264_mp4toannexb -f h264` で取り出す） |
| `h264-high-b-aac.mp4` | 640x360、5 s、High、`bframes=3:b-pyramid=normal:open-gop=1:keyint=30:min-keyint=30:scenecut=0`、AAC | 表示順、open GOP の seek、A/V |
| `h264-main-crop-sar.mp4` | 1920x1088 → crop で 1080、`setsar=4/3`、VUI の colour 709・full range | crop、SAR、色 |
| `h264-baseline-small.mp4` | 176x144、Constrained Baseline、POC type 2 | 深さ 0 |
| `h264-nocts.mp4` | `h264-high-b-aac` の video を ctts 無しにした物（raw の Annex B から `-r 25 -c copy`、§15 E1 の作り方） | H-04 の回帰（POC の表示順） |
| `h264.mkv`・`h264.ts`・`h264.avi` | `h264-high-b-aac` を各 container に `-c copy` | §3.1 の回帰（host の codec の試験） |

- 何度走らせても同じ bytes になるように `-fflags +bitexact -flags:v +bitexact -flags:a +bitexact -map_metadata -1`、x264 は `threads=1`。
- ffmpeg の AAC の encoder は PNS・intensity・TNS が**既定で有効**（`ffmpeg -h encoder=aac`、§15 E5）。波形を比べる stream は明示に切る。
- 参照（第 2 版で直した、H-03・M-05・M-10）:
  - video: mp4 から `ffmpeg -i X.mp4 -fps_mode passthrough -pix_fmt nv12 -f framehash -hash sha256 -`（WS083 の `make-streams.sh` と同じ計算）→ `X.sha256`。frame の数を
    `ffprobe -count_frames` の `nb_read_frames` と照らす。
  - audio の float の参照は tree に入れず、host 試験が実行の時に `ffmpeg -c:a aac -i X -c:a pcm_f32le -f f32le -`（decoder の指定は `-i` の前）で `build/` に作る。
  - audio の target の参照（`X.rms`）は p011 で、**自前の decoder を host で走らせた** 16 bit・48 kHz・stereo の出力から作る（§10.3）。
- p002 の確認に「B のある全 mp4 の video の track に ctts がある」「open GOP の stream の stss に非 IDR の I がある」を入れる。

### 10.2 host 試験（`plan/ws202/tests/`）

| script | 確かめる物 | 基準 |
| --- | --- | --- |
| `run-host-bits.sh` | `bits.c` | 手で作った bit 列の期待と一致 |
| `run-host-picture.sh` | scaler（601・709・2020、limited・full、1:1・縮小・拡大）、pool（ring 8＋window 2＋深さ 16 の模擬で尽きない、上限を越えた返却は free） | 式との差 ≦ 1、ASan、時間の表示 |
| `run-host-sound.sh` | downmix の係数、resample（1・10・18 kHz の sine と sweep の 44.1 → 48 kHz、48 → 48 の素通り）、切り詰めの sample の数、D23（receive の後 sound を呼ばずに次へ: 残りが捨てられ履歴が空になる）、trim | 通過域 ±0.1 dB、1・10・18 kHz の SNR ≧ 80 dB、画像の成分 ≦ −80 dB、数が一致 |
| `run-host-mediafile.sh`（`plan/tools/media/`） | pasp・colr（nclx・nclc）・end_us | 期待 |
| `run-host-aac-parse.sh` | 全 AAC の stream の全 frame の parse、bit の数え、道具の数え、HE-AAC の書き換えの ASC（§6.6）、ADTS | 0 failures |
| `run-host-aac.sh` | 自前の decoder の float（core の rate、channel ごと、**切り詰めの後** — ffmpeg の mov の reader と同じく先頭だけ切り、末尾の切り詰めは比べる時に外す）を host の ffmpeg と比べる。5.1 は ffmpeg の出力の順（FL FR FC LFE BL BR）と自前の（C、L、R、Ls、Rs、LFE）を対応させる | PNS・intensity の無い stream: 各 channel で max \|差\| ≦ 2^-14、RMS(差) ≦ 2^-17（U6）、sample の数が一致。`aac-is-pns`: 両方の出力を同じ MDCT（frame の格子に揃えた自前の解析）で scalefactor band に分け、自前の parse で分かる PNS の band は energy の比が ±1 dB、それ以外の band（intensity を含む）は差の energy が band の energy の −80 dB 以下（無音の band は絶対の下限）。M/S を noise・intensity の band に誤って掛けるとここで外れる。EIGHT_SHORT を含む frame は band の比べから外し、frame の energy の比 ±1 dB だけ。乱れの 1000 通りで無事。2 thread の同時の open（TSan、§6.9） |
| `run-host-h264.sh` | `h264.c`・`h264-dpb.c` を mp4 の stream に通し、picture ごとの `StdVideoDecodeH264PictureInfo`・slice・DPB の計画を、**同じ mp4 から取り出した Annex B** を probe の `h264.c`・`dpb.c` に通した結果と比べる（probe が扱う範囲だけ）。probe の外: POC type 1（手で作った bit 列）、frame_num の gap（`h264-high-b-aac` の途中の非 IDR の I から始め leading を捨てた時に DPB が保たれ、後の P・B の参照の list が ffmpeg の decode の順と POC から作る期待と一致）、MMCO 5（J4 の stream があればそれ、無ければ手で作った slice header の列）、VUI、表示順（POC の bumping の出る順が ffmpeg の表示順と一致、`h264-nocts` を含む） | 一致 |
| `run-host-vkvideo.sh` | 偽の Vulkan の関数の表で: instance の apiVersion が 1.0・properties2 の拡張が有効、作る object の順と引数、RESET、parameters の作り直し、D18（空の slot を参照に積まない）、seek の後の捨て方、表示順と時刻（packet の pts の整列の当て方）、de-tile と crop、skip の扱い、DEVICE_LOST の後の作り直し（instance も新しい） | 期待の呼び出しの列と一致、ASan |
| `run-host-codec.sh`（`plan/tools/media/`） | 既存の全 file ＋ `aac-adts.ts`・`h264.{mkv,ts,avi}`・`h264-nocts.mp4`: AAC は自前、H.264 は host では DEVICE → add-in。TS の AAC が鳴る（回帰しない） | 既存の PASS を保つ、新しい試料も PASS |

偽の Vulkan は本番の dlopen・`vkGetInstanceProcAddr` の道を通らない（coding-style §12 の「Tests must exercise the default production path」の限界）。この道と本当の decode は
p010 の終わりの 5330 の小さい確認（M-09）で確かめる。

### 10.3 試験の道具 `media-probe`（`userland/tests/media-probe/`）

- video の部分（p010）: `media-probe --video-hash [--expect=FILE] [--seek=S] [--time] FILE`。表示順の frame ごとの NV12 の SHA-256（crop の窓、WS083 の参照と同じ計算、
  `userland/base/common/sha256.c` を使う）。1 行目に `media-probe: video=h264/vulkan-video`。`--time` で decode・de-tile の平均・最大と open の時間。
- audio の部分（p011、M-10 の決定）: `--audio-rms`。`media_decoder_sound` の普通の口で 16 bit・48 kHz・stereo を取り、1024 frame ごとの左右の RMS を出す。参照は同じ
  libmedia の source を host で compile した `host-media-rms.c` の出力（`X.rms`、tree に入れる小さい text）。比べ: RMS が 1e-4（−80 dBFS）未満の frame は絶対差 ≦ 1e-6、
  他は相対差 ≦ 1e-3（target の clang と host の gcc の浮動小数の違いを許す）。精度そのものは host の `run-host-aac.sh` で確かめる。
- test の image にだけ入れる。

### 10.4 試験の image と QEMU・5330（第 2 版で直した、H-07）

- image の構成は 1 つ: `plan/ws202/tests/config-media.mk` = `include config/ci/config-amd64.mk` と
  `ZEDBSD_USER_PROGRAMS := $(filter-out libavcodec,$(ZEDBSD_USER_PROGRAMS)) media-probe`。CI の構成は i915・openssh・libmedia・videoplayer・music・audiod・
  graphical login を持つので、QEMU と 5330 を兼ねる。passthrough（`plan/ws075/tests/config-test-hw.mk`）は使わない。
- stream の置き方: `plan/tools/guest/test-image.sh` は `ZEDBSD_TEST_EXTRA_FILES` を make の command line で渡すので、config の `+=` は**消える**（GNU make の command line の
  変数が makefile の代入に勝つ。§15 E7 で確かめた）。stream は T1 の依頼の `test-image.sh` の `--file /usr/share/zedbsd-tests/ws202/<名>=plan/ws202/tests/streams/<名>` で
  渡す（kei が読める場所。その path が image で読めることは p010 の最初の T1 で確かめる、U12）。
- 5330: WS083 p005 と同じく、USB で起動した実機に SSH。
- p010 の終わり（M-09）: T1 に小さい依頼「5330 で `media-probe --video-hash --expect` を WS083 の 6 本の mp4 と `h264-high-b-aac`（+`--seek=2.5`）」。
- p012: QEMU（libavcodec 無し: AAC の RMS、Music の scenario、DEVICE の notice。libavcodec 有り: Video Player の回帰）と 5330（全 h264 の stream、1080p、Video Player の
  再生と late の数、compositor の fps、Music）。
- 1080p の試料（J5）: 推しは make-streams.sh の `--large` が合成の 1080p（testsrc2・mandelbrot、x264 の Baseline・Main・High、10 s、AAC 付き）を `build/` に作り、
  T1 が scp で 5330 に送る（image の入力ではない）。参照も同じ script が作る。`/home/awe/zedbsd-media/` の sample は UAT だけに使う。
- 受け入れ（M-07）: 5330 で 1080p30 の再生中の compositor の fps（log）が、再生していない時の 90% 以上。満たさない時は Q1 がユーザーに示す（decode の非同期化・
  batch は WS083 の側の別の WS）。

### 10.5 証拠を分けて書く

host・QEMU・5330 の結果を別の行に書く。やっていない確認は「未実施」。

## 11. license

- 新しい code は自前（Zlib）。vkvideo-probe の code は写してよい（D2）。
- FFmpeg（LGPL）の code は写さない。参照は (1) AAC の表の値（H5 (a) の時）、(2) host の ffmpeg の program の実行。
- 試験の stream は合成の素材から（第三者の著作物を含まない）。
- 特許（J3）: AAC・H.264 の decoder を base の libmedia に入れることになる。今の release は同じ codec の decoder を持つ libavcodec を既に配っている。

## 12. Phase の分け方

[ws.md](ws.md) の表。AAC の列（p005→p006→p007）と H.264 の列（p008→p015→p009→p010）は p003 の後は独立。共有の file の merge の順は ws.md（L-13）。

## 13. 人の判断の点（推し付き）

| ID | 問い | 推し | 理由 |
| --- | --- | --- | --- |
| H1 | Vulkan Video の無い機械の H.264 | (a) notice、libavcodec があればそれ。CPU の decoder は別の WS | CPU の decoder は 45〜60 LW で性能が不明。Windows の QEMU の配布物は libavcodec を入れている |
| H2 | libavcodec の入った build | (a) 両方、自前が先、release は libavcodec のまま | HEVC・VP9・MP3・Opus と QEMU の H.264 を保つ |
| H3 | HE-AAC | (a) core だけ鳴らす（2 段目だけ）、libavcodec があればそちら | .m4a の大半は LC。host で HE-AAC を作れない |
| H4 | ベータ2 に入れるか | 入れない（ベータ3） | 75 LW と review・T1・UAT で RC（10/13）に間に合わない |
| H5 | AAC の表の出典 | (a) FFmpeg 9.0.2 の tarball から値だけを生成 | 値は規格の事実。aactab.c は data として読める（§15 E6）。WS141 と同じ扱いの許可が要る |
| H6 | GPU の image の共有の表示 | 今は CPU | 出力の image は SAMPLED を持たない（WS083 HD5） |
| J1 | 自前の back end を全 container に使うか（H2 (a) の時） | 全 container（mp4・mkv・TS・AVI）に使い、§10.2 の回帰の試験（`run-host-codec.sh` の TS・mkv・AVI・ctts 無し）を受け入れの条件にする | 第 2 版で入力の形（§3.1）と表示順（§5.6）を直し、回帰の原因を取り除いた。絞ると TS の AAC 等が libavcodec 無しで鳴らない |
| J2 | browser の engine も自前の H.264 を使うか | 使う（特別の扱いをしない） | 同じ道で code が増えない。browser は止めているので UAT の外。video の context を 1 つ使うだけ |
| J3 | AAC・H.264 の decoder を base の libmedia に入れる特許の扱い | 進める。release の文書に「AAC と H.264 の decoder を含む」と書く | 今の release は同じ decoder を持つ libavcodec を既に配っており、危険は増えない。法律の判断はユーザー |
| J4 | MMCO 5・frame_num の gap・冗長 slice の試験の stream の出典 | ITU-T H.264.1 の conformance の bitstream を tree の外で使う（script が取得して SHA-256 を確かめ、`build/` に置く。tree に入れない）。host の parser・DPB の試験と 5330 の hash（conformance の package の yuv の md5 と比べる） | 手で作る bit 列は slice の data が無く decode の確かめにならない。conformance の stream の配布の条件は p015 で読む（U13） |
| J5 | 1080p の試料と参照 | make-streams.sh が合成の 1080p を `build/` に作り、T1 が scp。参照も同じ script | tree の外の sample と記録の無い参照に完了の条件を頼らない。sample は UAT に |
| J6 | 再生の途中の DEVICE_LOST | その file の再生を失敗にする（notice）。libavcodec への切り替えはしない | 切り替えは次の IDR からの再開と時計の合わせが要り（+2 LW）、hang は稀 |

## 14. 確かめていない物

| ID | 内容 | どこで |
| --- | --- | --- |
| U1 | fragmented の大きな file の open の時間 | p004（100〜200 MB の合成の file を担当の `build/` に、外挿） |
| U2 | 1080p の de-tile の時間 | p010 の 5330 |
| U3 | CPU の H.264 decoder の性能（H1 (b) の時だけ） | — |
| U4 | AAC の decode の時間 | p006 の host、p012 の 5330 |
| U5 | scaler の時間 | p003 の host、p012 の 5330 |
| U6 | AAC の精度の基準が ISO/IEC 14496-26 の基準と同じか、ISO/IEC 14496-3 の表の番号 | 規格を読める時に。読めなければ §10.2 の値を WS の基準とする |
| U7 | libvulkan の soname と dlopen の名 | p009 |
| U8 | decode と desktop の描画の同時の fps | p012 の 5330（受け入れ §10.4） |
| U9 | ASO（slice の順の入れ替え）を i915 の MFX が扱うか | 扱えなければ result status が ERROR。stream があれば p015 |
| U10 | 9 個目の process の video session の作成が何の Vulkan の結果を返すか | p009（i915 の `render/video.c`・`worker.c` を読む） |
| U11 | SPS の level_idc を `maxLevelIdc` に丸めることが規格の VUID に触れないか | p009（Vulkan の仕様を読む） |
| U12 | `/usr/share/zedbsd-tests/ws202/` が image で kei に読めるか | p010 の T1 |
| U13 | ITU-T H.264.1 の conformance の bitstream の入手先と利用の条件 | p015 |
| U14 | 1 つの kernel session の中の 2 つの video session（同時 2 本の動画）を WS083 が実機で試したか（設計は 1 つの context を共有、WS083 design §242） | 記録が無い。p012 の 5330 で media-probe を 2 つ同時に走らせる |
| U15 | ffmpeg の mov の reader が edts の末尾を切らないこと（§15 E4）が全ての場合に言えるか | p006 の試験の作り方で吸収（末尾は比べから外す） |

## 15. 第 2 版で host で確かめたこと（scratchpad の中、tree には書いていない）

- E1（review の E1 を受けて）: raw の Annex B を `-c copy` で mp4 にすると ctts が無い（review の確認）。代わりに x264 で直に mp4 にした
  `testsrc2 320x240 25 fps 6 s、bframes 3、b-pyramid normal、open-gop、keyint 30` の file は stbl に `ctts` があり、packet の pts と dts が違う。
- E2: `ffmpeg -c:a aac -f ipod` の .m4a の `elst` は version 0、1 entry、segment 1000（timescale 1000）、media_time 1024（review の確認と同じ）。
- E3: E1 の open GOP の file の `stss` は 1・30・60・90・120。NAL の数え（`trace_headers`）で nal_unit_type 5 は 1 つだけ、他の I は type 1（非 IDR）。
  frame_num は 0〜7 を回る。
- E4: 1.0 s・44100 sample の sine の .m4a を ffmpeg が decode すると 45056 sample（packet 45 個 × 1024 − priming 1024）。先頭は切るが末尾は切らない。
  `-c:a aac -i X -c:a pcm_f32le -f f32le -` の形で動く。
- E5: `ffmpeg -h encoder=aac` で `-aac_pns`・`-aac_is`・`-aac_tns` は既定で有効、`-aac_ms` は auto、`-aac_coder` は twoloop。
- E6: `build/distfiles/ffmpeg-9.0.2.tar.xz`（SHA-256 が package の Makefile と一致）の `libavcodec/aactab.c`（3903 行、LGPL-2.1+）に `codes1[81]`〜`codes11[289]`・
  `bits1`〜`bits11`、`ff_aac_scalefactor_code[121]`・`_bits[121]`、`swb_offset_1024_{96,64,48,32,24,16,8}`・`swb_offset_128_*`、`ff_aac_num_swb_1024`・`_128`、
  `ff_tns_max_bands_1024`・`_128` がある（tns の表の宣言は `aactab.h` 111〜114）。
- E7: `FILES := a` と `FILES += b` の makefile を `make FILES=c` で走らせると `[c]`（command line が makefile の代入と `+=` に勝つ）。
- E8: `ffmpeg … -ch_layout 5.1 -c:a aac` は動き、ffprobe の channel_layout は `5.1`（L-10）。
- E9: `include/libc/vulkan/vk_video/vulkan_video_codec_h264std.h` で `STD_VIDEO_H264_LEVEL_IDC_5_1 = 14`（review の M-03 の「15」は 5.2 の値）。i915 は level_idc を
  比べない（`render/video.c` で level の語は 132〜133・828〜829・1860 だけ）。

## 16. review-001 の指摘と直した場所

| 指摘 | 直した場所 |
| --- | --- |
| H-01 apiVersion | §1.4、§5.4 D9、§10.2 `run-host-vkvideo.sh`、p009 |
| H-02 MMCO 5・gap・seek の DPB・空の slot | §1.4、§3 D2、§5.2、§5.3、§5.7、D18、§10.2 `run-host-h264.sh`・`run-host-vkvideo.sh`、新 p015、J4 |
| H-03 mp4 の作り方 | §10.1（直に mp4、Annex B は mp4 から、framehash、ctts の確かめ）、§15 E1、p002 |
| H-04 表示順と時刻 | §1.1、§5.6 D22、§10.1 `h264-nocts.mp4`、§10.2、p010 |
| H-05 container の入力 | §1.2、§3.1（D16）、§6.1、§10.1（`aac-adts.ts`・`h264.{mkv,ts,avi}`）、§10.2 `run-host-codec.sh`、p006・p010、J1 |
| H-06 音の約束 | §1.1、§8.5 D23、§9.1、§10.2 `run-host-sound.sh`、p003・p006 |
| H-07 試験の image | §10.4（`config-media.mk`、`--file` は test-image.sh の引数、SSH の実機）、§15 E7、p010・p011・p012・p013 |
| M-01 共有の device | §1.4、§5.4 D9、§5.9、ws.md の依存（WS083 p007）、J6 |
| M-02 pool | §7.1 D11、§5.4 D21、§10.2 `run-host-picture.sh`、p003 |
| M-03 level | §1.4、§5.4 D20、§15 E9、U11、p009 |
| M-04 M/S の除外 | §6.3、§10.1（`aac-ms-tns`・`aac-is-pns`）、§10.2、p006 |
| M-05 AAC の精度の試験の合わせ方 | §4.1 D17、§6.5、§10.1、§10.2、§10.3、§15 E2・E4、p002・p006 |
| M-06 resampler | §6.5 D13、§10.2、p003 |
| M-07 描画の fps | §10.4 の受け入れ、U8、p012 |
| M-08 Music の AAT の helper | §1.3、§9.4、p007 |
| M-09 早い実機 | §10.3（media-probe の video を p010 へ）、§10.4、p010・p011 |
| M-10 media-probe の音の口 | §9.1、§10.1、§10.3、p002・p011 |
| M-11 HE-AAC の試験と用語 | §6.1、§6.6、§10.2、p005 |
| M-12 seek の後の音 | §8.2 D14（改）、§9.1 `media_decoder_trim`、p003・p006・p007・p011 |
| M-13 表の初期化 | §6.9、§10.2、p005・p006 |
| L-01 open の thread | §5.1、§9.3、p012 |
| L-02 dlclose | §5.4 D8 |
| L-03 VUI の誤り・nclc・SAR の順 | §4、§5.2 D19、§7.2 |
| L-04 log の互換 | §8.1、p011 |
| L-05 rate の引数 | §6.5 D13、p003 |
| L-06 video の context 8 個 | §1.4、§5.4、§9.1 `MEDIA_PROBLEM_BUSY`、§9.3、U10 |
| L-07 EAGAIN・ETIMEDOUT | §5.1、§5.6、§5.9 |
| L-08 冗長 slice・ASO | §5.1、§5.2、§5.8、U9 |
| L-09 U1 の file | §14 U1、p004 |
| L-10 `-ch_layout` | §10.1、§15 E8、p002 |
| L-11 試験の残し方 | ws.md の「WS の終わり」、p014 |
| L-13 merge の順 | ws.md の「並べて進める時の merge の順」 |

## 17. Future（WS202 の外）

- H.264 の CPU の decoder、HE-AAC の SBR・PS、GPU の image の共有の表示、TRANSFER_SRC の経路。
- vkvideo-probe と libmedia の parser の重複の解消（probe が MMCO 5・gap を扱えるようにもなる）。
- stsd の複数の entry、`iTunSMPB`、decode の非同期化（M-07 を満たさない時）、再生の途中の libavcodec への切り替え（J6 (b)）。
