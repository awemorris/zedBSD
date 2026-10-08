---
id: bugs.alt-tab-order
title: Alt+Tab が今の app から開き、Tab で bar の並びの 1 つ右へ進む
status: active
areas: [compositor, switcher]
paths: [userland/desktop/wayland/switcher.c, userland/desktop/wayland/switcher-shell.c, userland/desktop/wayland/apps-bar.c]
machine: either
human: none
since: BUG-209
---

## 目的
BUG-209（Alt+Tab の順が bar の icon の並びと合わない）の決定の仕様（今の app から開き、Tab で右へ、端で回る、素早い Alt+Tab は開いたまま）を確かめる。

## 準備
AAT の image。

## 操作と確認
1. 操作: Files・Terminal・Text Editor の順に開く。
   確認事項: bar の並び。正解: `KWL APPS icon app=… x=…`（窓のある物）を x で並べた順。確認方法: log。
2. 操作: Alt を押したまま Tab。
   確認事項: 今の app（Text Editor）で開く。正解: `KWL SWITCH open via=keys index=N app=<Text Editor>`。確認方法: log、撮影 switcher。
3. 操作: もう一度 Tab、Esc、Alt を離す。
   確認事項: bar の 1 つ右（端なら左端）。正解: `KWL SWITCH step … app=<次>`、`KWL SWITCH cancel via=escape`。確認方法: log。
4. 操作: 素早い Alt+Tab（すぐ離す）、Esc。
   確認事項: 開いたまま。正解: `KWL SWITCH stay via=quick-alt …`。確認方法: log。

## 合格
2〜4 の全部。2・3 が違えば BUG-209 の再現（fail）。

## 注記
touch pad の 3 本指の tap の開き方は実機。 helper は `plan/tools/aat/scenarios/helpers_bugs.py`、一覧と読み方は `plan/agents/bug-ui-sweep-20261008.md`（q911）。
