---
id: desktop.language.compositor-japanese
title: 表示の言語を日本語にすると、desktop（system bar・音量・network・Wiseview・lock）の文が再起動なしで日本語になる
status: draft
areas: [compositor, language, settings]
paths: [userland/desktop/wayland/language.c, userland/desktop/libkeiland/translate.c, userland/desktop/libkeiland/translate-follow.c, userland/desktop/locale/]
machine: either
human: look
since: ws158-p003
---

## 目的
WS158 の翻訳の仕組み（libkeiland の kl_tr、catalog `wayland.tr`、設定 `ui.language`）で、compositor の文が logout も再起動もなしに日本語に切り替わり、英語に戻ることを確かめる。

## 準備
desktop（os.boot.session-up）。`/usr/share/keiland/locale/ja/wayland.tr` がある。表示の言語は英語（`ui.language` 0）。Settings の Languages の頁に「Display language」の選択が入るまでは、`keiland-settings set ui.language 1` を user として流して変える。

## 操作と確認
1. 操作: 表示の言語を日本語にする（`keiland-settings set ui.language 1`）。
   確認事項: 切り替え。正解: session の log に `ZWL LANGUAGE language=ja from=setting error=0`。確認方法: `aat wait-log 'ZWL LANGUAGE language=ja'`。
2. 操作: system bar を撮る。
   確認事項: 時計。正解: `10月5日(月)  14:05` の形（月・日・曜日が日本語）。確認方法: 撮影（人が見る）。
3. 操作: 音量の icon を click。
   確認事項: popup の文。正解: 「サウンド」「消音」。確認方法: log `ZWL VOLUME popup open`、撮影。Esc で閉じる。
4. 操作: network の icon を Alt を押しながら click。
   確認事項: 詳細の文。正解: 「状態」「IPv4 アドレス」「MAC アドレス」などの見出し。確認方法: log `ZWL NETWORK info open`、撮影。Esc。
5. 操作: 窓を 1 つ開いて Windows+Tab。
   確認事項: Wiseview の文。正解: 「Wiseview  -  1 個のウインドウ」「ほかのウインドウはありません」。確認方法: log `ZWL WISEVIEW opening`、撮影。Esc。
6. 操作: Super+L。
   確認事項: lock の画面。正解: 欄の hint が「パスワード」。確認方法: log `ZWL LOCK locked`、撮影。password で戻る。
7. 操作: 表示の言語を英語に戻す（`keiland-settings set ui.language 0`）。
   確認事項: 戻り。正解: `ZWL LANGUAGE language=en from=setting error=0`、bar の時計が `Mon Oct 5  14:05` の形。確認方法: log、撮影。

## 合格
1・7 の log。2〜6 の日本語の見えは needs-person。

## 注記
login の前の画面（greeter）は system の言語（`/etc/keiland/language` の 1 行、`ja` か `en`）。それを変える管理者の操作は Settings に後で入る。画面 keyboard の tool の文はもともと日本語（WS102）で、この catalog の対象外。errno の文（`strerror`）は英語のまま。
