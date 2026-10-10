# WS202 の設計: libavcodec なしの H.264＋AAC の mp4 と .m4a の再生（ws202-p001）

**第 1 版**（2026-10-10、設計の担当）。範囲は [ws.md](ws.md) とユーザーの 2026-10-11 の指示。実装は別のセッションが行う。
この文書の「事実」は main `c7aeea0c7` の file を読んだ物で、file を添える。「決定」は D 番号、「人の判断」は H 番号（§13）、
確かめていない物は U 番号（§14）。

## 0. 読んだ物と前提

- 規則: `AGENTS.md`（zedBSD の規則）、`plan/guardrail.md`、`plan/coding-style.md`（全文、ANSI C の基準、§12 試験だけの環境変数の禁止）、
  `plan/master-design-policy.md`。
- 手本: `plan/ws083/ws.md`・`design.md`・`phase004/phase.md`、`plan/ws199/ws.md`。
- libmedia: `userland/desktop/libmedia/`（`media.h`・`media-decoder.h`・`media-private.h`・`decoder.c`・`avcodec.c`・`avcodec-layout.h`・
  `bitstream.c`・`engine.c`・`exports.map`・`Makefile`）。
- 容器の reader: `userland/desktop/mediafile/`（`mediafile.h`・`mediafile-private.h`・`mp4.c`、他に mkv・ts・ogg・avi）。
- app: `userland/desktop/videoplayer/`（`videoplayer.h`・`main.c`・`media.c`・`audio.c`）、`userland/desktop/music/`（`play.h`・`play.c`・`main.c`）。
- Vulkan Video: `userland/desktop/libvulkan/video.c`、`userland/tests/vkvideo-probe/`（`main.c`・`h264.[ch]`・`dpb.[ch]`・`frame.[ch]`）、
  `docs/reference/vulkan-video.md`、`include/libc/vulkan/vulkan_video.h`・`vk_video/`。
- 外部 package: `userland/packages/multimedia/libavcodec/Makefile`（FFmpeg 9.0.2、LGPL、`--disable-hwaccels`）。
- 試験: `plan/tools/media/`（`run-host-mediafile.sh`・`run-host-codec.sh`）、`tests/scenarios/apps/videoplayer/`・`music/`、
  `plan/ws083/tests/`（`make-streams.sh`・`streams/`・`config-video-hw.mk`）、`/home/awe/zedbsd-media/`（tree の外の 1080p の試料、README.txt）。
- host の道具: ffmpeg 7.1.5（Debian 13）。encoder は `aac`（自前の AAC-LC）と `libx264`。**HE-AAC を作れる encoder（libfdk_aac）は無い**。
  host の Vulkan は llvmpipe だけで **Vulkan Video は無い**。

## 1. 今の形（事実）

### 1.1 libmedia

- 3 つの層: 容器の reader（`mediafile`、自前）→ decoder（`decoder.c` の back end の表）→ 再生の engine（`engine.c`、browser の `<video>` 用）。
  Video Player（`videoplayer/media.c`）と Music（`music/play.c`）は engine を使わず、`media_file_*` と `media_decoder_*` を直に使う。
- back end の表 `decoder_backends[]`（`decoder.c`）は今 `media_avcodec_ops` 1 つ。`struct media_decoder_ops`（`media-private.h`）は
  load・reason・open・decoder_name・send・receive・picture・sound・flush・close・picture_free・picture_size・picture_scale・scaler_free。
  表の順に open を試し、最初に受けた back end が decode する。誰も受けなければ**最初の back end の**問題（`MEDIA_PROBLEM_*`）を返す。
- `avcodec.c` は libavcodec・libavutil・libswscale を dlopen し、header 無しで呼ぶ。picture は AVFrame の参照で、表示は libswscale で
  BGRA へ縮小・拡大する（`media_frame_scale`）。音は `media_decoder_sound` で 16 bit stereo に線形補間で変換する（最初の 2 channel だけ）。
  video の picture の時刻は「送った packet の時刻のうち最小の物」を順に当てる。
- `bitstream.c` は avcC の長さ付きの NAL を Annex B（start code 付き）に、AAC の raw に ADTS の header を付ける（libavcodec に extradata を渡さないため）。
- `MEDIA_PROBLEM_MISSING`（libavcodec が無い）・`_VERSION`・`_FORMAT` の 3 つ。`exports.map` は `media_decoder_*`・`media_frame_*`・
  `media_codec_load`・`media_codec_reason` 等を出す。NEEDED は libc だけ。

### 1.2 容器の reader（mp4）

`mediafile/mp4.c` は**既にある**（WS122 p003、ws177-p027）。moov を丸ごと読み、track ごとに stsd（最初の entry だけ）・stts・ctts・stsc・
stsz/stz2・stco/co64・stss から sample の一覧（offset・size・時刻・sync）を作る。fragmented（mvex・moof・traf・tfhd・tfdt・trun・trex）も
open の時に全部読む。edts の最初の edit で pts をずらす（empty edit は遅延、最初の実の edit の media time を 0 に）。sample は file の offset の
順に渡す。seek は最初の video track の、時刻以前の最後の sync sample へ、他の track はその dts 以前の最後の sample へ。avcC は private data として
丸ごと、esds は decoder specific info（AudioSpecificConfig）を private data として持つ。`avc1`・`avc3`・`mp4a` を知る。

足りない物: pasp（画素の縦横比）・colr（色）は読まない。edts の最初の edit の**長さ**（表示の終わり）は持たない。stsd の 2 つ目以降の entry は読まない。

### 1.3 app

- Video Player: window の thread（`main.c`）と media の thread（`media.c`）。media の thread が読み・decode し、picture を 8 枚の ring に置き、
  音を `vp_audio_write`（libkeiland の `kl_audio_stream`、48 kHz・stereo・16 bit）に書く。時計は音（stream の再生位置）、音が無ければ monotonic。
  window は時刻の来た picture を `vp_media_take` で取り（遅れた物は捨てる）、`media_frame_scale` で canvas の BGRA の pixel に直に描く（CPU）。
  開けない時は `MEDIA_PROBLEM_*` から notice の文（「Playing video needs FFmpeg's libavcodec, which is not installed.」等）。
- Music: `play.c` の thread が sound の track を decode して `vp_audio_write`（videoplayer の `audio.c` を compile して共有）。
  `main.c` は起動の時に `media_codec_load()` を呼び、失敗すると「Playing needs libavcodec (the libavcodec package).」を出す。
- release の image（`config/release/config-amd64-beta2.mk`）は libavcodec と videoplayer を入れる。CI の構成も libavcodec を入れる。

### 1.4 Vulkan Video（WS083）

- zedBSD の libvulkan は i915（Gen12、VCS0・MFX）の上で `VK_KHR_video_queue`・`VK_KHR_video_decode_queue`・`VK_KHR_video_decode_h264`・
  `VK_KHR_synchronization2` を出す（2026-10-11 から既定で ON）。family 1 が video decode。device の apiVersion は 1.0（N1）。
