---
id: apps.photos.browse
title: Photos で写真を取り込み、月ごとに見て、1 枚を全面に、回転・お気に入り・album・slideshow を確かめる
status: active
areas: [photos]
paths: [userland/desktop/photos/, userland/desktop/mediastorage/, userland/desktop/picture/]
machine: either
human: look
since: ws157
---

## 目的
写真が取り込みで media library（mediastorage）の `~/Pictures/Media/Files/YYYY/MM/dd/<元の名前>` に複写され（元の file は残る）、
`~/Pictures/Media/metadata.db`（JSON。`media` の各 record に id・path・sha256・taken・favorite・turns、`albums` に id・name・members）に入り、
撮影日の新しい順に月ごとの grid に出て、1 枚を全面に出し、前後へ移り、回転とお気に入りが record に、album が `albums` に残り、slideshow が進み、
同じ写真は 2 度取り込まれない（SHA-256）ことを確かめる（2026-10-07 ユーザーの要件）。

## 準備
host で `plan/tools/aat/make-photos.py --view` が 9 枚を作る（EXIF の日付が 2026 年 9 月 2 枚・8 月 3 枚・7 月 1 枚、EXIF の向きで回る JPEG（hill.jpg）、
EXIF の無い PNG・GIF・壊れた JPEG）。user の `~/AATPhotos` に置き、PNG・GIF・壊れた JPEG の file の時刻を `touch -t` で 7 月にする。
`~/Pictures/Media` と縮小画像の cache（`~/.cache/keiland/photos`）は無い。

EXIF の日付の無い file は取り込んだ日（guest の local の暦）の日付になる。file の時刻（mtime）は使わない（import.c、`plan/ws157/mediastorage-usage.md`）。
写真の番号（log の `photo=N`）は Photos の並び（撮影日の新しい順、同じ時刻は path の順。library.c）で、助けは metadata.db から計算する。
2026 年 9 月 20 日より後に流すと、0〜2 が取り込んだ日の 3 枚（同じ秒なら broken.jpg・sticker.png・wave.gif の順）、3 lake.jpg、4 hill.jpg、5 sea.jpg、
6 cake.jpg、7 park.jpg、8 garden.jpg。

Photos は library が変わる度（自分の取り込み・保存も）に読み直し（`PHOTOS MEDIA done imported=0 ...`）、そこで表示を Timeline に戻す。
助けは読み直しが止むのを待ってから操作する。

## 操作と確認
1. 操作: user として `/bin/photos --import=/home/user/AATPhotos`。
   確認事項: 取り込みと一覧。正解: `PHOTOS MEDIA done imported=1 error=0 snapshot=0`。metadata.db に 9 枚が 1 つずつ、`path` が
   `Files/2026/09/20/lake.jpg`・`Files/2026/09/12/hill.jpg`・`Files/2026/08/30/sea.jpg`・`Files/2026/08/03/cake.jpg`・`Files/2026/08/01/park.jpg`・
   `Files/2026/07/05/garden.jpg`、PNG・GIF・壊れた JPEG は `Files/<取り込んだ日>/`。`Files` の下に 9 つ、`~/AATPhotos` に 9 つ。
   EXIF のある 6 枚が新しい順。壊れた file の番号で `PHOTOS THUMB photo=<その番号> error=`（他の写真の THUMB の失敗は無い）。
   見出しは取り込んだ月・September・August・July。確認方法: log、metadata.db、find、撮影。
2. 操作: hill.jpg の cell（September の 2 枚目）を double click。
   確認事項: 全面。正解: `PHOTOS OPEN photo=<hill> name=hill.jpg`、`PHOTOS PICTURE photo=<hill> error=0 width=640 height=480`。確認方法: log、撮影。
3. 操作: →、R、（保存を待つ）、F。
   確認事項: 次・回転・お気に入り。正解: `PHOTOS OPEN photo=<hill+1> name=sea.jpg`、`PHOTOS TURN photo=<sea> turns=1`、metadata.db の sea.jpg の
   `turns` が 1。回転の保存の後の読み直しでも sea.jpg は全面のままで、F で `PHOTOS FAVORITE photo=<sea> on=1`、metadata.db の sea.jpg の
   `favorite` が 1・`turns` が 1。確認方法: log、file、撮影。
4. 操作: A、card の名前の欄を click、`Sea`、Enter。
   確認事項: album。正解: `PHOTOS CARD album photo=<sea>`、`PHOTOS ALBUM create name=Sea error=0`、`PHOTOS ALBUM add album=Sea photo=<sea> error=0`、
   metadata.db の `albums` が 1 つで、name が `Sea`、members が sea.jpg の id だけ。確認方法: log、file。
5. 操作: Esc、Space、待つ、Space、Esc。
   確認事項: slideshow。正解: `PHOTOS BACK photo=<sea>`、`PHOTOS SLIDESHOW on=1 photo=<sea>`、3 秒ごとに `PHOTOS SLIDE photo=<sea+1>`・
   `PHOTOS SLIDE photo=<sea+2>`、`PHOTOS SLIDESHOW on=0`。確認方法: log、撮影。
6. 操作: Photos を閉じ、もう一度 `/bin/photos --import=/home/user/AATPhotos`、読み直しが止んでから左の album（Sea）を click。
   確認事項: 重複・読み戻し・cache。正解: `PHOTOS MEDIA done imported=1 error=0 snapshot=0`、metadata.db の sha256 が閉じた時と同じ、
   `Files` の下は 9 つのまま、`PHOTOS LIST list=2 album=0`、`~/.cache/keiland/photos` に `<id>.ppm` が 8 つ（壊れた file の他）。確認方法: log、sha256sum、ls、撮影。

## 合格
1〜6 の正解。撮影は人が見る（月の見出し、壊れた file の灰色の四角、正立の hill.jpg、回った sea.jpg と Unfavorite、album の Sea と心の印）。

## 注記
助け: `plan/tools/aat/scenarios/helpers_photos.py`（host の PIL で写真を作り、user の `~/AATPhotos` に置き、始めと終わりに
`~/AATPhotos`・`~/Pictures/Media`・`~/.cache/keiland/photos` を消す）。
窓の中の位置は view.c の layout（左の列は窓の幅の 0.24（200〜260）、glass では 8 の隙間、grid は上から 72、月の見出しは 40、
cell は列の左から 20、約 150 の正方形が 6 の間隔で入るだけの列、月ごとに新しい行。左の列の行は上から 64、Timeline・Favorites が 44 ずつ、
見出し Albums が 36、最初の album の行が 48。album の card は幅 360 で真ん中、album が無い時の高さ 160、名前の欄は上から 56・左から 16・高さ 30）。
