<!-- awesome-plan project=zedbsd record=ws177-p030 -->

# ws177-p030: AVI の reader（案 T の 4）

Parent: [WS177](../ws.md)
Status: planned（2026-10-08 夜 P2 q906 で立てた）
Disposition: normal
Primary Milestone: MG006（WS から継承）
Queue / attempts: q906（P2、承認: ユーザー 2026-10-08 夜「Tは通常優先度でスケジューリングをお願いします」）
Origin: [backlog-p2](../backlog-p2.md) の 116〜123 のうち browser の video を除く分、[案](../phasing-20261008.md) の T。
依存: ws177-p029

## 範囲

- RIFF・LIST hdrl（avih・strl の strh・strf）・LIST movi・idx1（無ければ movi を走査）、OpenDML の AVIX は範囲外（読める所まで）。
- MPEG-4 Part 2・H.264・MJPEG、PCM・MP3・AAC。時刻は strh の rate/scale と frame の番号（音は byte 数か block）。
- index の外を指す entry を落として数える（p027 と同じ dropped_count）。

## 確認の予定

- host: `sh plan/ws177/tests/host-media-t.sh` の group（ffmpeg で作った file を ffprobe の packet と照合、切った file・壊れた file）。
- build warning 0、style-check。QEMU は p031 の後に T1 へまとめて。
