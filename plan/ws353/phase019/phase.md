<!-- awesome-plan project=zedbsd record=ws353-p019 -->

# ws353-p019: printers の 1 s の見回りを消す（printd の push と、一覧は設定から）

Status: planned
Parent: [WS353](../ws.md)

## 目的

2026-11-14 のユーザーの方針（ws.md「ユーザーの決定（続き）」）: printd は印刷の spool の間だけ起動する（前からの実装）。printd が起動している間は pipe で printd に繋いで push を受ける。kernel に file の変化の通知などを足さない。printer の一覧を得るのに printd と通信する必要は無く、Settings が printd と話すのは spool の中身を見る時だけ。

今は kl_system を開く app（Settings・Files・Photos・Monitor・pdfviewer、libkeiland の ui/app.c）が開いている間、compositor の printers-shell が設定の file を 1 s ごとに見る（設計の対応表 2-04、p007 の記録）。

## 範囲

1. 調べ: printers の設定の file を誰が書くか（Settings・compositor の io-file・printd）、printers の object を作る所（libkeiland）、printd の起動と終わり、今の 1 s の見回りが拾っている変化は何か。
2. 設計（この phase.md に追記）: 一覧と設定の変化を、書く側（compositor 自身か、それに要求する client）から事象で知らせる形。printd が起動している間の spool の状態は printd への pipe の push で。libkeiland は printers の object を使う時だけ作る。
3. 実装と試験（host、QEMU で Settings を開いた間に printers の timer が 0、printer の追加・削除・印刷の状態が見える）。
4. HAL・Makefile・mk/・toolchain は変えない。kernel を変えない。

## 受け入れ

- printers の 1 s の見回りが無い（Settings などを開いた間も）。
- 設計の対応表の 2-04 を「消えた」に直す。