- 範囲: Baseline（66）・Main（77）・High（100）、8 bit、4:2:0、progressive だけ、4096x4096・36864 MB・level 5.1、DPB 17・参照 16、
  DPB と出力は同じ image（COINCIDE）、slot ごとに 1 image、bitstream の offset は 32 byte 揃え。256 slice を超える picture は skip。
- 出力の picture は NV12（`VK_FORMAT_G8_B8R8_2PLANE_420_UNORM`）・OPTIMAL・usage は DECODE_DST と DPB **だけ**（SAMPLED・TRANSFER_SRC は無い、
  WS083 HD5）。読むには host visible の memory に bind し、`vkGetImageSubresourceLayout`（PLANE_0・PLANE_1）の offset と pitch で
  **Intel Tile Y**（128 byte × 32 行の tile、16 byte × 32 行の column）を de-tile する（N3、zedBSD の約束）。memory type は 1 つで
  DEVICE_LOCAL|HOST_VISIBLE|HOST_COHERENT|HOST_CACHED。
- skip された decode は result status query で ERROR（-1）。規則違反の submit は DEVICE_LOST。i915 の worker は 1 本で同期（submit が decode の
  終わりまで戻らない）。1080p の 1 decode は 4.2〜5.2 ms（5330、WS083 p008）。
- Venus（QEMU）・他の GPU には video の family が無い（`vkvideo-probe --list` は `video families 0`）。
- `vkvideo-probe` が唯一の利用者: Annex B の file を読み（`h264.c`: SPS・PPS・scaling list・slice header・POC type 0/2・MMCO）、DPB（`dpb.c`:
  slot の割り当て、sliding window・MMCO の marking、frame_num の gap）を持ち、picture ごとに 1 submit・fence を待ち、slot の image を
  de-tile して hash（`frame.c`）。表示順は IDR の間で POC で並べ替える（全部を貯める）。

## 2. 目標と範囲

- **目標 A**: libavcodec の無い image で、Video Player が H.264（Baseline・Main・High、8 bit 4:2:0 progressive）＋AAC-LC の mp4 を、
  音つき・同期・seek・終わりまで再生する。H.264 は Vulkan Video（5330 の i915）で decode する。
- **目標 B**: libavcodec の無い image で、Music が .m4a（AAC-LC）を libmedia の自前の decoder で再生する。
- 範囲の外: H.264 の CPU の decoder（H1）、HE-AAC の SBR・PS の再現（H3）、他の codec（HEVC・VP9・AV1・MP3・Opus・Vorbis 等は libavcodec の
  add-in のまま）、encode、interlaced、10 bit、AAC の Main・LTP・SSR・ER 系、DRM（encv・enca）、ネットワークの streaming（DASH・HLS）。

## 3. 全体の構成

```
mediafile (mp4.c)  ──packet──▶  decoder.c の back end の表（順に試す）
                                 1. vkvideo   H.264 → Vulkan Video（libvulkan を dlopen）→ de-tile → NV12 の picture
                                 2. aac       AAC-LC → 自前の decoder → float PCM
                                 3. avcodec   他の全て（libavcodec が入っていれば）
                               ◀──picture（NV12・自前の scaler で BGRA へ）／sound（16 bit stereo 48 kHz）
```

新しい file（`userland/desktop/libmedia/`、全部 Zlib、自前）:

| file | 中身 |
| --- | --- |
| `bits.c` | bit の reader（MSB から、`ue(v)`・`se(v)`、RBSP の emulation prevention の除去） |
| `picture.c` | 自前の back end の picture（NV12 linear、参照の数え、pool）と scaler（NV12 → BGRA、色の行列、bilinear、固定小数点） |
| `sound.c` | 音の出力の変換（channel の downmix、polyphase の resampler、16 bit への丸め、先頭・末尾の切り詰め） |
| `aac.h`・`aac.c` | AAC の back end（ops、AudioSpecificConfig、frame の構文、Huffman の復号） |
| `aac-tools.c` | 逆量子化、PNS、M/S、intensity、TNS |
| `aac-filterbank.c` | IMDCT（FFT による）、窓（sine・KBD）、窓の列、overlap-add |
| `aac-tables.c` | Huffman の codebook 12 個、scalefactor band の表（生成した表、§6.7） |
| `h264.h`・`h264.c` | SPS（VUI を含む）・PPS・slice header・POC（type 0・1・2）。vkvideo-probe の `h264.c` を元に access unit 単位へ |
| `h264-dpb.c` | DPB の slot の計画と marking（vkvideo-probe の `dpb.c` を元に）、表示順の深さ |
| `vkvideo.h`・`vkvideo.c` | Vulkan Video の back end（ops、session、parameters、decode、読み出し、表示順の並べ替え） |
| `vkvideo-device.c` | 共有の instance・device・queue（process に 1 つ）、関数の表（dlopen と `vkGet*ProcAddr`） |

D1: 既存の `struct media_decoder_ops` に back end を足す形にし、`media-decoder.h` の関数（app が使う口）はほぼ変えない（§9）。
engine.c（browser の `<video>`）・Video Player・Music は同じ表を通るので、全部が自前の decoder を使う。

D2: vkvideo-probe の `h264.c`・`dpb.c` は同じ project の Zlib の code なので、libmedia へ写して直す（probe は WS083 の試験の道具として今のまま
残す。重複の解消は Future、§15）。

## 4. mp4（ISO BMFF）の demuxer

D3: 新しく書かない。`mediafile/mp4.c` をそのまま使い、足りない 3 点だけ足す。.m4a も同じ reader（`mp4a`・esds）。

| 足す物 | 中身 | 使う所 |
| --- | --- | --- |
| pasp | visual sample entry の子の `pasp`（hSpacing・vSpacing）を `media_track` の `sar_num`・`sar_den` に（無ければ 0） | H.264 の VUI の SAR が無い時の縦横比 |
| colr | `colr` の `nclx`（primaries・transfer・matrix・full range）を `media_track` の `colour_matrix`・`full_range` に（無ければ未知） | VUI が無い時の色の行列 |
| 表示の終わり | edts の最初の実の edit の segment の長さ（movie timescale）から、表示の終わり `end_us`（0 は未知）を `media_track` に | AAC の末尾の padding の切り詰め（§6.5） |

- 範囲の判断（D4）: **fragmented mp4 は扱う**（既に open の時に全部読む実装がある。足す仕事は無い）。ただし ネットワークの段階的な読み
  （moof を順に読みながらの再生）はしない（file を開く時に全部の moof を読む今の形で、数百 MB の file は open が遅い。U1 で測る）。
- stsd の 2 つ目以降の entry（途中で解像度・codec が変わる file）は扱わない（最初の entry の track として読み、違う entry の sample は
  decoder が失敗して飛ばす）。限界として記す。
- `media_track` の field を足すのは libmedia の内部の ABI（SDK ではない）。videoplayer・music・libbrowser は同じ build で作り直る。

### 4.1 時刻（pts・dts）

- mp4.c が返す `pts_us`（ctts と edit の shift の後）と `dts_us` をそのまま使う。B frame のある H.264 は dts 順（file の順）に届き、pts で表示する。
- AAC の .m4a は edit の media time（encoder の遅延、普通 2112 sample）だけ pts が負から始まる。負の時刻の音は出さない（§6.5）。

