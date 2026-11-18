---
id: apps.settings.display-language
title: Languages の頁で日本語を選ぶと Settings と Files がすぐ日本語になり、English で戻る
status: active
areas: [settings, files, i18n]
paths: [userland/desktop/settings/page-languages.c, userland/desktop/settings/look.c, userland/desktop/files/main.c, userland/desktop/locale/, userland/desktop/libkeiland/translate.c, userland/desktop/libkeiland/translate-follow.c]
machine: either
human: look
since: ws158-p004
---

## 目的
表示の言語（`ui.language`、WS158）を Settings の Languages の頁で変えると、Settings と Files の文がすぐその言語になることを確かめる。

## 準備
Settings の Languages の頁。

## 操作と確認
1. 操作: Display language の「日本語」の switch（control 5）を click。
   確認事項: 切り替え。正解: `ZSETTINGS LANGUAGES ui language=ja`、`ZSETTINGS LOOK language=ja`。確認方法: log、撮影（頁の見出し・sidebar が日本語）。
2. 操作: App Home から Files を開いて撮り、閉じる。
   確認事項: Files も日本語。正解: 撮影の文が日本語（`ZFILES LANGUAGE language=ja` は言語が変わった時の行で、起動の時に出るとは限らない）。確認方法: 撮影（sidebar の「よく使う項目」「ホーム」など）。
3. 操作: Settings の Languages の頁で「English」（control 4）。
   確認事項: 戻る。正解: `ZSETTINGS LOOK language=en`。確認方法: log。

## 合格
1・3 の正解、撮影で日本語の文。

## 注記
menu（compositor に送る）の言語の追従はまだ無い（ws158-p004 の残り）。言語の名前（English・日本語）は訳さない。
