<!-- awesome-plan project=zedbsd record=ws345-p015 -->

# ws345-p015 headless の `_OWN` の道（`--threads`）

- WS: [ws.md](../ws.md)。設計: [design.md](../design.md) §11 の R12。
- 状態: planned
- 出所: 2026-11-14 Q1 の判断（p011 の手順 3 が求める headless の `--threads` がどの Phase にも無かった。P3 の報告から新しい Phase に）。

## 目的

browser の headless の mode（`--run`・`--dump=*`・`--render`・`--render-gpu`）を `BROWSER_THREADING_OWN` の view でも回せるようにし、WPT の reftest の見本・DOM の試験・component・段 1 の host の試験が出荷する道（`_OWN`）を通るようにする（R12）。

## 背景

- 今の headless の mode は `browser_view_settle`（仮想の時計で timer を予算まで回す）・`browser_view_dump`・`browser_view_draw_pixels` に頼る。これらは `_OWN` の view では `ENOTSUP`（design.md §5 の表）。
- 二つの道は front（積むか、直に呼ぶか）だけが違い page の code は同じ、というのが R12 の前提。だから settle と dump も front の命令にして script の thread で同じ code を走らせれば、二つの道で同じ結果になるはず。

## 手順

1. `browser_view_settle`・`browser_view_dump` を `_OWN` の view でも受ける: front が命令として mailbox に積み、script の thread が今の `_CALLER` の code（仮想の時計の予算、`BROWSER_SETTLE_LAYOUT`、dump の文字列）を走らせ、呼んだ thread は返事を待つ（待つ間も描画の thread と他の命令は止めない）。`browser.h` の説明を合わせ、公開 API の変更の理由を design.md に書く（HAL ではないので承認は要らない、Q1）。
2. `_OWN` の view の絵は `browser_view_render_pixels`（確定した frame）で取る。settle の返事の前にその時の状態で確定する。
3. `userland/desktop/browser/main.c` に `--threads`（headless の各 mode の view を `_OWN` で作る）。
4. component の client（`plan/tools/browser-component/client.c`）にも同じ選択（引数か環境）を足す。

## 試験

- host: `host-threaded-view` に settle・dump の `_OWN` の check、ThreadSanitizer の variant。
- R12: WPT の reftest の見本・DOM の試験（`run-dom-tests.py`）・component・`plan/tools/browser-win32/stage1.sh` の host の試験を `--threads` 有りと無しで流し、結果が同じ（違えば原因）。
- 回帰: host-build（plain・tsan）warning 0、zedBSD 3 機種の build warning 0、cross-build。

## 受け入れ

- 上の試験が通り、`--threads` 有りと無しで DOM・WPT の見本・component・段 1 の結果が同じ。