## 5. H.264: Vulkan Video の back end

### 5.1 流れ

1. open（track）: codec が H.264 でなければ FORMAT（次の back end へ）。avcC を読み（`bitstream.c` の `bitstream_avcc` を流用）、中の SPS・PPS を
   parse する。SPS の範囲（§5.8）を外れれば PROFILE。共有の device（§5.4）が無い・video の family が無いなら DEVICE。
2. send（packet）: `media_bitstream_convert` で Annex B に（key frame の前に SPS・PPS が付く）。NAL を順に読み、SPS（7）・PPS（8）を更新、
   slice（1・5）を集めて 1 access unit = 1 picture にする（mp4 の 1 sample = 1 picture）。SEI（6）・AUD（9）・filler（12）・SPS ext（13）・
   prefix・SVC・MVC（14・15・20）は飛ばす。data partition（2〜4）は PROFILE。
3. 1 picture の decode: DPB の計画（§5.3）→ bitstream の buffer に slice を start code 付きで写す（offset を 32 byte に揃える）→
   command を記録（begin coding・必要なら RESET・status の query・decode・end）→ submit・fence を待つ → result status を読む →
   出力の slot の image を de-tile して NV12 の picture にする（§5.5）→ marking（§5.3）→ 表示順の待ち行列に pts とともに入れる（§5.6）。
4. receive: 待ち行列から表示順に 1 枚出す（深さを超えた時か、drain の時）。
5. flush（seek）: 待ち行列を捨て、DPB を空にし、次の decode を RESET にし、次の I（IDR か非 IDR の I）まで slice を捨てる（§5.7）。
6. close: queue を idle にし、session・parameters・image・buffer・memory・command pool・fence・query pool を壊す。共有の device の参照を 1 減らす。

D5: decode は media の thread（Video Player の `media.c`、engine の thread、Music は使わない）の上で同期に行う。i915 の submit は decode の
終わりまで戻らないので、非同期にしても得が無い（WS083 の事実）。1080p で 1 picture ≒ 5 ms＋de-tile。

### 5.2 parser（`h264.c`）

vkvideo-probe の `h264.c` を元に、次を変える・足す。

- 入力は access unit（1 packet の Annex B の bytes）。stream 全体を読む形をやめ、`struct h264_parser`（SPS・PPS の表、POC の状態）に
  1 AU ずつ渡し、`struct h264_picture`（`StdVideoDecodeH264PictureInfo`、slice の offset・size、slice type、marking）を返す。
- SPS: 今の物＋**VUI**（aspect_ratio_info → SAR、video_signal_type の video_full_range_flag・colour_description の matrix_coefficients、
  bitstream_restriction の max_num_reorder_frames・max_dec_frame_buffering）。frame_cropping は既にある（表示の窓）。
- POC: type 0・2 に **type 1** を足す（H.264 8.2.1.2、offset_for_ref_frame の表）。
- PPS: 今の物（scaling list を含む）。num_slice_groups_minus1 > 0（FMO）は PROFILE（Vulkan の StdVideo の PPS に slice group が無い）。
- 誤った NAL（範囲外の id、読めない ue(v)）はその AU を捨てて EINVAL を返す（engine・player は次の packet へ進む）。
- emulation prevention（00 00 03）の除去は `bits.c`。

D6: SPS・PPS の内容が変わった時（同じ id で違う bytes）は parameters の object を作り直す。新しい id が増えただけでも作り直す
（`vkUpdateVideoSessionParametersKHR` を使わない。稀な事象なので単純さを取る）。古い object は今の decode の fence の後に壊す。

### 5.3 DPB（`h264-dpb.c`）

- vkvideo-probe の `dpb.c`（slot の割り当て、短期・長期、sliding window、MMCO 1〜6、frame_num の gap の補い）を写す。
- slot の数 = `max_num_ref_frames + 1`（≦ 17）。D7: 出力は decode の直後に CPU の NV12 へ写す（§5.5）ので、slot は「参照に使われているか」
  だけで再利用でき、表示待ちのために slot を持ち続けない。
- 表示順の深さ（§5.6 の待ち行列の長さ）: VUI の `max_num_reorder_frames` があればそれ。無ければ、profile 66（Constrained Baseline、B が無い）は 0、
  他は level の MaxDpbMbs（H.264 表 A-1）÷ picture の MB 数（≦ 16）。

### 5.4 Vulkan の object（`vkvideo-device.c`・`vkvideo.c`）

D8: libvulkan は **dlopen**（`libvulkan.so`、soname は Phase で image の物を確かめる）し、`vkGetInstanceProcAddr`・`vkGetDeviceProcAddr` で
関数の表を埋める。libmedia の NEEDED は libc のまま（H.264 を再生しない program — Music・browser の大半 — は Vulkan を読み込まない）。

D9: instance・physical device・device・video の queue は **process に 1 つ**（`pthread_once` で最初の H.264 の open の時に作る、参照の数で
最後の decoder の close で壊す）。queue への submit は mutex で守る。decoder ごとに session・parameters・image・buffer・command pool・fence・
query pool を持つ（同時に 2 本の動画 — 例 browser の 2 つの `<video>` — も動く）。

- device の選び方: `VK_QUEUE_VIDEO_DECODE_BIT_KHR` の family があり、`videoCodecOperations` に H.264 decode がある最初の physical device。
  拡張 `VK_KHR_synchronization2`・`VK_KHR_video_queue`・`VK_KHR_video_decode_queue`・`VK_KHR_video_decode_h264` を有効にする。
- 「video の無い機械」の判定は 1 回だけ行い、結果を process の中で覚える（2 回目からは instance を作らずに DEVICE を返す）。
- capability: `vkGetPhysicalDeviceVideoCapabilitiesKHR` を SPS の profile（stdProfileIdc、PROGRESSIVE）で問い、maxCodedExtent・maxLevelIdc・
  maxDpbSlots・maxActiveReferencePictures と比べる。外れれば PROFILE。
- session: maxCodedExtent = SPS の coded の大きさ（MB に切り上げ）、maxDpbSlots = slot の数、maxActiveReferencePictures = max_num_ref_frames、
  format は NV12。session の memory は `vkGetVideoSessionMemoryRequirementsKHR` の通りに bind。
- image: slot ごとに NV12・OPTIMAL・usage DST|DPB・profile list を chain、HOST_VISIBLE の memory に bind して map したまま（probe と同じ）。
- bitstream の buffer: usage `VIDEO_DECODE_SRC`、最初は 1 MiB、大きい AU で 2 倍ずつ（最大 `mediafile` の 64 MiB）。map したまま。
- result status: family の `queryResultStatusSupport` が TRUE なら query pool を作り、decode ごとに 1 query。
- SPS の大きさ・profile・参照の数が変わった時（新しい IDR の SPS）: queue を idle にし、session・image を作り直す。

### 5.5 decode した picture の表示: CPU への写し（D10）

- decode の fence の後、出力の slot の image の PLANE_0・PLANE_1 を `vkGetImageSubresourceLayout` の offset・pitch で読み、Tile Y を de-tile して
  **linear の NV12**（crop の窓だけ、§5.2 の frame_cropping）を picture の buffer に写す。de-tile は probe の `frame.c` の
  `frame_tile_y_offset` と同じ式（16 byte の column の単位で写す）。
