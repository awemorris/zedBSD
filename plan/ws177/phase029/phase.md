<!-- awesome-plan project=zedbsd record=ws177-p029 -->

# ws177-p029: Ogg の reader（案 T の 3）

Parent: [WS177](../ws.md)
Status: in-progress（2026-10-08 夜 P2 q906 実装・host PASS・build）
Disposition: normal
Primary Milestone: MG006（WS から継承）
Queue / attempts: q906（P2、承認: ユーザー 2026-10-08 夜「Tは通常優先度でスケジューリングをお願いします」）
Origin: [backlog-p2](../backlog-p2.md) の 116〜123 のうち browser の video を除く分、[案](../phasing-20261008.md) の T。
依存: ws177-p028

## 範囲

- Ogg の page と packet（lacing、page をまたぐ packet）、Opus（OpusHead・OpusTags、pre-skip、48 kHz の granule）・Vorbis（3 つの header を private data に）・Theora（3 つの header、granule の key frame の shift）。
- 長さは最後の page の granule から、seek は granule で二分。
- 切れた file・CRC の誤った page（飛ばす）。

## 実装（2026-10-08 P2）

- `userland/desktop/mediafile/ogg.c`（新規）: 先頭の page が BOS の Ogg（`mf_ogg_detect`）。BOS の page の最初の packet で stream を判定（OpusHead・`\x01vorbis`・`\x80theora`、他の stream は読まない）、header の packet（Opus 2・Vorbis 3・Theora 3）を集めて private data に（Opus は OpusHead、Vorbis・Theora は 3 つを Xiph lacing、Matroska の CodecPrivate と同じ形）。
- page は CRC（多項式 0x04c11db7）で確かめ、stream ごとに packet を組み立てて page の終わりに queue。時刻: stream の最初（と seek・lost page の後）は page の granule から逆に、その後は packet の長さで前へ（Opus は TOC から、Vorbis は setup header の mode（末尾から逆に読む）の block size から（前の block の 1/4＋自分の 1/4、最初は自分を 2 回）、Theora は 1 frame）。Opus は pre-skip を引く、Theora の granule は key frame の番号＜＜shift＋その後の frame（3.2.1 より前は 0 から）。key は音は全部、Theora は intra frame。
- 長さは末尾 1 MiB の各 stream の最後の granule。seek: 先頭の Theora の stream（無ければ先頭）の granule で file を二分して時刻以前に終わる最後の page。Theora はその page（と次の page の key が時刻以前ならそれ）の key frame の前で終わる最後の page から読み、key frame より前の packet は渡さない。
- 壊れ方: CRC の誤った page は飛ばし（壊れた packet を数える）、page の番号の欠け・続きの無い packet・file の終わりで切れた packet を数える（`dropped_count`）、"OggS" を探し直す。
- `mediafile.h` に `MF_CODEC_VORBIS`（10）・`MF_CODEC_THEORA`（11）、`mkv.c` に A_VORBIS・V_THEORA。`mediafile.c` の判定（Ogg は TS の前）、3 つの Makefile と他の WS の 4 つの host 試験の一覧に ogg.c。
- 範囲の外（記録）: Vorbis・Theora の decode（player の libavcodec の add-in は extradata を渡さないので、Vorbis・Theora の decoder は開けない。p031 の decoder の ops で extradata を渡す道を作る）、FLAC in Ogg・Speex・Skeleton（読まない）、chained Ogg（途中の新しい BOS は読まない）、Opus の pre-roll（seek の後の 80 ms）。

## 確認

- host: `sh plan/ws177/tests/host-media-t.sh mp4 ts ogg` → PASS（ASan・UBSan）。ffmpeg で作った 4 つ（Opus 48 kHz、Vorbis 44.1 kHz、Theora＋Vorbis、Theora（GOP 7）＋Opus）を ffprobe の packet（pts・size・key・Adler-32）と照合、seek 5 点（Theora は key frame、音だけの file は時刻以前 2 秒以内）。壊した 2 つ: 途中の page の 1 byte（その page の packet が落ちて数えられ、他は読める）、途中で切る（各 track が ffprobe の先頭と一致）。
- host（他の WS）: `plan/ws122/tests/run-host-mediafile.sh`・`run-host-codec.sh`・`plan/ws121/tests/run-host-engine.sh` PASS、`plan/ws074/tests/host-build.sh plain` exit 0。
- build: videoplayer・music・libmedia.so exit 0・warning 0。style-check 指摘なし。
- QEMU: 未実施（p031 の後に T1 へ）。
