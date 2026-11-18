---
id: bugs.browser-tap-button
title: Browser で HTML の button を touch screen の tap で押せる
status: active
areas: [browser, touch]
paths: [userland/desktop/browser/shell/touch.c]
machine: either
human: none
since: BUG-182
---

## 目的
touch の tap が click になり、HTML の中の button が押されることを確かめる（BUG-182: 実機でタップがクリックにならず button を押せなかった）。

## 準備
AAT の image。`/tmp/aat-work/b182.html`（窓の幅いっぱい・高さ 420 px の button が 1 つ、onclick で `console.log('aat-bug182-click')`）を helper が書く。

## 操作と確認
1. 操作: Browser を `/bin/browser /tmp/aat-work/b182.html` で開く。
   確認事項: 頁の表示。正解: `ZBROWSER READY` か `ZBROWSER NAVIGATE`。確認方法: log。
2. 操作: button を pointer で click（比べるため）。
   確認事項: button が押される。正解: `ZBROWSER CONSOLE level=N aat-bug182-click`。確認方法: log。
3. 操作: 同じ所を touch screen で tap（`aat tap`）。
   確認事項: tap が click になる。正解: `ZBROWSER TOUCH tap x=… y=…` の後に 2 つ目の `ZBROWSER CONSOLE … aat-bug182-click`。確認方法: log、撮影 tapped。

## 合格
3 で console の行が出る。出なければ BUG-182 の再現（fail）。2 が出ない時は頁の問題で、判定しない。

## 注記
実機の touchpad の tap（5330）は別の経路（compositor の touchpad.c）。この scenario は touch screen の経路。 helper は `plan/tools/aat/scenarios/helpers_bugs.py`、一覧と読み方は `plan/agents/bug-ui-sweep-20261008.md`（q911）。