- picture の buffer は decoder ごとの pool（D11、§7.1）から取る。window が持っている間は pool に戻らない（参照の数）。
- 表示は今と同じく window の thread が `media_frame_scale` で canvas の BGRA に描く。scaler は libswscale の代わりに自前（§7.2）。
- 理由: decode の picture は SAMPLED・TRANSFER_SRC を持たない（WS083 HD5）。Video Player は canvas（CPU の pixel）に描く。GPU の image を
  共有して描くには libvulkan・i915 の側（SAMPLED、ycbcr の変換）と libkeiland の描画の両方の変更が要る。CPU の写しは 1080p で
  約 3 MB の読み（HOST_CACHED の memory）で、de-tile ≒ 1〜2 ms の見込み（U2）。
- 制限: de-tile は zedBSD の libvulkan の約束（OPTIMAL の NV12 の subresource layout が Tile Y を指す）に頼る。他の Vulkan の実装（Mesa 等）では
  画素が化ける。libmedia は zedBSD の build だけなので今は問題ない。他の OS へ持って行く時は TRANSFER_SRC と buffer への copy の経路が要る（Future）。
- H6（人の判断）: 将来 GPU で描く経路を足すか（推し: 今は CPU、性能が足りなければ別の WS）。

### 5.6 表示順と時刻（pts）

- picture には decode した AU の packet の `pts_us` を付ける（container の時刻を正とする）。
- 表示順の待ち行列（最大 17）: pts の小さい順に並べ、長さが §5.3 の深さを超えたら一番小さい pts を 1 枚出す。drain（packet NULL）で全部出す。
- 同じ pts が 2 枚来たら（壊れた file）後の物を捨てる。pts が無い（容器が 0 を返す）場合は POC の順と frame の長さから時刻を作る（mp4 では起きない）。

### 5.7 seek と先頭の picture

- flush の後は、次の I picture（IDR、または全 slice が I の非 IDR の picture）まで AU を捨てる（`mp4_seek` は stss の sync sample に
  移すので、普通は最初の AU が I）。
- 非 IDR の I から始めた時（open GOP）: DPB は空から始め、その I を最初の参照とする（frame_num の gap の補いは使わない）。その I より pts が
  小さい後続の B（leading picture、前の GOP を参照する）は decode せずに捨てる。参照の slot が DPB に無い P・B も decode せずに捨てる
  （log に数える）。
- 表示の側の「seek した時刻より前の picture は出さない」（`skip_before`）は今の player・engine のまま。

### 5.8 扱う範囲（profile・B frame・reorder）

| 項目 | 扱い |
| --- | --- |
| profile | 66（Constrained Baseline・Baseline、FMO・ASO・冗長 slice の無い物）、77（Main）、100（High）。他（High 10・4:2:2・4:4:4・Extended・SVC・MVC）は PROFILE |
| 形 | 8 bit、4:2:0、frame_mbs_only_flag = 1（progressive）。interlaced（PAFF・MBAFF・field）は PROFILE |
| 大きさ・level | capability の maxCodedExtent（4096x4096）・maxLevelIdc（5.1）まで |
| entropy | CAVLC・CABAC（hardware が扱う） |
| B frame | 扱う（B pyramid を含む、WS083 で実機の hash が一致） |
| 参照 | 短期・長期、MMCO、frame_num の gap、weighted prediction（hardware）、scaling list（SPS・PPS） |
| slice | 1 picture に 256 まで（越えると skip、§5.9） |
| reorder | §5.6（最大 16） |

### 5.9 失敗の扱い

| 事象 | 扱い |
| --- | --- |
| result status が ERROR（decode が skip された） | その picture を出さない。log `vkvideo: picture skipped`（最初の 8 回と、以後 100 回ごとの数）。連続 30 枚で decoder を失敗（EIO）にし、player は FAILED（「The video could not be decoded.」） |
| submit が DEVICE_LOST（hang、規則違反） | decoder を失敗にし、共有の device を「壊れた」と印す。最後の decoder の close で壊し、次の open で作り直す（i915 は engine を reset して video を再び出す、最大 3 回） |
| memory の不足 | ENOMEM、player は FAILED |
| 参照の無い P・B（seek の直後、壊れた file） | decode せず捨てる（§5.7） |

### 5.10 Vulkan Video の無い機械（Venus の QEMU、他の GPU）

- vkvideo は DEVICE を返し、表の次の back end（libavcodec の add-in）が入っていればそれが H.264 を decode する。libavcodec も無ければ
  player は「This computer's GPU cannot decode H.264 video, and FFmpeg's libavcodec is not installed.」を出す（音だけは鳴らさない、§8.4）。
- **H1（人の判断）**: Vulkan Video の無い機械で H.264 をどうするか。
  - (a) **推し**: 上の通り（エラーの表示、libavcodec があればそれ）。CPU の decoder は別の WS（Future に登録）。
  - (b) WS202 に CPU の H.264 decoder を足す（CAVLC・CABAC・intra・inter・変換・deblocking・weighted・direct。約 45〜60 LW、SIMD 無しの C で
    1080p30 に届くかは不明（U3）。ITU-T H.264 は無料で読めるので表の出典の問題は無い）。
  - 推しの理由: Windows の QEMU/Venus の配布物（ベータの配布物）は libavcodec を入れているので H.264 は今まで通り再生できる。CPU の decoder は
    WS202 の残り全部より大きく、性能の目処が立たない。

## 6. AAC: 自前の decoder

### 6.1 AudioSpecificConfig（esds の decoder specific info）

- audioObjectType（5 bit、31 は escape で 32＋6 bit）、samplingFrequencyIndex（15 は 24 bit の明示）、channelConfiguration。
- AOT 2（LC）: GASpecificConfig の frameLengthFlag は 0（1024）でなければ PROFILE（960 は DAB 等）。dependsOnCoreCoder は 0。
  channelConfiguration 0 の時は ASC の中の PCE で channel を決める。
- AOT 5（SBR）・29（PS）の明示の signalling: extensionSamplingFrequency と中の AOT（2 でなければ PROFILE）を読む。暗黙の signalling
  （GASpecificConfig の後の syncExtensionType 0x2b7）も読む。扱いは §6.6。
- AOT 1（Main）・3（SSR）・4（LTP）・17 以上（ER 系）・他は PROFILE（libavcodec があればそちら）。
- private data が無い（esds が無い）track は、mp4 の sample entry の rate・channels から LC と見なす（ADTS ではない raw の AU）。

### 6.2 frame の構文（raw_data_block）

ISO/IEC 14496-3 の 4.4.2 の構文を読む。

- 要素: SCE（0）・CPE（1）・CCE（2）・LFE（3）・DSE（4）・PCE（5）・FIL（6）・END（7）。
  - DSE・FIL は長さの分を飛ばす（FIL の SBR の拡張は §6.6）。
  - PCE は channel の並びを更新する（matrix_mixdown_idx も読む、§6.5）。
  - CCE（channel coupling）: 構文を全部読んで捨てる（適用しない、log に 1 回）。LC で使う encoder は稀。
