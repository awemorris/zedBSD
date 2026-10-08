<!-- awesome-plan project=zedbsd record=ws177-p029 -->

# ws177-p029: Ogg の reader（案 T の 3）

Parent: [WS177](../ws.md)
Status: planned（2026-10-08 夜 P2 q906 で立てた）
Disposition: normal
Primary Milestone: MG006（WS から継承）
Queue / attempts: q906（P2、承認: ユーザー 2026-10-08 夜「Tは通常優先度でスケジューリングをお願いします」）
Origin: [backlog-p2](../backlog-p2.md) の 116〜123 のうち browser の video を除く分、[案](../phasing-20261008.md) の T。
依存: ws177-p028

## 範囲

- Ogg の page と packet（lacing、page をまたぐ packet）、Opus（OpusHead・OpusTags、pre-skip、48 kHz の granule）・Vorbis（3 つの header を private data に）・Theora（3 つの header、granule の key frame の shift）。
- 長さは最後の page の granule から、seek は granule で二分。
- 切れた file・CRC の誤った page（飛ばす）。

## 確認の予定

- host: `sh plan/ws177/tests/host-media-t.sh` の group（ffmpeg で作った file を ffprobe の packet と照合、切った file・壊れた file）。
- build warning 0、style-check。QEMU は p031 の後に T1 へまとめて。
