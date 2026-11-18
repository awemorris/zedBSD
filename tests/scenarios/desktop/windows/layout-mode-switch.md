---
id: desktop.windows.layout-mode-switch
title: 最大化は desktop の tablet mode: 切り替え先も最大化、窓の mode では窓に
status: draft
areas: [compositor, windows, switcher]
paths: [userland/desktop/wayland/layout.c, userland/desktop/wayland/shell.c, userland/desktop/wayland/switcher-shell.c]
machine: either
human: none
since: ws142-p008
---

## 目的
最大化を窓ごとでなく session の状態（tablet mode）として扱うことと、最大化の mode を出た時に全部の窓が窓に戻ること（2026-10-07 の UAT、WS181 p002）を確かめる（BUG-217、ws142-p007 §1）。QEMU の自動の版は plan/ws142/tests/p010-guest.sh の 1.〜5.。

## 準備
Files と Terminal を開く（窓の mode）。

## 操作と確認
1. 操作: Terminal の title bar を double click。
   確認事項: mode。正解: Terminal が最大化、log `KWL LAYOUT mode=docked reason=double-click`。確認方法: log、撮影。
2. 操作: Alt+Tab+Tab で Files へ。
   確認事項: 切り替え先。正解: Files も最大化（`KWL LAYOUT switch … action=dock mode=docked via=switch`）、Terminal は描かれない。確認方法: log、撮影。
3. 操作: bar の restore の button。
   確認事項: mode と後ろの窓。正解: Files が窓に戻り、後ろで最大化のままだった Terminal も窓に戻る（`KWL LAYOUT leave via=button front=… quiet=1`、`KWL LAYOUT float-quiet surface=…`、`KWL LAYOUT windows … mode=windowed … docked=0`）。確認方法: log、撮影（最大化の窓が残っていない）。
4. 操作: Terminal の body を click。
   確認事項: 大きさ。正解: Terminal が前に来るだけで大きさは変わらない（`KWL LAYOUT switch … action=float` も `KWL GLASS undock` も出ない）。確認方法: log、撮影。
5. 操作: Terminal を最大化し、Terminal を閉じる。
   確認事項: 次の窓。正解: 最大化の mode が終わり、Files は窓のまま見える（`KWL LAYOUT leave via=closed`、`KWL LAYOUT front … action=dock` は出ない）。確認方法: log、撮影。

## 合格
各段の log の行と、撮影で最大化の間は今の app の窓だけが見え、最大化を出た後に最大化の窓が残っていないこと。
