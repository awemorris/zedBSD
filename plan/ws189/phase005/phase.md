<!-- awesome-plan project=zedbsd record=ws189-p005 -->
# ws189-p005: 規約の全文の見直し

Status: 見直し済み、Q1 の判定待ち（2026-10-09 P3 Sonnet が p001〜p004 の C を全文で読み直した。前の Haiku の見直しは uncleared）
Disposition: normal
Parent: [WS189](../ws.md)
Queue: —
Design: [plan/coding-style.md](../../../plan/coding-style.md) の全文

## 範囲

WS189 の p001〜p004 が変えた C（libkeiland の drag と drop、compositor の drag の印・spring-loading、app 6 つ、Mail の添付、host 試験 3 つ）の
WS189 の行を plan/coding-style.md の全文と照らして読み、書き方だけを直した。動作・API・ABI・計算の順序・error の値は変えていない。
共有の file（shell.c、ui.c、compose.c、find.c、data.c など）は WS189 の行（`ws189` の注釈、diff の hunk）だけを読んだ。

## 読んだ file

libkeiland/ui の clipboard.c・drop-look.c・present-shm.c・ui.c（kl_ui_pointer_cancel）、picture/png-write.c（全文）、
wayland の data.c・protocol.c・dnd-state.c・shell.c（kwl_glass_draw_drag_mark）・seat.c・main.c・heads.c・compose.c・apps-bar.c、
tests/data-probe/main.c、textedit の app.c・main.c・draw.c、files の window.c・ui-desktop-drag.c・dnd.c・main.c、photos の main.c・view.c、
notes の main.c・window.c・picture-file.c、pdfviewer の find.c・view.c・main.c、libbrowser の page/link.c・view/view.c、browser/shell/shell.c、
mailer の compose.c・view.c・main.c、plan/ws189/tests の host-png-write.c・host-dnd-state.c・host-mail-attach.c。

## 直した規則

- **条件が 3 つ以上の節を 1 行に並べた**（§6）: apps-bar.c の spring の点灯、files/ui-desktop-drag.c の drop_content、photos/main.c の ph_uri の plain と photos/view.c の view_drag_arm、
  notes/main.c の app_drag_out、pdfviewer/find.c の選択の中の判定（5 節と 3 節）、browser/shell/shell.c の drag の距離、
  libbrowser/view/view.c の browser_view_image_at、mailer/compose.c の compose_parameter（4 節と 8 節）、mailer/main.c の ml_uri_path。計 11 か所。
- **段落の見出しの comment と空行が無かった**（§5）: clipboard.c の answer の記憶と limit、data.c の drag_forget の後の mark と drag_pick の最後の loop、
  protocol.c の attach の offset（`if (icon)` の前）、data-probe/main.c の第 2 device の接続と 2 つの enter の `offer == NULL`、textedit/app.c の te_app_drop_text と app_drag_out、
  photos/main.c の ph_drag_start の guard、pdfviewer/main.c の白の fill、mailer/compose.c の MIME-Version・添付の loop・base64 の group。計 15 か所。
- **comment の位置の誤り**: protocol.c の surface_commit で、offset を足す段落が「cursor の surface」の comment と `if` の間に割り込んでいた。段落を前に出した（順序は無関係の代入なので動作は同じ）。
- **success でない場所の "Succeeded:"**（§11）: data.c の offer_set_actions で `if` の上に付いていた "Succeeded:" を普通の説明に直した。
- **macro の置き場所**（§2）: photos/view.c の VIEW_DRAG_DISTANCE を ID の並びの中から外へ、browser/shell/shell.c の SHELL_DRAG_* を struct の後から file の先頭の macro へ、
  libbrowser/page/link.c の link_image_box の前方宣言を他の宣言の block へ。
- **連なった式を 1 行ずつに**（§1.1）: libbrowser/view/view.c の premultiply（`red`・`green`・`blue` の名前の付いた変数）。
- **host 試験 3 つを全面に書き直した**（試験に規約の例外は無い）: host-png-write.c（条件の中の memcmp・read32、三項演算子、`check` の引数の複合式、関数の分割 `check_chunks`・`check_samples`・`has_type`・`expected_red`）、
  host-dnd-state.c（前方宣言、三項演算子、条件の中の strcmp・kwl_dnd_mark_name、名前の表）、host-mail-attach.c（前方宣言、条件の中の strstr・memcmp・strcmp、`contains` の補助）。検査の内容は同じ（dnd-state は 165 検査のまま）。
- 前の Haiku が足した注釈（clipboard.c の "Closes the read end of the pipe."・"Resets the drag type count."、libbrowser/view/view.c の 4 つ）は、
  code が実際にする事（pipe の読み側を閉じる、drag の type 数を 0 に戻す、prefetch の失敗で page を捨てる、script の初期化、location の vector の解放）と合っていた。直していない。

## 直さなかった物

- libbrowser/page/link.c の `goto cleanup` 10 件: 既存の行（WS189 の前から）で、cleanup label への前方の跳びだけ（§6 の「共有の cleanup label への 1 回の前方の跳び」に合う）。style-check の goto は規則の判定の制限。
- 他の WS の既存の違反（所有者の WS の見直しで扱う）:
  wayland/shell.c の 973・2183・2190（paragraph-comment）と 2216・6364（blank-after-brace）、wayland/compose.c の 335・498・517・551（blank-after-brace）、
  files/window.c の 422・files/main.c の 808・810・1624・1629・1743（blank-after-brace、1624・1743 は ws188-p004 の mounts の行）。
- 欠陥に見える所は無かった（notes の `notes_picture_png` で `owned` を返し忘れる経路は、PNG・IDAT・JPEG の kind では `owned` が NULL なので漏れない）。

## 確認

- `python3 plan/tools/style-check.py`（変えた file）: WS189 の行の指摘 0。残りは上の「直さなかった物」だけ（goto 10、他の WS の blank-after-brace・paragraph-comment 15）。
  plan/ws189/tests の host-*.c 3 つは指摘 0。
- `git diff --check`: 0。clang-format は worktree の環境に無く、未実施。
- build: `make -j16 ZEDBSD_CONFIG=plan/ws035/tests/config-amd64-zdesktop.mk BUILD=build/p3-ws189 build/p3-ws189/dynamic/libkeiland.so build/p3-ws189/dynamic/libbrowser.so build/p3-ws189/bin/{wayland,files,photos,notes,pdfviewer,mailer,textedit,browser,data-probe}`: rc 0、warning 0。
  `make -j16 keiland-linux KEILAND_LINUX_BUILD=build/p3-ws189-linux`: rc 0、warning 0。
- host 試験（全部 PASS）: plan/ws189/tests の run-host-png-write.sh・run-host-dnd-state.sh（165 検査）・run-host-mail-attach.sh、
  plan/ws169/tests/run-host-mailer.sh、plan/ws175/tests/run-host-notes-edit.sh、plan/ws177/tests/host-pdf-find-l.sh（plain と ASan）、plan/ws128/tests/run-host-pdfviewer-find.sh（plain と ASan）。
- 未実施: QEMU・実機の回帰（WS の最後に T1 へ、Q1 経由）、plan/ws079/tests/run-pdfviewer-host.sh（build/ws079-p006-host/notes.pdf が要る）。

## 結果

WS189 の C の WS189 の行を全文と照らして読み、上の規則の 40 か所ほどを直した。Q1 の判定待ち（Status を自分で cleared にしていない）。
