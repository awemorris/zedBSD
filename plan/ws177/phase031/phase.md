<!-- awesome-plan project=zedbsd record=ws177-p031 -->

# ws177-p031: videoplayer・Music を libmedia へ移す（案 T の 5）

Parent: [WS177](../ws.md)
Status: planned（2026-10-08 夜 P2 q906 で立てた）
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

## 確認の予定

- host: `sh plan/ws177/tests/host-media-t.sh` の group（ffmpeg で作った file を ffprobe の packet と照合、切った file・壊れた file）。
- build warning 0、style-check。QEMU は p031 の後に T1 へまとめて。