- ICS: global_gain、ics_info（window_sequence・window_shape・max_sfb・scale_factor_grouping、predictor_data_present は 0 でなければ失敗）、
  section_data（codebook と区間）、scale_factor_data（scalefactor・intensity の position・PNS の energy、各 DPCM と scalefactor の Huffman）、
  pulse_data（long の時だけ）、tns_data、gain_control_data_present（1 なら失敗）、spectral_data（codebook 1〜11、ESC の 11）。
- CPE: common_window、ms_mask_present（0・1・2）と ms_used。
- 1 packet が 1 raw_data_block（mp4 の 1 sample = 1024 sample の frame）。読み終わりが packet の長さと合わなければ log（frame は使う）。

### 6.3 道具（`aac-tools.c`）

spec の decode の順に従う。

1. spectral の Huffman の復号（unsigned の codebook の符号の bit、ESC の escape）。
2. pulse data を量子化値に足す。
3. 逆量子化: `sign(q) · |q|^(4/3)`（|q| ≦ 8191、表は init の時に計算）× `2^(0.25 · (sf − 100))`。
4. PNS（perceptual noise substitution、NOISE_HCB）: 一様乱数（自前の LCG）の band を energy に合わせる。CPE で ms_used の band は同じ雑音
   （相関）を左右に。
5. M/S（ms_used の band で L = M + S、R = M − S）。
6. intensity stereo（INTENSITY_HCB・HCB2、position と ms_used の符号）。
7. TNS: 各窓の filter（order ≦ 12 の long、≦ 7 の short、係数の逆量子化 3・4 bit、上向き・下向き）を spectral に掛ける。
8. filterbank（§6.4）。

### 6.4 filterbank（`aac-filterbank.c`）

- IMDCT: N = 2048（long）・256（short）。N/4 点の複素 FFT（radix-2/4 の自前、twiddle は init で計算）による。
- 窓: sine と KBD（α = 4 の long、6 の short）。KBD は Bessel の I0 の級数で init の時に計算（表を持たない）。
- 窓の列: ONLY_LONG・LONG_START・EIGHT_SHORT・LONG_STOP。前の frame の窓の形（window_shape の前の値）を左の半分に使う。
- overlap-add: channel ごとに 1024 sample の前の半分を持つ。
- 計算は float（`float`、累積は `double` が要る所だけ）。

### 6.5 出力（`sound.c`）

- decoder の出力: channel ごと 1024 sample の float（±1.0 = 16 bit の ±32768）。
- 先頭の切り詰め（D12）: packet の pts が負の frame は、時刻 0 より前の sample を落とす（sample の単位、edit の media time の 2112 等）。
- 末尾の切り詰め: `media_track.end_us`（§4）があれば、それより後の sample を落とす。
- downmix（stereo へ）: mono は左右に同じ。stereo はそのまま。3〜8 channel は PCE の matrix_mixdown があればその係数、無ければ
  ITU-R BS.775 の係数（C と Ls・Rs に 0.7071）で、和の最大が 1 を越えないように正規化。LFE は捨てる。
- resample（D13）: core の rate（8〜96 kHz）から 48 kHz（`vp_audio` の固定の rate）へ、**polyphase の windowed sinc**（32 tap、Kaiser の窓、
  位相 256 の表は init で計算）。今の add-in の線形補間は 44.1 kHz の音楽で折り返しの雑音が出るため、自前の back end は sinc にする。
  avcodec の add-in の変換は変えない。
- 16 bit への丸め（四捨五入と飽和）。dither はしない。
- `media_decoder_sound`（capacity・rate を受ける）の約束は今の add-in と同じ: 受け取った frame の分を変換して返す（capacity を越える分は
  次の呼び出しに持ち越す。add-in のように捨てない）。

### 6.6 HE-AAC（SBR・PS）

- **H3（人の判断）**:
  - (a) **推し**: SBR・PS は再現せず、**core の AAC-LC だけ**を decode して鳴らす（帯域が半分で高音が欠ける。log `aac: playing the AAC-LC core of HE-AAC`）。
    ただし libavcodec が入っていれば HE-AAC はそちらに回す（表の 2 段の試し、§9.1）。
  - (b) SBR（約 14 LW）と PS（約 7 LW）を WS202 に足す。
  - (c) HE-AAC は PROFILE で断る（libavcodec が無ければ鳴らない）。
  - 推しの理由: .m4a の大半（iTunes・ffmpeg の既定・手元の CD の取り込み）は AAC-LC。HE-AAC は低い bitrate の配信の物。host の ffmpeg は
    HE-AAC を作れない（libfdk_aac が無い）ので試験の stream を手元で作れず、外の stream の license の確認が要る。core だけでも曲は判別できる。
- 暗黙の signalling（ASC に書かれず FIL の中の SBR の extension だけ）の stream: core の rate で LC として鳴らす（(a) と同じ）。

### 6.7 表と license

- 計算で作る表（出典の問題が無い）: `|q|^(4/3)`、`2^(0.25·x)`、sine・KBD の窓、FFT の twiddle、resample の sinc。
- spec の data の表: Huffman の codebook 1〜11 と scalefactor の codebook（4.A.1〜4.A.12）、scalefactor band の offset（rate ごと、long・short）、
  TNS の max band、sampling frequency の表。
- **H5（人の判断）**: data の表の出典（2026-10-07 のユーザーの決定「独自の AAC-LC は後の Phase で表の出典を決める」の判断）。
  - (a) **推し**: 表の値は規格の事実として、host の script（`plan/ws202/tests/gen-aac-tables.py`）が FFmpeg 9.0.2 の tarball（package の build が
    SHA-256 を確かめて取る物）の `libavcodec/aactab.c` から**値だけ**を読み、zedBSD の形（名前・並び・型は自前）で `aac-tables.c` を生成する。
    生成した file の頭に出典（規格の表の番号、読んだ file と SHA-256）を書く。code は写さない。WS141 の「値は事実として使ってよい」の決定と同じ
    扱いを WS202 にも認めてもらう。host の試験は生成した表と ffmpeg の decode の結果の一致で表の誤りを捕まえる。
  - (b) ユーザーが ISO/IEC 14496-3（有償）を用意し、規格の本文から表を起こす（最も clean、入手の手間）。
  - (c) 別の寛容な license の実装（Apache-2.0 の Android の旧 PV の AAC decoder 等）から値を取る（license の表示が要る）。
- 参照する仕様の名（code・文書の comment に書く）: ISO/IEC 14496-3（MPEG-4 Audio、AAC-LC）、ISO/IEC 13818-7（MPEG-2 AAC）、ISO/IEC 14496-14
  （MP4）・14496-12（ISO BMFF）・14496-15（avcC）、ITU-T H.264（2021-08）、Khronos Vulkan 1.3 の video の章と `vk_video` の header、ITU-R BS.775
  （downmix）、ITU-R BT.601・BT.709・BT.2020（色の行列）。

### 6.8 性能

