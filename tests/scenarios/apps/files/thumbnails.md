---
id: apps.files.thumbnails
title: Files と Settings の縮小画像を keiland-preview の子（sandbox）が作る
status: active
areas: [files, settings, sandbox]
paths: [userland/desktop/preview/, userland/desktop/files/thumb.c, userland/desktop/files/thumb-cache.c, userland/desktop/settings/look.c, src/kern/sandbox.c]
machine: either
human: look
since: ws168
---

## 目的
Files と Settings が画像・PDF の縮小画像を自分で復号せず、`keiland-preview` を `sandbox_spawn` で起こした子に作らせ、子が許された call だけで動くことを確かめる（WS168 p004）。

## 準備
AAT の samples（`/tmp/aat-samples` の sample.png・sample.jpg・sample.pdf）を kei の `~/AATThumbs` に複写し、壊れた PNG（signature の後に `broken`）を足す。縮小画像の cache（`~/.cache/keiland/thumbnails`）は空。

## 操作と確認
1. 操作: kei として `/bin/files /home/kei/AATThumbs`。
   確認事項: 子の縮小画像。正解: sample.png・sample.jpg・sample.pdf の `ZFILES THUMB path=… error=0 … cached=0 status=0 pid=<0 でない>`、broken.png は error が 0 でない。最初の終わり（`ZFILES THUMB path=…`）の前に `ZFILES THUMB start` が 2 つ（子は 2 つ同時、ws177-p010）。icon が絵（PDF は 1 頁目、埋め込まれていない Helvetica の「AAT page 1」が program の持つ font で描かれる）、壊れた PNG は種類の icon。確認方法: log、撮影。
2. 操作: Files を閉じ、もう一度開く。
   確認事項: cache。正解: sample.png の `ZFILES THUMB … error=0 … cached=1`、broken.png の `… cached=1 failed=1`（失敗の印、ws177-p010）。確認方法: log。
3. 操作: Settings の Wallpaper の頁。
   確認事項: 背景の tile。正解: `ZSETTINGS LOOK picture path=… error=0` が 1 つ以上、tile が絵。確認方法: log、撮影。
4. 操作: 1〜3 の前後で `dmesg | grep -c 'SANDBOX deny'`。
   確認事項: 子が集合の外の call をしない。正解: 数が増えない。確認方法: kernel の log。

## 合格
1〜4 の正解。撮影は人が見る（icon の絵、tile）。

## 注記
助け: `plan/tools/aat/scenarios/helpers_preview.py`（終わりに `~/AATThumbs` を消す）。
