---
id: apps.terminal.fullscreen-f11
title: Terminal の F11 で全画面にし、F11 と Super+↓ で戻る
status: active
areas: [terminal, compositor, windows]
paths: [userland/desktop/terminal/, userland/desktop/wayland/shell.c]
machine: either
human: look
since: BUG-194
---

## 目的
BUG-194（全画面から戻れない）が直ったままであることを確かめる。全画面から戻る key は compositor が持ち、F11 と Super+↓ の両方（2026-10-05 ユーザーの決定）。

## 準備
App Home から Terminal を開く。

## 操作と確認
1. 操作: F11。
   確認事項: 全画面。正解: Terminal が画面全体、bar が隠れる。確認方法: log `ZTERM FULLSCREEN key on=1`、`ZWL GLASS bar hidden fullscreen=1`、撮影。
2. 操作: F11。
   確認事項: 戻り。正解: 元の窓、bar が出る。確認方法: log `ZWL GLASS fullscreen-leave surface=S via=f11 error=0` か `ZTERM FULLSCREEN key on=0`、`ZWL GLASS bar shown`、撮影。

3. 操作: F11 で全画面にし、Super+↓。
   確認事項: 戻り。正解: 元の窓、bar が出る。確認方法: log `ZWL GLASS fullscreen-leave surface=S via=super-down error=0`、`ZWL GLASS bar shown`、撮影。

## 合格
1〜3 の log。見えは撮影。