AAC-LC の 48 kHz stereo の decode は 1 秒あたり 94 frame。float の FFT で 1 frame ≒ 30〜60 µs の見込み（5330 の 1 core の 1% 未満、U4）。
最適化（SIMD）はしない。

## 7. 共通の部品

### 7.1 picture（`picture.c`）

- `struct media_picture`（libmedia の内部）: 表示の大きさ（crop の後）、SAR、色（行列 601・709・2020、full range）、NV12 の 2 plane の pointer と pitch、
  参照の数、属する pool。
- pool（D11）: decoder ごとに同じ大きさの buffer を最大 「表示の深さ＋player の ring（8）＋2」 枚まで再利用。decoder が close した後も、window が
  持っている picture は使える（pool は参照の数で生き、最後の picture の free で消える）。参照の数は mutex（pool の）で守る。
- ops の picture・picture_free・picture_size・picture_scale を vkvideo の back end が `picture.c` の関数で埋める。

### 7.2 scaler（NV12 → BGRA）

- `media_frame_scale(frame, &scaler, pixels, stride, width, height)` の約束は今のまま（BGRA = canvas の 0xAARRGGBB、不透明）。
- 色: VUI → colr → 既定（高さ > 576 は BT.709、他は BT.601）の順で行列を選ぶ。limited range（16〜235）と full range。整数の固定小数点
  （係数 × 2^14）。
- 拡大・縮小: 縦横とも bilinear（16.16 の固定小数点、行ごとに source の行と重みの表を scaler に cache）。1:1 は補間しない速い道。
  chroma は輝度の位置に bilinear（chroma の位置は MPEG-2 の左寄せ）。
- 目標（U5、5330 で測る）: 1920x1080 → 1920x1080 が 8 ms 以下、→ 1280x720 が 10 ms 以下（1 thread、SIMD 無し）。超えるなら行を 2 thread に
  分ける（Phase の中の判断）。
- scaler の state は今と同じく caller（window）が持つ。

### 7.3 bit reader（`bits.c`）

MSB から読む reader（H.264 の RBSP、AAC の frame で共有）。`ue(v)`・`se(v)`、読み過ぎの印（範囲外は 0 を返して overrun を立てる。
呼び手は区切りで overrun を見る）。H.264 の NAL の emulation prevention の除去は別の関数（slice の header だけ、slice の data は hardware に生で渡す）。

## 8. A/V の同期・seek・終わり

### 8.1 同期

- 今の Video Player の形を保つ: 時計は音（`vp_audio_clock_position`）、音が無ければ monotonic。window は時刻の来た最新の picture を出し、
  遅れた picture は捨てる（`vp_media_take`）。decode は全部の picture を行う（参照が要るので飛ばさない）。
- picture の時刻は §5.6 の pts（表示順）。音の時刻は packet の pts（先頭の切り詰めの後）。
- decode が実時間に追いつかない時（大きな解像度、scaler の遅れ）: window は遅れた picture を飛ばし、log の `VIDEOPLAYER FRAMES shown=N` に
  `late=M`（飛ばした数）を足す（UAT の材料）。

### 8.2 seek

- 今のまま: `media_file_seek`（video の sync sample へ）→ 両 decoder の flush → `skip_before` より前の picture・音を出さない → 時計を新しい位置で
  合わせ直す。
- 音の `skip_before` は frame の単位（最大 21 ms 早く始まる）。自前の AAC は sample の単位で切ることもできるが、今の player の形を変えない（D14）。

### 8.3 終わり

- file の終わり（ENODATA）で両 decoder に NULL を送り（drain）、vkvideo は表示順の待ち行列を全部出し、AAC は末尾の切り詰めの後の音を出す。
- Video Player は今の通り `VIDEOPLAYER ENDED` を、Music は ring が鳴り終わってから曲の終わりを伝える。

### 8.4 一方だけ decode できる時

- video を decode できない（DEVICE・PROFILE）時: 今の player は開くのを失敗にする（video の track が要る）。この形を保ち、notice を出す（音だけの
  再生はしない。D15、UAT で要るなら後で）。
- 音を decode できない（HE-AAC を断る (c) 等）時: 今と同じく絵だけ再生する（`has_audio` 0）。

## 9. API と app の変え方

### 9.1 `media-decoder.h`・`decoder.c`

| 変更 | 中身 |
| --- | --- |
| 問題の追加 | `MEDIA_PROBLEM_DEVICE 4`（codec は自前の decoder が扱うが、この機械の GPU が decode できない）、`MEDIA_PROBLEM_PROFILE 5`（codec の profile・形を扱わない: interlaced・10 bit・HE-AAC を断る時等） |
| 問題の選び方 | 誰も受けない時: 自前の back end が DEVICE・PROFILE を言い、libavcodec が MISSING・VERSION ならその自前の問題。自前が FORMAT（codec を知らない）なら libavcodec の問題。全部 FORMAT なら FORMAT |
| 2 段の試し | back end の open に `degraded` の引数（ops の open の型を変える）。1 段目は全 back end を degraded = 0 で試し、誰も受けなければ 2 段目に degraded = 1 で試す。AAC の back end は HE-AAC を 2 段目だけで受ける（H3 の (a)）。avcodec は degraded を見ない |
| back end の名 | `const char *media_decoder_backend(const struct media_decoder *)`（"libmedia"・"vulkan-video"・"libavcodec"）。log と試験が読む |
| 縦横比 | `void media_frame_aspect(const struct media_frame *, int *num, int *den)`（SAR、未知は 1:1）。avcodec の picture は 1:1 のまま |
| 表 | `decoder_backends[] = { &media_vkvideo_ops, &media_aac_ops, &media_avcodec_ops }` |
| `media_codec_load`・`_reason` | 残す（libavcodec の add-in の状態を言う関数として）。app は起動の門に使わない |

`exports.map` は `media_decoder_*`・`media_frame_*` の wildcard で新しい関数も出る（変更無し）。

### 9.2 libavcodec の入った build（H2）

- **H2（人の判断）**: libavcodec の add-in を残すか。
  - (a) **推し**: 両方を持つ。表の順は自前（vkvideo・aac）が先、libavcodec は後ろの予備（他の codec、Vulkan Video の無い機械の H.264、
    HE-AAC の完全な再生）。release の image は今の通り libavcodec を入れる（Windows の QEMU の配布物で H.264 を再生するため）。
  - (b) libmedia だけにする（`avcodec.c` と libavcodec の package を release から外す）。HEVC（ユーザーの元の sample.mp4 は HEVC）・VP9・MP3・
    Opus 等は再生できなくなる。QEMU では H.264 も再生できない。
  - (c) 両方を持ち、libavcodec があればそちらを先にする（自前は libavcodec の無い image だけで働く）。自前の decoder が普段は使われず、試験が薄くなる。
- (a) の時でも、試験の image（§10）は libavcodec を**入れずに**作り、自前の経路だけを確かめる。

### 9.3 Video Player

- `vp_notice` に DEVICE（「This computer's GPU cannot decode H.264 video, and FFmpeg's libavcodec is not installed.」）と PROFILE（「This video's
  format (for example interlaced or 10-bit H.264) is not supported.」）の文。MISSING の文は「This video's codec needs FFmpeg's libavcodec, which
  is not installed.」に変える（H.264・AAC は要らないので）。
