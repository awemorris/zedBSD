---
id: apps.music.play
title: Music で ~/Music の m4a を再生し、次の曲・一時停止・曲の終わりを確かめる
status: active
areas: [music, audio]
paths: [userland/desktop/music/, userland/desktop/libmedia/aac.c, userland/desktop/media-app/, userland/desktop/videoplayer/audio.c, userland/desktop/mediafile/]
machine: either
human: look
since: ws120
---

## 目的
`~/Music` の m4a（AAC）が album ごとに一覧に出て、libmedia の自前 AAC-LC で decode され libkeiland の音の stream（compositor から audiod、WS191）で鳴り（再生の位置が進む）、次の曲・一時停止・再開・曲の終わり（最後の曲の後で止まる）が動くことを確かめる（WS120 p008・p009）。音そのものは耳で聞かない（QEMU）。

## 準備
host の ffmpeg で 8 秒の正弦波の m4a を 2 つ作る（440 Hz と 660 Hz、AAC、題 Tone A・Tone B、artist AAT、album AAT Tones、番号 1・2、Tone A に PNG の cover）。kei の `~/Music/AAT/` に置く。image に libmedia と audiod。AAC-LC では libavcodec の package は不要。QEMU では音の device が要る（無いと stream が `ENODEV` で開かず、`MUSIC AUDIO error=` が 0 でない。T1-301 の頃は audiod の直の client だった）: `plan/tools/guest/guest.sh start` に `--qemu-extra '-audiodev none,id=snd0 -device intel-hda -device hda-duplex,audiodev=snd0'`。

## 操作と確認
1. 操作: App Home から Music を開く。
   確認事項: 一覧。正解: `MUSIC LIBRARY songs=2 error=0`、`MUSIC AUDIO error=0`、`MUSIC COVER album=0 error=0`（cover は初めて描く時に file から読む、ws177-p020）、左に All Songs と AAT Tones（cover）。確認方法: log、撮影。
2. 操作: 右の上の Play。
   確認事項: 再生。正解: `MUSIC PLAY song=0 error=0`、`MUSIC PLAY open codec=aac-lc backend=libmedia`、3 秒ほどで `MUSIC POSITION song=0 ms=` が 2000 以上、下の bar に Tone A と cover・位置。確認方法: log、撮影。
3. 操作: 下の bar の次（▶▶|）。
   確認事項: 次の曲。正解: `MUSIC PLAY song=1 error=0`。確認方法: log。
4. 操作: Space、続けてもう一度 Space。
   確認事項: 一時停止と再開。正解: `MUSIC PAUSE song=1`、`MUSIC RESUME song=1`。確認方法: log。
5. 操作: 待つ（最大 15 秒）。
   確認事項: 曲の終わり。正解: `MUSIC PLAY ended`、`MUSIC ENDED song=1`、`MUSIC STOP song=1`（最後の曲なので止まる）。確認方法: log、撮影。
6. 操作: Music を開いたまま、kei として `mkdir /home/kei/Music/AAT/copy && cp /home/kei/Music/AAT/01-tone-a.m4a /home/kei/Music/AAT/copy/`。
   確認事項: folder の変化（ws177-p020）。正解: 10 秒以内に `MUSIC RESCAN songs=3 albums=2 error=0`（同じ番号の曲が別の folder にあるので別の album）。確認方法: log。
7. 操作: Music を閉じ、kei として `/bin/music /home/kei/Music/AAT/02-tone-b.m4a`（Files が `audio/mp4` を開く時の command）。
   確認事項: file から。正解: `MUSIC FILE song=` と `error=0`、`MUSIC PLAY song=` の同じ番号と `error=0`（6 の copy があるので番号は 1 とは限らない）。確認方法: log。

## 合格
1〜7 の正解。撮影は人が見る（cover、一覧、bar）。

## 注記
助け: `plan/tools/aat/scenarios/helpers_music.py`（host の ffmpeg で m4a を作り、kei の `~/Music/AAT` に置き、終わりに消す）。窓の中の位置は view.c の layout（左の列は窓の幅の 0.28（220〜300）、glass では 8 の隙間、Play は右の列の左から 216・上から 135、bar は下の 80、その真ん中の再生の button、次は +52）。
