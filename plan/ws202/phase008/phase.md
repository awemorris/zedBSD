<!-- awesome-plan project=zedbsd record=ws202-p008 -->

# ws202-p008: H.264 の parser と DPB

Status: planned
Disposition: normal
Parent: [WS202](../ws.md)
見積もり: 7 LW
依存: p002（stream）、p003（`bits.c`）

## 目的

mp4 の packet（1 AU）から Vulkan Video に渡す `StdVideo*` と slice の位置、DPB の計画、表示順の深さを作る。vkvideo-probe と同じ結果を出すことを
host で確かめる（design §5.2・§5.3）。

## 成果（`userland/desktop/libmedia/`）

1. `h264.h`・`h264.c`: `userland/tests/vkvideo-probe/h264.[ch]` を写して直す（同じ project の Zlib の code、file の頭の comment に元の file を書く）。
   - 形: `struct h264_parser`（SPS 32・PPS 256 の表と scaling list、POC の状態）、`h264_parser_init`、`h264_parser_config`（avcC の SPS・PPS を入れる。
     `bitstream.c` の avcC の読みと重ねないように、avcC は `media_bitstream_open` が Annex B の prefix に変えた物を NAL で読む形でよい）、
     `h264_parser_access_unit(parser, data, size, picture, &reason)`（1 AU の Annex B）。
   - 足す物: VUI（aspect_ratio_idc と 255 の明示の SAR、video_full_range_flag、matrix_coefficients、bitstream_restriction の
     max_num_reorder_frames・max_dec_frame_buffering。hrd の parameters は読み飛ばす（nal・vcl の hrd を構文の通りに読んで捨てる））、POC type 1
     （H.264 8.2.1.2）、FMO（num_slice_groups_minus1 > 0）・interlaced（frame_mbs_only_flag 0）・profile の範囲（design §5.8）の検べ
     （`h264_check_sps` が PROFILE の理由の文を返す）、NAL 2〜4 の拒否、14・15・20 の飛ばし。
   - 1 AU の中の SPS・PPS の更新は表を書き換え、`parameters_changed` の印を立てる（design D6）。
   - slice の offset・size は AU の bytes の中の位置（start code の後）。
2. `h264-dpb.c`: `vkvideo-probe/dpb.[ch]` を写して直す。slot の数を SPS から（`max_num_ref_frames + 1`）、表示順の深さ `h264_reorder_depth(sps)`
   （design §5.3: VUI → profile 66 は 0 → 表 A-1 の MaxDpbMbs ÷ MB 数、≦ 16）。表 A-1 は ITU-T H.264 から level ごとの MaxDpbMbs だけを持つ。
   - flush の後の開始（design §5.7）: `h264_dpb_restart` で空にし、最初の I（IDR か全 slice が I）だけを受ける。非 IDR の I の時は frame_num の gap の
     補いをせず、その I を最初の参照にする。参照の slot が無い P・B は `plan` が「捨てる」を返す。
3. host 試験 `plan/ws202/tests/host-h264.c`・`run-host-h264.sh`:
   - p002 の `h264-*.mp4`（WS083 の 6 本を mp4 にした物）を mediafile で読み、`media_bitstream_convert` で Annex B にして libmedia の parser と DPB に通す。
     同じ stream の Annex B（`plan/ws083/tests/streams/*.h264`）を vkvideo-probe の `h264.c`・`dpb.c` に通し、picture ごとに
     `StdVideoDecodeH264PictureInfo`（flags・ids・frame_num・idr_pic_id・PicOrderCnt）、slice の数と大きさ、DPB の計画（reset・setup の slot と
     info・参照の slot と info）を比べる。全 picture で一致。
   - VUI: `h264-main-crop-sar.mp4` の SAR 4:3、matrix 1、full range 1、crop。`h264-high-b-aac.mp4` の表示順の深さ（x264 の VUI の値）。
   - POC type 1: 手で作った SPS と slice header の bit 列（ITU-T H.264 8.2.1.2 の式で期待を計算）。
   - seek の模擬: `h264-high-b-aac.mp4` の途中の sync sample から始め、最初の I の前を捨て、leading の B を捨てることを確かめる。
   - 乱れた入力（bit の反転 1000 通り、seed 固定）で crash しない（ASan/UBSan）。

## 確認

| コマンド | 期待 |
| --- | --- |
| `sh plan/ws202/tests/run-host-h264.sh` | 6 本の全 picture で probe と一致、VUI・POC type 1・seek の模擬・乱れの試験が PASS |
| `sh plan/ws083/tests/run-host-vkvideo-probe.sh` | PASS（probe を変えていないことの確かめ） |
| libmedia の build | warning 0 |

## 注意

- vkvideo-probe（`userland/tests/vkvideo-probe/`）は変えない（WS083 の試験の道具）。重複の解消は Future（design §15）。
