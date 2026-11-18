---
id: bugs.opacity-frosted
title: Settings の窓の透明度は初期値 100%、Frosted glass の switch で panel が不透明・すりガラスになる
status: active
areas: [settings, compositor, appearance]
paths: [userland/desktop/settings/page-look.c, userland/desktop/wayland/settings.c]
machine: either
human: look
since: BUG-214
---

## 目的
BUG-214（slider は初期 100% なのに実際は透けている）の直し（slider は中身の不透明度、Frosted glass の switch）を確かめる。

## 準備
AAT の image。helper が `window.frosted` の元の値を覚えて終わりに戻す。

## 操作と確認
1. 操作: Settings を Appearance の頁で開く。
   確認事項: slider の値。正解: `ZSETTINGS LOOK open opacity=100`（既定）、撮影 page に 100%。確認方法: log、撮影 page。
2. 操作: Frosted glass の switch（control 3）を click（off）。
   確認事項: panel が不透明。正解: `ZSETTINGS LOOK set key=window.frosted value=0 error=0`、`KWL PREFERENCES key=window.opacity applied value=… panels=opaque`。確認方法: log、撮影 opaque。
3. 操作: もう一度 click（on）。
   確認事項: すりガラスに戻る。正解: `value=1`、`panels=glass`。確認方法: log、撮影 glass。

## 合格
2・3 の行が出て、撮影で slider が 100%、opaque で透けない。

## 注記
前の scenario が opacity を変えていると 1 の値が 100 でない（note に値）。 helper は `plan/tools/aat/scenarios/helpers_bugs.py`、一覧と読み方は `plan/agents/bug-ui-sweep-20261008.md`（q911）。
