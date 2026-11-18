---
id: desktop.home.power-off-dialog
title: App Home の Power Off で desktop が暗くなり、Power Off・Restart・Log Out・Cancel を選ぶ
status: active
areas: [compositor, home, session]
paths: [userland/desktop/wayland/power-dialog.c, userland/desktop/wayland/power-layout.c, userland/desktop/wayland/home.c]
machine: either
human: none
since: ws099-p037
---

## 目的
App Home の Log Out が確認なしに session を終えないこと、Power Off の tile から暗くする dialog で選べることを確かめる（BUG-235、ws099-p037）。

## 準備
user の desktop。

## 操作と確認
1. 操作: Windows キーで App Home、`power` と打ち、Power Off の icon を click。
   確認事項: dialog。正解: desktop が暗くなり中央に Power Off・Restart・Log Out・Cancel、log `ZWL POWER dialog open source=home poweroff=1 restart=1`（ws131-p027 の後。sessiond がそれより前なら 0 で薄く描かれる）。session は終わらない。確認方法: log、撮影。
2. 操作: Esc。
   確認事項: 取り消し。正解: `ZWL POWER choice=cancel via=escape`、desktop が元に戻る。確認方法: log、撮影。
3. 操作: もう一度開き、card の外を click。
   確認事項: 取り消し。正解: `ZWL POWER choice=cancel via=outside`。確認方法: log。
4. 操作（エージェントが最後に、または人が）: もう一度開き、Log Out。
   確認事項: log out。正解: `ZWL POWER choice=logout`、`ZWL SESSION logout`、login の画面。確認方法: log、撮影。自動の補助は 1.〜3. だけ（4. は session を終えるので by-agent）。

## 合格
1.〜3. の log の行と撮影、4. の login の画面。Power Off で guest が止まることは別に（ws131-p027、T1）。
