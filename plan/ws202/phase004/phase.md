<!-- awesome-plan project=zedbsd record=ws202-p004 -->

# ws202-p004: mediafile の pasp・colr・表示の終わり

Status: planned
Disposition: normal
Parent: [WS202](../ws.md)
見積もり: 2 LW
依存: p001

## 目的

mp4 の reader（既存）に、縦横比・色・表示の終わりを足す（design §4）。

## 成果

- `mediafile.h` の `struct media_track` に `sar_num`・`sar_den`（0 は未知）、`colour_matrix`（0 未知、H.273 の値）・`full_range`（-1 未知、0・1）、`end_us`（0 未知）。
  J1 で container を絞る時だけ `container`（`media_file_format_name` と同じ文字列、mediafile.c が全 track に入れる。L2-15）。
- `mp4.c`:
  - `read_visual_entry`: 子の `pasp`（hSpacing・vSpacing）、`colr` の `nclx`（primaries・transfer・matrix 各 16 bit、full_range_flag 1 bit）と `nclc`（primaries・
    transfer・matrix、full range は -1 のまま）。avcC 等を見つけた後も続く子を読む（loop の形を変える）。
  - `read_edit_list`: 最初の実の edit の segment_duration を覚え、`finish_tracks` で `end_us` = 遅延 ＋ segment の長さ（無い・0 は 0）。
- `plan/tools/media/make-media.py` に pasp・colr（nclx・nclc）・edts の長さを持つ試料、`host-mediafile.c` に期待（WS の外: 差分を Q1 へ）。
- U1: fragmented の 100〜200 MB の合成の file（ffmpeg `-movflags frag_keyframe+empty_moov`、低い bitrate で長く）を**自分の worktree の `build/`** に作り、
  `media_file_open` の時間を測って 1 GB に外挿して記録する。file は消さず、path を Q1 に送る（rm は Q1）。

## 確認

| コマンド | 期待 |
| --- | --- |
| `sh plan/tools/media/run-host-mediafile.sh` | 既存と新しい期待が PASS |
| design §10.6 の build（`ZEDBSD_CONFIG=config/ci/config-amd64.mk`、libmedia・libbrowser・videoplayer・music） | exit 0、`grep -c 'warning:'` が 0 |
