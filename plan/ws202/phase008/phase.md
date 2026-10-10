<!-- awesome-plan project=zedbsd record=ws202-p008 -->

# ws202-p008: H.264 の parser と DPB の写し

Status: planned
Disposition: normal
Parent: [WS202](../ws.md)
見積もり: 7 LW
依存: p002（stream）、p003（`bits.c`）

## 目的

mp4 の packet（1 AU）から Vulkan Video に渡す `StdVideo*` と slice の位置、DPB の計画、表示順（POC の bumping）を作る。vkvideo-probe が扱う範囲で同じ結果を出すことを
host で確かめる（design §5.2・§5.3・§5.6）。probe に無い gap・MMCO 5 は p015。

## 成果（`userland/desktop/libmedia/`）

1. `h264.h`・`h264.c`: `userland/tests/vkvideo-probe/h264.[ch]` を写して直す（file の頭に元の file）。
   - `struct h264_parser`、`h264_parser_init`、`h264_parser_config`（avcC を `media_bitstream_open` の Annex B の prefix として NAL で読む）、
     `h264_parser_access_unit(parser, data, size, picture, &reason)`。
   - VUI（SAR、video_full_range_flag、matrix_coefficients、max_num_reorder_frames・max_dec_frame_buffering、hrd は読んで捨てる）。D19: VUI の中の読み誤りは VUI だけを
     捨て SPS は使う。
   - POC type 1（8.2.1.2）。
   - `h264_check_sps`（PROFILE の理由の文: profile・chroma・bit depth・frame_mbs_only・FMO）。D20 の大きさの検べは p009（capability と比べる）。
   - NAL 2〜4 は EINVAL、14・15・20 は飛ばす、冗長 slice（`redundant_pic_cnt` > 0）は捨てる。
   - SPS・PPS の更新で `parameters_changed`（D6）。
2. `h264-dpb.c`: `vkvideo-probe/dpb.[ch]` を写す（sliding window、MMCO 1〜4・6）。slot の数 `max_num_ref_frames + 1`。
   - 表示順（D22）: `h264_output_push(picture, poc)`・`_pop`（深さを越えたら POC の最小）・`_drain`。深さ `h264_reorder_depth(sps)`（VUI → profile 66 は 0 → 表 A-1 の
     MaxDpbMbs ÷ MB 数、≦ 16）。IDR の前に全部出す。表 A-1 の MaxDpbMbs は ITU-T H.264 から level ごとに持つ。
3. host 試験 `plan/ws202/tests/run-host-h264.sh`:
   - p002 の `h264-<名>.mp4`（WS083 と同じ素材の 6 本）を mediafile で読み、`media_bitstream_convert` で Annex B にして libmedia の parser と DPB に通す。**同じ mp4 から
     取り出した** `streams/h264-<名>.h264` を probe の `h264.c`・`dpb.c` に通し、picture ごとに `StdVideoDecodeH264PictureInfo`・slice・DPB の計画を比べる。
   - 表示順: `h264-high-b-aac.mp4`・`h264-nocts.mp4` で、bumping で出る POC の順が ffmpeg の表示順（`ffprobe -show_frames` の順）と一致し、当てた時刻が単調に増える。
   - VUI（`h264-main-crop-sar.mp4` の SAR 4:3、matrix 1、full range、crop）、深さ（`h264-baseline-small.mp4` は 0）。
   - POC type 1: 手で作った SPS・slice header の bit 列。
   - VUI の途中で切れた SPS で SPS が使える。
   - 乱れの 1000 通り（seed 固定）で crash しない。

## 確認

| コマンド | 期待 |
| --- | --- |
| `sh plan/ws202/tests/run-host-h264.sh` | 全項目 PASS（ASan/UBSan） |
| `sh plan/ws083/tests/run-host-vkvideo-probe.sh` | PASS（probe を変えていない） |
| libmedia の build | warning 0 |

## 注意

- 「probe と一致」は probe が扱う範囲だけの確かめ（probe は MMCO 5・gap で止まる）。それらは p015。
- vkvideo-probe は変えない。
