---
id: bugs.browser-busy-scroll
title: Browser が頁を読み込む間も scroll に応える
status: active
areas: [browser]
paths: [userland/desktop/libbrowser/page/script.c, userland/desktop/libbrowser/view/view.c]
machine: either
human: none
since: BUG-207
---

## 目的
BUG-207（通信中に UI に応答しない）を、読み込み中の wheel で頁が scroll するかで確かめる。

## 準備
AAT の image、guest から Internet に届くこと（QEMU の user-net）。`/tmp/aat-work/b207.html`（300 行の縦長の頁）を helper が書く。

## 操作と確認
1. 操作: 縦長の頁を開き、URL の欄に `https://www.wikipedia.org/` を入れて Enter、直後に頁の上で wheel を下へ 12 notch。
   確認事項: 読み込み中に scroll する。正解: `ZBROWSER NAVIGATE path=…wikipedia` より前に `ZBROWSER FRAME scroll=N`（N > 0）。確認方法: log の順、撮影 loading・loaded。

## 合格
新しい頁の NAVIGATE の前に scroll した frame がある。無ければ BUG-207 の再現（fail）。頁が来ない（network 無し）時は needs-person。

## 注記
頁が速く来て wheel の前に NAVIGATE した時も scroll の frame が無いので fail になる。その時は note の時刻を見て人が判断する。 helper は `plan/tools/aat/scenarios/helpers_bugs.py`、一覧と読み方は `plan/agents/bug-ui-sweep-20261008.md`（q911）。
