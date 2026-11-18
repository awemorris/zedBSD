---
id: bugs.browser-url-scheme
title: Browser の URL の欄を編集しても scheme のある URL に file:// が付かない
status: active
areas: [browser]
paths: [userland/desktop/browser/shell/titlebar.c]
machine: either
human: none
since: BUG-206
---

## 目的
BUG-206（https の頁で URL を編集すると https:// の前に file:// が付く）を、欄の文字の長さで確かめる。

## 準備
AAT の image。`/tmp/aat-work/b206.html` を helper が書く。https は guest に network がある時だけ、無ければ `data:` の URL で同じ経路を見る。

## 操作と確認
1. 操作: Browser で file の頁を開き、URL の欄を click、End、Enter。
   確認事項: file の頁の欄は `file://` と path。正解: `KWL TITLEBAR text … done=1 … length=N` の N が `file://` ＋ path の長さ。確認方法: log、撮影 editing-0。
2. 操作: 欄に `https://example.com/` を入れて開き、欄を click、End、Enter。
   確認事項: scheme のある URL のまま。正解: N が `ZBROWSER TITLEBAR … path=P` の P の長さと同じ（file:// の 7 文字が増えない）。確認方法: log、撮影。
3. 操作: 欄に `data:text/html,<p>aat206</p>` を入れて開き、同じく。
   確認事項: 同じ。正解: 同じ。確認方法: log、撮影。

## 合格
file の頁と、scheme のある頁を少なくとも 1 つ確かめ、全部で長さが合う。合わなければ BUG-206 の再現（fail）。

## 注記
network が無く data: も開けない時は fail（note に「scheme の頁を開けない」）。 helper は `plan/tools/aat/scenarios/helpers_bugs.py`、一覧と読み方は `plan/agents/bug-ui-sweep-20261008.md`（q911）。
