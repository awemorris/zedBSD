---
id: desktop.dnd.photo-to-notes
title: Photos の写真を Notes の頁へ drag すると、落とした点に画像が入り、Notes の画像は窓の外へ出して戻すと動く
status: draft
areas: [compositor, dnd, photos, notes]
paths: [userland/desktop/photos/, userland/desktop/notes/main.c, userland/desktop/notes/picture-file.c, userland/desktop/picture/png-write.c, userland/desktop/libkeiland/ui/clipboard.c]
machine: either
human: look
since: ws189-p003
---

## 目的
画像の drag の元（Photos）と受け（Notes）（WS189 の決定「最初に足す型: 画像（PNG）」）: 型の一覧（file の URI と PNG）のうち Notes は画像を受け、落とす場所の枠が光り、
落とした点に置かれること。Notes の画像の app の中の移動は窓の中ではそのまま、窓の外へ出すと外の drag になり、自分の頁へ戻すと移動になること。

## 準備
host で `plan/ws157/tests/make-photos.py --view` の写真を kei の `~/AATPhotos` に置き、`/bin/photos --import=/home/kei/AATPhotos` で取り込む。Photos と Notes（新しい notebook）を
重ならないように開く。

## 操作と確認
1. 操作: Photos の grid の 1 枚目を押して 20 px 動かし、Notes の頁の中ほどへ動かして止め、撮る。`aat mark start` は押す前。
   確認事項: drag と枠。正解: `PHOTOS DND start photo=0`、`PHOTOS DND picture photo=0 bytes=N errno=0`（N > 0）、`KWL DATA drag start ... types=2 actions=1 icon=`（0 でない icon）、
   `KWL DATA drag state=copy`。撮影で pointer の下に写真の縮小、右下に緑の +、Notes の頁に accent の枠。確認方法: log、撮影（人が見る）。
2. 操作: 離す。
   確認事項: 置かれた画像。正解: `KWL DATA receive ... mime=image/png source=`、`NOTES DND drop bytes=N x=... y=...`（落とした点）、`NOTES EDIT insert page=0 kind=image`、頁に写真。確認方法: log、撮影。
3. 操作: Select の道具で入った画像を押し、窓の外（右の端の外）まで動かし、そのまま頁の別の所へ戻して離す。
   確認事項: 外の drag と移動。正解: `NOTES DND drag start object=... errno=0`、`KWL DATA drag state=move`（頁の上、自分の頁）、`NOTES EDIT move page=0 object=...`、`NOTES DND drag done dropped=1`、画像が離した所へ動く。確認方法: log、撮影。
4. 操作: 3 と同じく外へ出し、Photos の窓の上で止めて離す。
   確認事項: 受けない窓。正解: `KWL DATA drag state=refused`、`NOTES DND drag done dropped=0`、Notes の画像は元の所のまま。確認方法: log、撮影。

## 合格
1〜4 の log、撮影の枠と印（人が見る）。
