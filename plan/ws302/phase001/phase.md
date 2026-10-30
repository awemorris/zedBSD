<!-- awesome-plan project=zedbsd record=ws302-p001 -->

# ws302-p001: DTB の検討と設計

Status: in-progress
Parent: [WS302](../ws.md)
Queue: q1775-i01（P5）

## 範囲

ws.md の p001 の行のとおり。どの統計からどの性質を推すか、sched への hint の口（kernel の中、HAL を変えない）、5330（P 2・E 8）の割り付けの方針、計測と受け入れの試験、Phase の表。header の案は `proposed/` に。design-reviewer を 1 回通す。ユーザーの判断が要る点は [../user-questions.md](../user-questions.md) に。source は変えない。

## 進み（2026-10-31 P5）

- 設計 [../design.md](../design.md) 第 1 版（main 3d820d9ca の code を読んで書いた）、header の案 [../proposed/include/kern/sched-hint.h](../proposed/include/kern/sched-hint.h)・[../proposed/include/kern/dtb.h](../proposed/include/kern/dtb.h)（host の cc で `-fsyntax-only -Wall -Wextra`、`CONFIG_DTB=0/1` の両方で通る事だけを確かめた）。
- 問い Q302-1〜7（[../user-questions.md](../user-questions.md)）。Q1 への点は design.md §10.2。
- design-reviewer の結果と反映は [../design-review.md](../design-review.md) と design.md §14。

## 受け入れ

- design-reviewer の指摘の反映（済みかは design.md §14）。
- ユーザーの review（Q302-1〜7 の答え）。これが来るまで cleared にしない。

## 後の Phase への持ち越し

- p002: `CONFIG_DTB=n` の object が今と同じ事の確かめ（sched.o・syscall.o・gpu.o・input.o）。
- p003: 手渡しの後継の 4 か所と tick の頭の `complete_handoff`、新しい task の入口から始まる後継の扱い（design.md §4.6.5）。
- p006: 5330 の CPU の番号の並びと SMT の兄弟の対を最初に記録（design.md §12）。