- 縦横比: `media_frame_aspect` を掛けて fitted の大きさを計算する（今は正方の画素と仮定）。
- log: `VIDEOPLAYER OPEN … video=<codec>/<backend> audio=<codec>/<backend>`（例 `video=h264/vulkan-video audio=aac/libmedia`）、
  `VIDEOPLAYER FRAMES shown=N late=M`。
- Makefile・comment の「FFmpeg が要る」の記述を直す。

### 9.4 Music

- `main.c` の起動の `media_codec_load()` の門（「Playing needs libavcodec」）を外す。曲を開けない時だけ、`mu_player_open` の問題から文を出す
  （MISSING は「This song's format needs FFmpeg's libavcodec, which is not installed.」、PROFILE は「This song's format is not supported.」）。
- log: `MUSIC PLAY open codec=aac backend=libmedia container=mp4 duration_ms=…`（今の `codec=aac` の行に `backend=` を足す。試験の scenario は
  `codec=aac` を読むので壊れない）。
- `play.h` の comment（codec.c・bitstream.c の古い記述）を直す。

### 9.5 engine（browser の `<video>`）

表を通るので自動で自前の decoder を使う。engine.c の変更は無い。browser（WS074、ベータ3 で止めている）の UAT は WS202 の範囲の外。

## 10. 試験

規則: 細かい修正ごとに回帰を回さない。実装の担当は build（warning 0）と変えた所の host 試験だけ。QEMU・実機は WS の最後に T1 へ 1 回で。
試験の image は `plan/ws202/tests/` の config.mk と `--file 宛先=元`（元は tree の中）だけで作る。

### 10.1 試験の stream（`plan/ws202/tests/make-streams.sh`、p002）

host の ffmpeg 7.1.5・libx264 で合成の素材（testsrc2・mandelbrot・sine・aevalsrc・anoisesrc）から作る。tree に入れるのは小さい物だけ
（合計 1 MiB 未満を目安）。版と引数を script の頭に書く。

| 名 | 中身 | 確かめる物 |
| --- | --- | --- |
| `aac-lc-stereo-44k.m4a` | 4 s、sine の sweep と和音、128 kb/s | 基本、priming（edts の 2112） |
| `aac-lc-mono-22k.m4a` | 3 s、mono 22.05 kHz、64 kb/s | mono、rate の表、resample |
| `aac-lc-51-48k.m4a` | 3 s、5.1（SCE・CPE・CPE・LFE）、320 kb/s | 多 channel、downmix、LFE |
| `aac-short.m4a` | 3 s、拍子木のような impulse（aevalsrc） | EIGHT_SHORT・START・STOP、窓の形の切り替え |
| `aac-tools-low.m4a` | 4 s、雑音＋音、48 kb/s、`-aac_pns 1 -aac_is 1 -aac_ms 1 -aac_tns 1` | PNS・intensity・M/S・TNS |
| `aac-lc-8k.m4a`・`aac-lc-96k.m4a` | 各 2 s | rate の両端 |
| `h264-*.mp4` | WS083 の `streams/*.h264` を `-c copy` で mp4 に（6 本） | probe と同じ picture の情報（§10.2） |
| `h264-high-b-aac.mp4` | 640x360、5 s、High、B pyramid、AAC、open GOP（`-x264-params open-gop=1`）、keyint 30 | 表示順、seek、A/V |
| `h264-main-crop-sar.mp4` | 1920x1080（crop 8 行）、`setsar=4/3`、VUI の colour 709・full range の 2 本 | crop、SAR、色 |
| `h264-baseline-small.mp4` | 176x144、Constrained Baseline、POC type 2 | B の無い深さ 0 |

参照（`*.sha256`・`*.rms`）も同じ script で作る: video は ffmpeg の NV12 の frame ごとの SHA-256（表示順、vkvideo-probe と同じ並び）、
audio は 1024 sample ごとの channel の RMS（`%.6e`、§10.3）。float の参照（大きい）は tree に入れず、host 試験が実行の時に `build/` の下に作る。

### 10.2 host 試験（`plan/ws202/tests/`、各 Phase）

| script | 確かめる物 | 基準 |
| --- | --- | --- |
| `run-host-bits.sh` | `bits.c`（ue・se・overrun・emulation prevention） | 手で作った bit 列の期待と一致 |
| `run-host-picture.sh` | scaler: 既知の YUV の値の BGRA（601・709・2020、limited・full）、1:1・縮小・拡大の 1 pixel の値、pool の参照の数（ASan） | 浮動小数の式との差 ≦ 1（8 bit）、時間（1080p → 1080p・720p）を表示 |
| `run-host-sound.sh` | downmix の係数、resample（1 kHz の sine の 44.1 → 48 kHz の SNR）、先頭・末尾の切り詰めの sample の数 | SNR ≧ 90 dB（帯域内）、切り詰めの数が一致 |
| `run-host-mediafile.sh`（`plan/tools/media/`、p004 で足す） | pasp・colr・end_us | make-media.py に足す試料の期待 |
| `run-host-aac.sh` | AAC の back end を `media_decoder_*` で通して float の出力（downmix・resample の前、core の rate、channel ごと）を host の ffmpeg（`-c:a aac` の decoder、`-f f32le`、同じ priming の扱い）と比べる | PNS の無い stream: 各 channel で max \|差\| ≦ 2^-14、RMS(差) ≦ 2^-17（ISO/IEC 14496-26 の 16 bit の基準に倣う、U6）。PNS の stream: frame ごとの RMS の比が ±1 dB 以内（乱数が違うので波形は比べない）。sample の総数が一致。ASan/UBSan で無事 |
| `run-host-h264.sh` | `h264.c`・`h264-dpb.c` を mp4 の stream に通し、picture ごとの `StdVideoDecodeH264PictureInfo`・slice の offset・DPB の計画（setup・参照の slot と info）を vkvideo-probe の `h264.c`・`dpb.c`（同じ stream の Annex B）の結果と比べる。VUI・POC type 1（手で作った SPS）・表示順の深さ | 全 picture で一致（POC type 1 は期待の値） |
| `run-host-vkvideo.sh` | vkvideo の back end を**偽の Vulkan の関数の表**（host の C、呼び出しを記録し、decode の時に「出力の image」に既知の Tile Y の模様を書く）で動かす: 作る object の順と引数、RESET、parameters の作り直し、DPB の slot、seek の後の捨て方、表示順と pts、de-tile と crop の結果、skip（status ERROR）の扱い、DEVICE_LOST | 期待の呼び出しの列と一致、de-tile の結果が模様と一致、ASan |
| `run-host-codec.sh`（`plan/tools/media/`、p007・p011 で直す） | 既存の全 file（libavcodec の add-in を含む）で、AAC が自前の back end へ行くこと、H.264 は host では DEVICE → add-in へ行くこと | 既存の PASS を保つ |

Vulkan Video の本当の decode は host でできない（host は llvmpipe だけ）ので、正しさの最後の確かめは 5330（§10.4）。

### 10.3 試験の道具 `media-probe`（`userland/tests/media-probe/`、p011）

