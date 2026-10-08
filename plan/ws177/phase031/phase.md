<!-- awesome-plan project=zedbsd record=ws177-p031 -->

# ws177-p031: videoplayer・Music を libmedia へ移す（案 T の 5）

Parent: [WS177](../ws.md)
Status: test-wait（T1-469、2026-10-08 夜 Q1）
Disposition: normal
Primary Milestone: MG006（WS から継承）
Queue / attempts: q906（P2、承認: ユーザー 2026-10-08 夜「Tは通常優先度でスケジューリングをお願いします」）
Origin: [backlog-p2](../backlog-p2.md) の 116〜123 のうち browser の video を除く分、[案](../phasing-20261008.md) の T。
依存: ws177-p030

## 範囲

- videoplayer と Music が mediafile・codec を自分で compile するのをやめ、libmedia.so を link する（libmedia の export を広げる）。
- 名前の接頭辞を media_ に（mf_・vp_codec の public の名前）、decoder の ops の表（GPU decode を後で差す境界、今は libavcodec の add-in だけ）。
- 落とした packet（dropped_count）を player で知らせる。
- 117（GPU decode・独自の AAC）は WS083 の実機の結果の後、118（game mode の cursor plane）はこの WS では入れない（Q1、Future Work）。

## 実装（2026-10-08 P2）

- **公開の名前を media_ に**: mediafile の API（`media_file_open`・`_open_source`・`_track_count`・`_track`・`_duration_us`・`_format_name`・`_read`・`_seek`・`_close`、新しい `media_file_dropped`）、型（`struct media_file`・`media_track`・`media_packet`・`media_source`）、定数（`MEDIA_TRACK_*`・`MEDIA_CODEC_*`）。各 format の reader の内部（`mf_read_at` など、private header）はそのまま。
- **decoder を libmedia へ**: `videoplayer/codec.c`→`libmedia/avcodec.c`（libavcodec の add-in、関数は static にして ops の表 `media_avcodec_ops`）、`bitstream.c`→`libmedia/bitstream.c`（`media_bitstream_*`）、`codec-layout.h`→`libmedia/avcodec-layout.h`。新しい `libmedia/decoder.c`（back end の表 `decoder_backends[]` を順に試し、最初に開けた back end で decode する＝GPU decode を前に差す境界。decoder・picture・scaler は作った back end を持つ）、`media-decoder.h`（app の API: `media_codec_load`・`media_decoder_*`・`media_frame_*`・`media_scaler_free`、問題の番号は `MEDIA_PROBLEM_*`）、`media-private.h`（`struct media_decoder_ops`・bitstream・`media_log`）。
- **新しい codec の decode**: Vorbis・Theora は extradata（Ogg の 3 つの header の Xiph lacing）を AVCodecParameters の先頭の field（codec_type・codec_id は AVCodec の先頭から、extradata・size）で `avcodec_parameters_to_context` に渡す（`avcodec-layout.h` に 2 つの構造体、`host-layout.c` で FFmpeg 7（61）と 9（63）の header に照合）。MJPEG は "mjpeg"、PCM は track の codec_name（pcm_s16le・pcm_u8・pcm_s24le）を decoder の名に。
- **videoplayer・Music は libmedia.so を link**（自分で mediafile・codec を compile しない）: 各 Makefile の source と依存（desktop/libmedia）、`platform/amd64/vmunix.mk` の link の規則（`-l:libmedia.so`・NEEDED の確かめ）。libmedia の log は `media_set_log` で各 app の行（"VIDEOPLAYER …"・"MUSIC …"）に。`exports.map` に `media_file_*`・`media_codec_*`・`media_decoder_*`・`media_frame_*`・`media_scaler_free`。
- **落とした packet の知らせ**: videoplayer の log `OPEN … dropped=N`・`END reached dropped=N`、libmedia の engine の `MEDIA ended … dropped=N`（画面の notice は出していない: 絵がある間は notice を描かない作りのため、UI は残り）。
- libbrowser/page/media.c の `struct mf_source`→`struct media_source`（Q1 の許可、1 行）。Files の `mime.c` に m2ts・mts（video/mp2t）・ogv（video/ogg）（Q1 の許可）。
- 他の WS の試験を名前と path に追従（Q1 の許可）: plan/ws122/tests/{host-codec.c,host-layout.c,host-mediafile.c,run-host-codec.sh}、plan/ws121/tests/{host-engine.c,run-host-engine.sh}、plan/ws074/tests/host-build.sh、plan/ws177/tests/{host-media-t.c,host-music-play.c}。run-host-codec.sh に Theora＋Vorbis（Ogg）・MJPEG＋PCM（AVI）・H.264＋MP3（TS）の decode を足した。
- 範囲の外（記録）: 117（GPU decode・独自の AAC）は WS083 の実機の結果の後、118（game mode の cursor plane）は Future Work（Q1）。Music の .ogg・.opus の曲の一覧への追加（Music の library は m4a 中心）、落とした packet の画面の notice。

## 確認

- host: `sh plan/ws177/tests/host-media-t.sh mp4 ts ogg avi` PASS、`plan/ws122/tests/run-host-codec.sh` PASS（layout 61・63 PASS、host の FFmpeg 7 で sample.mp4・H.264＋AAC（MP4・MKV）・H.265・VP9＋Opus・**Theora＋Vorbis（decoder theora・vorbis、100 枚、音 4.01 s）・MJPEG＋PCM（mjpeg・pcm_s16le）・H.264＋MP3 in TS** を decode・scale・seek）、`run-host-mediafile.sh` PASS、`plan/ws121/tests/run-host-engine.sh` PASS、`plan/ws177/tests/host-music-play.sh`・`host-music-m.sh` PASS、`plan/ws120/tests/run-host-music-library.sh` PASS、`plan/ws074/tests/host-build.sh plain` exit 0。
- build: `make -j16 ZEDBSD_CONFIG=config/current-uat.mk BUILD=build/amd64 build/amd64/dynamic/libmedia.so build/amd64/bin/videoplayer build/amd64/bin/music build/amd64/dynamic/libbrowser.so build/amd64/bin/files` exit 0・warning 0（libmedia の NEEDED は libc だけ、videoplayer の NEEDED に libmedia.so）。style-check: libmedia の全 file 0。
- QEMU: 未実施（T1 に依頼: Video Player で MP4・TS・Ogg（Theora）・AVI を開いて再生、Music の再生、browser の `<video>`）。
