---
id: apps.music.failures
title: Music の準正常系（音の service の喪失と開き直し、file の消失、検索の field の Escape、Next の連打、Files から開けない file）
status: active
areas: [music, audio]
paths: [userland/desktop/music/, userland/desktop/videoplayer/audio.c]
machine: either
human: none
since: ws177-p021
---

## 目的
再生中に音の service（audiod）が去った時に stream を開き直して同じ位置から続けること、曲の file が消えた時に知らせて次の曲へ進むこと、検索の field を Escape で離れた後の Space が再生・一時停止になること、Next の連打を 1 回の曲の切り替えにまとめること、Files から開いた鳴らせない file の理由を知らせることを確かめる（ws177-p021）。

## 準備
`apps.music.play` と同じ（kei の `~/Music/AAT/` に Tone A・Tone B、QEMU は音の device 付き）。加えて 3 曲目 `03-tone-c.m4a`（同じ作り方、題 Tone C、番号 3）と、音でない file `/home/kei/not-a-song.m4a`（中身は `not an mp4`）。

## 操作と確認
1. 操作: Music を開き、Play（Tone A）。3 秒待つ。root で audiod を SIGKILL で止める（zedBSD に pkill は無い: `ps -A -o pid,args` で `/sbin/audiod` の pid を探して `kill -KILL`。audiod は `restart=on-failure` で起き直る）。
   確認事項: 開き直し。正解: `MUSIC AUDIO lost song=0 ms=`（2000 以上）、`MUSIC AUDIO reopened song=0 error=`。error=0 なら `MUSIC PLAY seek to_ms=` が lost の ms の近くで、その後の `MUSIC POSITION song=0` が進む。error が 0 でなければ notice「There is no sound: the sound service is not running.」（log `MUSIC NOTICE text=`）。その場合は 3 秒待って Space → `MUSIC PLAY song=0 error=0`（次の再生で開き直す）。確認方法: log。
2. 操作: 下の bar の次（▶▶|）を素早く 2 回（同じ frame の間に）。
   確認事項: 連打のまとめ。正解: `MUSIC STEP step=2 requests=2` と `MUSIC PLAY song=2 error=0`（`song=1` の PLAY が無い）。2 回が別の frame に分かれた時は `STEP step=1 requests=1` が 2 つ（それも可、記録する）。確認方法: log。
3. 操作: 左の上の Search の field を click し、Escape、続けて Space。
   確認事項: field を離れる。正解: Space で `MUSIC PAUSE song=` か `MUSIC RESUME song=`（field に空白が入らない）。確認方法: log、撮影。
4. 操作: root で `touch -r /home/kei/Music/AAT /tmp/aat-music-time`、`/home/kei/Music/AAT/02-tone-b.m4a` を `/home/kei/` へ mv、`touch -m -r /tmp/aat-music-time /home/kei/Music/AAT`（folder の時刻を戻し、5 秒ごとの見直しが click の前に曲を消さないように）。Music の一覧で Tone B（2 行目）を double click。
   確認事項: file の消失。正解: `MUSIC GONE song=1`、`MUSIC RESCAN songs=2`、`MUSIC NOTICE text=This song's file is gone.`、続いて `MUSIC PLAY song=1 error=0`（Tone C、消えた曲の次。その前の `MUSIC PLAY song=1 error=6` は開けなかった印）。確認方法: log、撮影。
5. 操作: Music を閉じ、kei として `/bin/music /home/kei/not-a-song.m4a`。
   確認事項: Files から開けない file の理由。正解: `MUSIC FILE song=-1 error=3`（zedBSD の EINVAL）と `MUSIC NOTICE text=This file is not a song Music can play.`（音の無い MP4 なら error=21（EOPNOTSUPP）と「This file has no sound Music can play.」、読めない file なら「The file could not be read.」）。確認方法: log、撮影。

## 合格
1〜5 の正解（1 は error=0 の続き、または notice の後の再生のどちらか）。

## 注記
助け: `plan/tools/aat/scenarios/helpers_music.py` の `apps.music.failures`（3 曲と not-a-song.m4a を作って置き、終わりに guest の中で消す）。片付け: 4 で mv した file、`not-a-song.m4a`、`/tmp/aat-music-time`、`~/Music/AAT` を消す。decode の失敗（16 個続けて decode できない）と読みの失敗は host の試験（`plan/ws177/tests/host-music-play.sh`）で確かめる。
