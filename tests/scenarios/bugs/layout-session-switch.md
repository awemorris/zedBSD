---
id: bugs.layout-session-switch
title: 最大化は session の状態: 切り替え先の窓がその mode に合わせて dock・float する
status: active
areas: [compositor, windows]
paths: [userland/desktop/wayland/shell.c, userland/desktop/wayland/layout.c]
machine: either
human: none
since: BUG-217
---

## 目的
BUG-217（最大化を session の状態として扱う。切り替え先も最大化し、窓の mode の app から切り替える時は窓にする）を確かめる。

## 準備
AAT の image。

## 操作と確認
1. 操作: Files を開いて dock、Terminal を開く。
   確認事項: Terminal も dock で開く。正解: `KWL GLASS open-docked …`。確認方法: log。
2. 操作: bar の restore の button で窓の mode に戻し、Alt+Tab で Files へ。
   確認事項: Files が浮く。正解: `KWL LAYOUT mode=windowed`、`KWL LAYOUT switch surface=<Files> action=float mode=windowed` か `action=keep`（restore の button で mode を出ると全部の窓が静かに浮く `KWL LAYOUT float-quiet` ので、Files は既に浮いていて keep になる。2026-10-09 P1、T1-481）。確認方法: log、撮影 files-floating（Files が title bar の付いた窓）。
3. 操作: Files を double click で dock し、Alt+Tab で Terminal へ。
   確認事項: Terminal も dock する。正解: `KWL LAYOUT switch surface=… action=dock mode=docked`。確認方法: log、撮影 terminal-docked。

## 合格
2・3 の行が出る。無ければ BUG-217 の再現（fail）。

## 注記
touch pad の gesture での切り替え（Wiseview）は実機。 helper は `plan/tools/aat/scenarios/helpers_bugs.py`、一覧と読み方は `plan/agents/bug-ui-sweep-20261008.md`（q911）。
