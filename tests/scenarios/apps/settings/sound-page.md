---
id: apps.settings.sound-page
title: Sound の頁の slider と bar の音量が合う
status: active
areas: [settings, sound, audiod]
paths: [userland/desktop/settings/sound.c, userland/base/audiod/]
machine: either
human: look
since: ws131-p004
---

## 目的
Sound の頁が audiod に繋がり、値が bar と合うことを確かめる（UAT D2）。

## 準備
Settings の Sound の頁。

## 操作と確認
1. 操作: 頁を撮る。
   確認事項: 繋がり。正解: `ZSETTINGS SOUND open live=1 reachable=1 value=V muted=…`、V が bar の音量（`ZWL VOLUME`）と同じ。確認方法: log、撮影。

## 合格
reachable=1 で値が合う。音の device が無い（QEMU の構成による）時は reachable=0 を記録して needs-person。

## 注記
slider の drag は desktop.bar.volume-slider と同じ規則（BUG-170）。音が鳴るかは UAT。
