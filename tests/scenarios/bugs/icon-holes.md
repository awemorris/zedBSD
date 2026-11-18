---
id: bugs.icon-holes
title: app の icon の記号の部分が後ろ（壁紙・景色）を見せる（目視）
status: active
areas: [compositor, appearance, home]
paths: [userland/desktop/wayland/glass.c, userland/desktop/wayland/home.c]
machine: either
human: look
since: BUG-237
---

## 目的
BUG-237（icon の白抜きの部分が透過になっていない）を App Home・Alt+Tab・dark の外観で撮る。

## 準備
AAT の image。helper が `appearance.dark` の元の値を覚えて戻す。

## 操作と確認
1. 操作: App Home を開いて撮る。
   確認事項: 記号が壁紙を見せる。正解: 白でない。確認方法: 撮影 home-light。
2. 操作: Files・Terminal を開き、Alt を押したまま Tab で switcher を撮る。
   確認事項: 記号が blur の景色を見せる。正解: 白でない。確認方法: 撮影 switcher。
3. 操作: `appearance.dark 1` で App Home を撮る。
   確認事項: dark の外観でも同じ。正解: `KWL THEME appearance=1`。確認方法: log、撮影 home-dark。

## 合格
人が撮影を見て判断（needs-person）。

## 注記
なし。 helper は `plan/tools/aat/scenarios/helpers_bugs.py`、一覧と読み方は `plan/agents/bug-ui-sweep-20261008.md`（q911）。
