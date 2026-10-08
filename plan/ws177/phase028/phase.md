<!-- awesome-plan project=zedbsd record=ws177-p028 -->

# ws177-p028: MPEG-TS の reader（案 T の 2）

Parent: [WS177](../ws.md)
Status: planned（2026-10-08 夜 P2 q906 で立てた）
Disposition: normal
Primary Milestone: MG006（WS から継承）
Queue / attempts: q906（P2、承認: ユーザー 2026-10-08 夜「Tは通常優先度でスケジューリングをお願いします」）
Origin: [backlog-p2](../backlog-p2.md) の 116〜123 のうち browser の video を除く分、[案](../phasing-20261008.md) の T。
依存: ws177-p027（同じ試験の道具）

## 範囲

- MPEG-TS（188 byte の packet、PAT・PMT、PES、H.264（Annex B のまま）・AAC の ADTS（ADTS の header を外して AudioSpecificConfig を作る）・MP3、90 kHz の PTS・DTS、PCR は使わない）。
- 長さは最初と最後の PTS から、seek は PTS で二分（key は H.264 の IDR・random access indicator）。
- 切れた file・同期の外れ（0x47 を探し直す）。

## 確認の予定

- host: `sh plan/ws177/tests/host-media-t.sh` の group（ffmpeg で作った file を ffprobe の packet と照合、切った file・壊れた file）。
- build warning 0、style-check。QEMU は p031 の後に T1 へまとめて。
