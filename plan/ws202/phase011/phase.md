<!-- awesome-plan project=zedbsd record=ws202-p011 -->

# ws202-p011: Video Player、試験の道具 media-probe、試験の image、利用者の文書

Status: planned
Disposition: normal
Parent: [WS202](../ws.md)
見積もり: 4 LW
依存: p007、p010

## 目的

Video Player を自前の decoder に合わせ、T1 の試験（p012）に要る道具と image の構成を揃える（design §9.3・§10.3・§10.4）。

## 成果

1. `userland/desktop/videoplayer/`:
   - `main.c` の `vp_notice`: DEVICE・PROFILE の文、MISSING の文の変更（design §9.3）。
   - 縦横比: `media_frame_aspect` を描画の fitted の計算に（`vp_draw` の aspect）。
   - log: `VIDEOPLAYER OPEN … video=<codec>/<backend> audio=<codec>/<backend>`、`VIDEOPLAYER FRAMES shown=N late=M`（`vp_media_take` が捨てた数を数える）。
   - `videoplayer.h`・`media.c`・`Makefile` の comment の「libavcodec が要る」の記述を直す。
2. `userland/tests/media-probe/`（新、design §10.3）: `main.c`・`Makefile`（`userland/tests/vkvideo-probe/Makefile` と同じ形の package の定義、
   test の image だけ）。libmedia の `media_file_*`・`media_decoder_*` を使う。video は表示順の frame ごとに NV12 の SHA-256（crop の窓、
   vkvideo-probe の `frame_hash` と同じ計算: Y の各行、続いて CbCr の各行）。audio は back end の decode の結果を downmix の前で取る口が要る:
   試験の道具のために `media_decoder_sound_float`（core の rate、channel ごと）を足すか、16 bit の出力の RMS を参照にするかを実装の時に決め、
   design §10.3 と参照（p002 の `.rms`）を合わせる（決めた方を phase.md に書く）。`--time` で decode と de-tile の時間の平均・最大（U2）。
   SHA-256 は vkvideo-probe と同じく `userland/base/common/sha256.c` を compile して使い、`frame_hash` の計算（Y の行、続いて CbCr の行）を写す（Zlib）。
3. 試験の image の構成（`plan/ws202/tests/`）:
   - `config-media-qemu.mk`: `include config/ci/config-amd64.mk` の後に `ZEDBSD_USER_PROGRAMS := $(filter-out libavcodec,$(ZEDBSD_USER_PROGRAMS)) media-probe`。
   - `config-media-hw.mk`: `include plan/ws075/tests/config-test-hw.mk` の後に同じ filter-out と media-probe。
   - stream は `--file /tmp/ws202/<名>=plan/ws202/tests/streams/<名>`（T1 の依頼に列挙）。
4. `tests/scenarios/apps/videoplayer/play.md` の `areas`・`paths`・目的の「libavcodec を使う」を今の形（AAC は libmedia、MPEG-4 Part 2 の映像は
   libavcodec）に直す。新しい scenario `tests/scenarios/apps/videoplayer/h264-native.md`（5330、machine: hardware、libavcodec 無しの image で
   `h264-high-b-aac.mp4` を開き `video=h264/vulkan-video audio=aac/libmedia`、再生・seek・終わり）と `no-video-decode.md`（QEMU、DEVICE の notice）。
   tests/ の file は Q1 へ差分を送るか許可を受けて直す。
5. 利用者の文書: `docs/reference/media-playback.md`（新、英語）: 自前で再生できる形（H.264 の範囲、Vulkan Video が要る機械、AAC-LC と HE-AAC の扱い）、
   libavcodec が入っている時の扱い、notice の意味。docs/ から plan/ へ link しない。`docs/reference/vulkan-video.md` の「Example program」に
   libmedia が利用者であることを 1 文足す。

## 確認

| コマンド | 期待 |
| --- | --- |
| `make -j16 BUILD=build/<担当> ZEDBSD_CONFIG=plan/ws202/tests/config-media-hw.mk ZEDBSD_USER_PROGRAMS="libmedia videoplayer music media-probe" …` の各成果 | warning 0、check-dynamic-elf PASS |
| media-probe の host の build（Linux の host の libvulkan で link できる範囲、または build だけ） | warning 0 |
| `make … ZEDBSD_CONFIG=plan/ws202/tests/config-media-qemu.mk` の image の package の一覧 | libavcodec が無い、media-probe がある |

QEMU・実機は p012。
