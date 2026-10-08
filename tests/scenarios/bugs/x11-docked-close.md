---
id: bugs.x11-docked-close
title: dock した X11 の窓（glxtest）を bar の close の button で閉じると glxtest が終わる
status: active
areas: [compositor, x11]
paths: [userland/x11/glxtest/, userland/desktop/xserver/, userland/desktop/wayland/shell.c]
machine: qemu
human: none
since: BUG-273
---

## 目的
BUG-273（x11-p004 の段 3: docked の glxtest を bar の close で閉じても終わらない）が compositor の側か試験の座標かを分ける。

## 準備
image は `plan/tools/aat/config-amd64-aat-bugs.mk`（`/bin/glxtest` 入り）。kei で `DISPLAY=:0 /bin/xserver` と `glxtest` を helper が立て、終わりに止める。

## 操作と確認
1. 操作: glxtest の窓の title bar を double click で dock。
   確認事項: dock。正解: `KWL GLASS dock surface=S … buttons=X,…`。確認方法: log、撮影 glxtest・docked。
2. 操作: bar の close の button（buttons の最初の x、y=22）を click。
   確認事項: close が届き glxtest が終わる。正解: `KWL GLASS close surface=S`、5 秒以内に glxtest の process が無い。確認方法: log、ps、撮影 closed。

## 合格
2 の両方。close の行が無い → press が button に当たらない、行があり glxtest が残る → X の窓の close の経路（どちらも fail、note で分ける）。image に glxtest が無い時は needs-person。

## 注記
x11-p004 は y=17 を押す。この scenario は aatlib.close と同じ y=22。 helper は `plan/tools/aat/scenarios/helpers_bugs.py`、一覧と読み方は `plan/agents/bug-ui-sweep-20261008.md`（q911）。