- `media-probe [--video-hash] [--audio-rms] [--expect=FILE] [--seek=S] FILE`: libmedia で file を開き、video は表示順に frame ごとの NV12 の SHA-256
  （crop の窓、vkvideo-probe と同じ計算）、audio は 1024 sample ごと（core の rate、downmix の前）の channel の RMS を出す。downmix の前の値を
  取る口（試験の道具のための内部の口、例 `media_decoder_sound_float`）か 16 bit の出力の RMS を参照にするかは p011 で決め、p002 の参照と合わせる。`--expect` で参照と比べて
  exit status で合否（video は一致、audio は相対 1e-4 以内）。`--seek` は seek の後の最初の frame の pts と hash。
- 1 行目に `media-probe: video=h264/vulkan-video audio=aac/libmedia`（どの back end が使われたか）。
- test の image にだけ入れる（release の image に入れない）。tests の package の置き場所は WS106 の規則（`userland/tests/`）。

### 10.4 QEMU と 5330（T1 に 1 回で、p012）

- image: `plan/ws202/tests/config-media-qemu.mk`（CI の構成から **libavcodec を外し**、media-probe を足す）と `config-media-hw.mk`（5330 の
  i915 の試験の構成 `plan/ws075/tests/config-test-hw.mk` から libavcodec を外し、media-probe を足す）。stream は `--file` で `/tmp/ws202/` へ。
- QEMU（Venus）: (1) boot-test、(2) `media-probe --audio-rms --expect` を AAC の全 stream（自前の decoder が target で同じ結果）、(3) Music の
  scenario `tests/scenarios/apps/music/play.md`（libavcodec 無しで `MUSIC PLAY open codec=aac backend=libmedia`、位置が進む）、(4) Video Player で
  `h264-high-b-aac.mp4` を開き、DEVICE の notice（`VIDEOPLAYER OPEN error=… problem=4`）、(5) 既存の `tests/scenarios/apps/videoplayer/play.md` は
  libavcodec の入った image が要るので、T1 の既定の回帰の image で 1 回（AAC が自前に代わった後の回帰）。
- 5330（実機、SSH）: (1) `media-probe --video-hash --expect` を h264 の全 stream（WS083 の 6 本の mp4、crop・SAR・B・open GOP）、(2) 1080p の
  `/home/awe/zedbsd-media/sample-h264-{baseline,main,high}.mp4`・`sample-h264-high-aac.mp4` は T1 が scp で送り、参照の hash は T1 が host の
  ffmpeg で作る、(3) `media-probe --seek=2.5`、(4) Video Player で `sample-h264-high-aac.mp4` を 10 秒再生して `VIDEOPLAYER FRAMES shown=N late=M`
  （M が N の 5% 未満）と撮影、(5) Music で .m4a を 1 曲。
- 5330 の UAT（ユーザー、p013）: Video Player で音と絵の同期（口の動き等のある手元の動画、無ければ sample）、seek（矢印・bar の drag）、
  一時停止、全画面、終わり、Music で手元の .m4a（iTunes・CD の取り込み）を数曲、曲の切り替え、seek。音の質（雑音・途切れ）を耳で。

### 10.5 QEMU と実機の証拠を分けて書く

Phase の記録で、host・QEMU・5330 の結果を別の行に書く。やっていない確認は「未実施」と書く。

## 11. license

- 新しい code は全部自前（Zlib、`Copyright (C) 2026 Awe Morris`）。vkvideo-probe の `h264.c`・`dpb.c`・`frame.c` は同じ project の Zlib の code で、
  写して直してよい（D2）。
- FFmpeg（LGPL）の code は写さない。参照は (1) AAC の表の値（H5 の (a) の時、生成の script が data として読む）、(2) host 試験の参照の decoder
  （host の ffmpeg の program を実行するだけ）。
- 試験の stream は host の ffmpeg・x264 で合成の素材から作る（第三者の著作物を含まない）。x264 は GPL だが、出力の stream は x264 の著作物ではない
  （WS083 と同じ扱い）。`/home/awe/zedbsd-media/` の sample は tree に入れない。
- 規格の文書（ISO・ITU・Khronos）は読むだけ。spec の構文の名（`ics_info` 等）は事実の名として code の comment に使ってよい。

## 12. Phase の分け方

[ws.md](ws.md) の表。AAC の列（p005〜p007）と H.264 の列（p008〜p010）は p003 の後は独立で、2 人の担当で並べて進められる。

## 13. 人の判断の点（推し付き、ws.md にも同じ表）

| ID | 問い | 推し |
| --- | --- | --- |
| H1 | Vulkan Video の無い機械（Venus の QEMU、他の GPU）の H.264 | (a) エラーの表示、libavcodec があればそれ。CPU の decoder は別の WS（§5.10） |
| H2 | libavcodec の入った build | (a) 両方を持ち自前が先、release は libavcodec を入れたまま（§9.2） |
| H3 | HE-AAC（SBR・PS） | (a) core の AAC-LC だけで鳴らす、libavcodec があればそちら（§6.6） |
| H4 | ベータ2 に入れるか | 入れない（ベータ3）。約 62 LW（実時間 約 21 時間）と review・T1・UAT で RC（10/13）に間に合わない。release の image は libavcodec で今まで通り再生できる |
| H5 | AAC の表の出典 | (a) FFmpeg 9.0.2 の tarball から値だけを script で生成（WS141 と同じ「値は事実」の扱い）（§6.7） |
| H6 | 表示を GPU の image の共有にするか | 今は CPU への写し（§5.5）。足りなければ別の WS |

## 14. 確かめていない物

| ID | 内容 | どこで |
| --- | --- | --- |
| U1 | fragmented の大きな file の open の時間（全 moof を先に読む） | p004 で 1 GB 級の合成の file を host で測る（記録だけ） |
| U2 | 1080p の de-tile の時間（HOST_CACHED の読み） | p012 の 5330（media-probe の時間の行） |
| U3 | CPU の H.264 decoder の性能（H1 の (b) の時だけ） | — |
| U4 | AAC の decode の時間 | p006 の host、p012 の 5330 |
| U5 | scaler の時間 | p003 の host、p012 の 5330 |
| U6 | AAC の精度の基準の値が ISO/IEC 14496-26 の基準と同じか | 規格を読める時に確かめる。読めなければ §10.2 の値を WS の基準とする |
| U7 | libvulkan の soname と dlopen の名 | p009 |
| U8 | decode（i915 の worker を 5 ms 塞ぐ）と desktop の描画の同時の fps（WS083 の U8 と同じ） | p012 の 5330（compositor の fps の log） |

## 15. Future（WS202 の外）

- H.264 の CPU の decoder（H1 の (b) を選ばない時）。
- HE-AAC の SBR・PS（H3 の (a) の時）。
- GPU の image を共有する表示（H6）、TRANSFER_SRC の経路（他の Vulkan の実装）。
- vkvideo-probe と libmedia の `h264.c`・`dpb.c` の重複の解消（probe が libmedia の parser を使う）。
- stsd の複数の entry（途中の解像度の変更）、AAC の sample 単位の seek、HEVC の Vulkan Video（WS083 の Future）。
