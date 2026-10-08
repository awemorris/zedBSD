<!-- awesome-plan project=zedbsd record=ws177-p027 -->

# ws177-p027: fragmented MP4 と壊れた index（案 T の 1）

Parent: [WS177](../ws.md)
Status: in-progress（2026-10-08 夜 P2 q906 実装・host PASS・build）
Disposition: normal
Primary Milestone: MG006（WS から継承）
Queue / attempts: q906-i01（P2、承認: ユーザー 2026-10-08 夜「Tは通常優先度でスケジューリングをお願いします」）
Origin: [backlog-p2](../backlog-p2.md) の WS122 ws122-p003（mediafile）の行「fragmented MP4（moof）、MPEG-TS・AVI・Ogg、壊れた index」の MP4 の分、[案](../phasing-20261008.md) の T。

## 範囲

- fragmented MP4: moov の mvex（trex の既定）、moof の traf（tfhd・tfdt・trun）を開く時に走査して、tkhd の track_ID の track の sample の表に足す。stbl の表が空・無くても mvex のある track は残す。
- 壊れた index: file の外を指す sample（file が途中で切れた、表が壊れた）と MF_PACKET_MAX を越える sample を開く時に落とし、track ごとに数える（`mf_track.dropped_count`、新しい field）。

## 実装（2026-10-08 P2）

- `userland/desktop/mediafile/mp4.c`: `find_moov` の top-level の box の読みを `top_box` に分けた。`mp4_open` は mvex があれば `read_extends`（trex）→ `read_fragments`（全ての top-level の box を見て moof を丸ごと読む、16 MiB まで）→ `read_moof` → `read_traf`（tfhd の base data offset・default-base-is-moof・暗黙の base（moof の先頭、2 つ目からは前の traf の data の後）、tfdt（無ければ track の最後の sample の後から））→ `read_trun`（data offset・first sample flags・sample ごとの duration・size・flags・cts、`sample_is_non_sync_sample` で key、無い field は tfhd・trex の既定）→ `append_sample`（2 倍で伸ばす、16 Mi まで）。file の外に出る box（切れた file）・壊れた moof は fragment の読みを止める（そこまでを再生）。最後に `finish_tracks` で `drop_outside`、packet_count、長さ（header が 0 か短い時は sample から）。時刻は 2^33 秒までに制限（細工された tfdt の overflow を防ぐ）。
- `mediafile.h`: `struct mf_track` に `dropped_count`。
- 知らせる側（player が「N 個の frame を飛ばした」と出す）は p031（libmedia への移行）で見る。

## 確認

- host（新規）: `sh plan/ws177/tests/host-media-t.sh`（group mp4）→ PASS（ASan・UBSan）。ffmpeg で作った 6 つ（`frag_keyframe+empty_moov+default_base_moof`・`frag_keyframe+empty_moov`（tfhd の base data offset）・`frag_keyframe`・`separate_moof`・`frag_every_frame`・faststart の普通の MP4）と、変えた 5 つ（tfdt を free に、track_ID を 7・3 に、fragmented を fragment の data の途中で切る、普通の MP4 を 6 割で切る、video の最後の chunk を file の外に）を、ffprobe の packet（pts・dts・size・key・Adler-32）と track ごとに照合、落とした数、seek（0・1.0・1.55・2.9・10 s の key frame）。変更前の reader では 11 のうち 10 が FAIL（faststart だけ ok）を確かめた。
- host（既存）: ws122 の make-media.py の 6 つ（bad-cut・bad-junk・mkv-cues・mkv-scan・mp4-narrow・mp4-wide）が期待どおり、sample.mp4 を最後まで読む（手で同じ手順を流した。run-host-mediafile.sh は中の rm -rf のため流していない）。
- build: `make -j16 ZEDBSD_CONFIG=config/current-uat.mk BUILD=build/amd64 build/amd64/bin/videoplayer build/amd64/bin/music build/amd64/dynamic/libmedia.so` exit 0・warning 0。style-check 指摘なし、`git diff --check` 空。
- QEMU: 未実施（T1 に、p031 の後にまとめて）。
