<!-- awesome-plan project=zedbsd record=ws345-p007 -->

# ws345-p007 shell（三つの platform）

- WS: [ws.md](../ws.md)。設計: [design.md](../design.md)。
- 状態: planned

## 目的

browser の shell（zedBSD・Linux の Keiland、Windows の OmniGrammaton に共通の `browser/shell/`）を `BROWSER_THREADING_OWN` にし、窓の thread は event だけ、描画は shell の描画の thread で行う（D1・D5、design.md §6.3）。

## file

- `userland/desktop/browser/shell/shell.c`・`window.c`・`present.c`・`present-cpu.c`・`touch.c`・`mail.c`・`internal.h`、新しい `shell/render.c`（描画の thread、kl_thread で作る shell の thread）。
- `apps/browser-win32/`（必要なら。WM_COPYDATA の道は今のまま窓の thread）。
- libkeiland の resize の口は p013（この Phase は p013 に依存）。

## 手順

1. view を `_OWN` で作る。窓の loop から `browser_view_prepare`・`shell_frame` を外し、描画の thread へ。
2. surface は窓の thread で作り、device・swapchain・present は描画の thread。resize は p013 の口で、ack を描画の thread の新しい大きさの最初の present の直前に（R8）。止め方: acquire の時間切れ 100 ms、閉じる順（描画の thread の join → swapchain・surface → 窓 → view）、Windows は join の間も message を回す（design.md §6.3）。CPU の道は描画の thread が描いて窓の thread が shm の present。
3. drag の画像は `browser_view_request_image` と callback、mail は `_focus_field` の答えの callback、文字入力は session の番号付きの commit、touch の scroller は snapshot と命令。
4. Windows: system の modal loop の間も描画の thread が描く事を確かめる。

## 試験

- zedBSD の QEMU（Q1 が割り当てる名前・port）: `plan/tools/boot-test.sh` と browser の頁の画面（`plan/ws074/tests/browser-guest.sh` の道、画面の PNG で判定）。長い script の頁（probe の loop の頁を 30 s に）を開いて、その間に titlebar の reload・location の操作と resize が効く事を画面の PNG で。
- Windows: Wine で同じ頁（plan/ws334 の Wine の道）。本物の Windows の確認はユーザーが拾える試験の一覧に（Q1）。
- Linux の Keiland（WS105 の guest、Q1 が割り当てる）: 同じ。
- 回帰（この Phase の終わりに 1 回）: `sh plan/ws074/tests/host-build.sh plain`（warning 0）、host-* の exit が前と同じ、js、http・http-async・loader・fetch、component（`plan/tools/browser-component/run.py`）、`sh plan/ws334/tests/stage1.sh TREE plain` の前後、`sh plan/ws334/tests/cross-build.sh`。

## 受け入れ

- 三つの platform で起動し、長い script と connect の掛かりの間も窓の操作（titlebar・resize・再描画）が効く（画面の PNG の証拠）。
