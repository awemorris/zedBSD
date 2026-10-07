<!-- awesome-plan project=zedbsd record=ws181-p010 -->
# ws181-p010: Alt+Shift+左右で仮想 desktop を移る

Status: test-wait（T1 の依頼は Q1 経由。2026-10-08 P1 q875: 実装・build warning 0 まで。QEMU は `plan/ws181/tests/p010-guest.sh`）
Disposition: normal
Parent: [WS181](../ws.md)
Queue: q875（P1、2026-10-08 Q1 の承認「5330 なしで進められる小さい物を順に」の 1 番）

## 由来（ユーザー）

- 2026-10-08「Super+Shift+左右、使いやすくてすばらしい設計ですね！…あとでいいので、Ctrl+Shift+左右で、Virtual Desktopを移動できるようにお願いします。」
- 2026-10-08（Ctrl+Shift+左右は app の単語の選択と重なると伝えた後）「では、Alt+Shift+左右で仮想デスクトップ移動、ではどうですか？私はEmacsユーザなので、標準テキストエディタがどう使われているのか知りませんでした。」→ key は Alt+Shift+左右（master の decisions-log）。

## 範囲

1. compositor（`userland/desktop/wayland/shell.c`）: Alt と Shift だけを押して Left・Right → 前・後の仮想 desktop へ（`desktop_turn`、log `KWL GLASS desktop=N via=alt-shift`）。端では動かず key は取る（`KWL GLASS desktop stays=N via=alt-shift`）。押下を取った key の release も app へ渡さない。
2. 既存の Ctrl+Alt+左右（desktop を移る）と Ctrl+Alt+Shift+左右（窓を連れて移る）はそのまま。Super+Shift+左右（ws113-p007、窓を隣の display へ）とも重ならない（修飾の完全一致で判定）。
3. Welcome の Keys の段（`userland/desktop/settings/welcome.c`）の表示を「Alt+Shift+Left / Right — Move between desktops」に（Ctrl+Alt も効くが、案内は新しい key）。

範囲の外: Alt+Shift+Up・Down（app へ渡す）。

## 影響（ユーザーが承知の上の選択）

- Emacs の M-S-left・M-S-right（shift-select の単語の移動）と、terminal の Alt+Shift+矢印の escape は compositor が取るので app に届かなくなる（ws.md の p010 の行に記録済み、ユーザーは Emacs 利用者として Alt+Shift を選んだ）。

## 実装（2026-10-08 P1）

- `desktop_alt_shift_key()`（display_move_key の後に新設）。`MODIFIERS_ALT_SHIFT (8U | 1U)` と `MODIFIERS_ANY` で修飾の完全一致を見て、押下の key を `desktop_key_eaten` に覚え release も食べる。`kwl_glass_key` で `display_move_key` の後、Ctrl+Alt の判定の前に呼ぶ。
- welcome.c の key の一覧の 1 行。

## 確認

- build（warning 0）: `make -j16 BUILD=build/ws181-p010 ZEDBSD_CONFIG=plan/ws035/tests/config-amd64-zdesktop.mk build/ws181-p010/bin/wayland`・`.../bin/settings`（rc 0、log の warning 0 行）。
- host 試験: 無し（key の分岐だけで host の harness が無い。読みで確認: Ctrl+Alt+Shift は MODIFIERS_ANY の値が 0xd で 0x9 と違い、Alt+Shift の分岐に入らない）。
- QEMU（T1）: `plan/ws181/tests/p010-guest.sh BUILD OUTDIR`（pen の guest、plan/ws079/tests/config-amd64-pen.mk、`plan/ws079/tests/pen-guest.sh start IMAGE`）。BUILD は依頼の tree で build した物（bin/wayland・bin/wltest）。合格は最後の行 `ws181-p010: PASS`、PNG は p010-right・p010-left。
- 実機: 未実施（5330・5320 の UAT で Emacs の外での使い心地）。

## 積み残し

- 無し（端で wrap しないのは Ctrl+Alt と同じ）。
