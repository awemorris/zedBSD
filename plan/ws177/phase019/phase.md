<!-- awesome-plan project=zedbsd record=ws177-p019 -->

# ws177-p019: Browser の IME・form と shell の fd（案 O）

Parent: [WS177](../ws.md)
Status: test-wait（2026-10-08 P1 q890 の 3: 実装・host PASS（plain・ASan）・zedBSD と Linux の build warning 0。QEMU は T1）
Disposition: normal
Primary Milestone: MG006（WS から継承）
Queue / attempts: q890 の 3（P1、2026-10-08）
Origin: [backlog-p1](../backlog-p1.md) の 12・13・29〜32（ws131-p025・ws090-p025）、[案](../phasing-20261008.md) の O。2026-10-08 朝 ユーザー「libbrowser に口を足してよい（描画の改善は止めたまま）」

## 設計と変更（2026-10-08 P1）

- **12 網の descriptor が 64 を超える**: shell が view から取る descriptor を 256 まで（`SHELL_NET_FDS`）にし、application（libkeiland、`KL_APP_FDS_MAX` 64）が watch できなかった物は、dispatch の後に shell が自分で `poll(…, 0)` して revents を入れる（`window_poll_unwatched`）。その間は待ちを 10 ms までに縮める。数が変わった時に `ZBROWSER NET fds=N watched=M polled=K` を log に出す。前は watch できない descriptor は黙って外れ、loader の 30 秒の timeout でしか進まなかった。`watched[]` の大きさは `SHELL_WATCHED_MAX`（= `KL_APP_FDS_MAX`）で揃えた。
- **13 POLLERR・POLLNVAL の区別**: 判断: **要らない**。libbrowser の loader は revents が 0 でないことだけを見て、読み・書きの結果で誤りを知る（`KL_APP_FD_HANGUP` → `POLLHUP` で同じに動く）。shell が自分で poll する descriptor は poll の revents をそのまま渡す。libkeiland の ready に error を足す変更はしない。
- **29 surrounding text・content type**: libkeiland に `kl_window_text_context(window, text, cursor, anchor, hint, purpose)`（**KL_VERSION 68**、`KL_TEXT_PURPOSE_*`・`KL_TEXT_HINT_MULTILINE`・`KL_TEXT_SURROUNDING_MAX` 4000）。text-input-v3 の `set_surrounding_text`・`set_content_type` を、変わった時と enable の時に送る（4000 byte を超える text は caret の周りを文字の境で切る）。前は enable の時に NONE・NORMAL を送るだけ。libbrowser に public の `browser_view_text_context(view, text, size, &cursor, &purpose, &hints)`（`BROWSER_TEXT_PURPOSE_*`・`BROWSER_TEXT_HINT_MULTILINE`、数は text-input-v3 の物）: focus の control の値（default の値も）と caret の byte、`inputmode`（numeric・decimal・tel・email・url）か `type`（number・tel・email・url）の purpose、textarea の multiline。browser の shell が毎 frame（`shell_text_input`）に渡す。
- **30 選んだ文節**: `dom_control` に `preedit_begin`、`page_compose` が begin を受ける。`browser_view_compose` の begin < end の範囲を、全体の細い下線に加えて 3 倍の太さの下線で描く（`paint/list.c` の `list_preedit_underline`。描画の改善ではなく IME の表示の分岐の追加）。
- **31 合成中の click・script の value**: click で caret を動かす前（`page_place_caret`）に合成中の text を値に入れる（typing と同じく input が起きる、`form_compose_finish`）。script が value を書いた時（`dom_input_set_value`）は合成を消す。IME の側の合成の状態は次の surrounding text（29）で伝わる。
- **32 textarea の value**: `bind/html-textarea.c`（新）で HTMLTextAreaElement の `value` の getter・setter（`dom_control_value`・`dom_input_set_value`）。`bind/internal.h` の `BIND_HTML_TEXT_AREA_ELEMENT`、`bind/node.c` の tag の対応、`bind/window.c` の interface の一覧、Makefile。
- [browser component の規則](../../standards/browser-component.md): 新しい public の口は C の文字列と数だけ（Wayland の型を出さない）。`BROWSER_API_VERSION` は 2 のまま（export の追加のみ）。libkeiland の exports.map を `exports.py` で作り直した。

## 確認（host・build、2026-10-08）

- `sh plan/ws177/tests/host-browser-o.sh plain` と `asan` → PASS 13: textarea の value の get・set（`notes=hello`、`x\ny`）、email の purpose、inputmode=numeric が type より勝つ、textarea の context（`x\ny`、caret 3、multiline）、値と caret の byte（`ab日` → 5）、長い値を 999 byte に切る、選んだ文節の太い下線 1 本と細い下線、範囲が無い時は太い下線が無い、click で合成中の「あいう」が値に入り input が起きる、script の value が合成を消す。
- `sh plan/ws177/tests/host-shell-fds.sh`（window.c と libkeiland の fake、ASan/UBSan）→ PASS 8: 80 個のうち 64 を watch、残り 16 を shell が poll（読める物に POLLIN、閉じた物に POLLHUP）、待ちを 10 ms までに、log、少なくなれば元の待ち。
- 回帰: `plan/ws090/tests/host-browser-ime.sh`（28 checks 0 failed）、`plan/ws177/tests/host-focus-field.sh`（PASS）、`plan/tools/browser-component/run.sh plain`（83 checks PASS）。
- build（warning 0）: zedBSD `make -j16 BUILD=build/p1-mailer ZEDBSD_CONFIG=plan/ws169/tests/config-amd64-mailer.mk build/p1-mailer/dynamic/libbrowser.so build/p1-mailer/dynamic/libkeiland.so build/p1-mailer/bin/browser`、Linux `make -f userland/desktop/keiland-linux.mk KEILAND_LINUX_BUILD=build/p1-linux all`。style-check: 変えた file の指摘は変更前と同じか少ない（form.c 2・list.c 2・view.c 4 は既存、window.c は 3 → 0）。

## 未実施

- QEMU（T1）: Browser の欄で IME（Mozc 互換の日本語の IME）の候補・OSK の layout が purpose に従うか、文節の太い下線、click で合成が値に入るか。zedBSD の IME が surrounding text を使う所（keyboard.c の reading）は見ていない。
- 64 を超える descriptor の実際の page（多数の host への同時の接続）は QEMU で見ていない（host の fake の試験だけ）。
- anchor（選択の他端）は libbrowser の control に選択が無いので caret と同じを送る。

## Event

2026-10-08 / q890-i03（P1）: 実装と host・build の確認。
