<!-- awesome-plan project=zedbsd record=ws063p002 -->

# ws063-p002: 強制終了の試験、計測、回帰、規約の適合

Phase ID: `ws063-p002`
Parent: [WS063](../ws.md)
Status: planned
Queue: none

## 目的

root と作業 volume で、書き込みの途中に QEMU を強制終了して起動し直し、volume が UFS OK で fsync した内容が残ることを複数の時点で確かめる（`plan/tools/ufs/crash-test.sh`）。configure を測る。WS060・WS063 の変更の全体を規約で見直す。

## 受け入れ

WS063 の受け入れ。
