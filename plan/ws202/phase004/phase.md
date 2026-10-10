<!-- awesome-plan project=zedbsd record=ws202-p004 -->

# ws202-p004: mediafile の pasp・colr・表示の終わり

Status: planned
Disposition: normal
Parent: [WS202](../ws.md)
見積もり: 2 LW
依存: p001

## 目的

mp4 の reader（`userland/desktop/mediafile/mp4.c`、既存）に、縦横比・色・表示の終わりを足す（design §4）。demuxer は新しく書かない。

## 成果

- `mediafile.h` の `struct media_track` に `sar_num`・`sar_den`（0 は未知）、`colour_matrix`（0 未知、H.273 の matrix_coefficients の値をそのまま）・
  `full_range`（-1 未知、0・1）、`end_us`（表示の終わり、0 は未知）。comment に意味。
- `mp4.c`:
  - `read_visual_entry` で子の `pasp`（hSpacing・vSpacing、各 32 bit）と `colr`（type `nclx` の primaries・transfer・matrix（各 16 bit）・
    full_range_flag（1 bit））を読む。avcC 等の configuration の box を見つけた後も、続く子の pasp・colr を読む（今は configuration で return して
    いるので、loop の形を変える）。
  - `read_edit_list` で最初の実の edit の segment_duration（movie timescale）を覚え、`finish_tracks` で `end_us` = 遅延 + segment の長さ にする
    （edit が無い・長さ 0 は 0）。
- `plan/tools/media/make-media.py` に pasp・colr・edts の長さを持つ試料を足し、`host-mediafile.c` に期待を足す（`plan/tools/` は WS の外の file:
  差分を Q1 に送る、または Q1 の許可で直す）。
- fragmented の大きな file の open の時間を測る（U1）: host で 1 GB 級の fragmented の合成の file（ffmpeg `-movflags frag_keyframe+empty_moov`、
  testsrc の低い bitrate で長く）を作って `media_file_open` の時間を記録する（直さない。記録だけ）。

## 確認

| コマンド | 期待 |
| --- | --- |
| `sh plan/tools/media/run-host-mediafile.sh` | 既存と新しい期待が PASS |
| `make … ZEDBSD_USER_PROGRAMS="libmedia" build/<担当>/dynamic/libmedia.so` | warning 0 |

## 注意

- `struct media_track` を使う所（videoplayer・music・libbrowser・engine）は field を足すだけなので code の変更は要らない。build で確かめる。
