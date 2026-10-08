<!-- awesome-plan project=zedbsd record=ws090-p010 -->

# ws090-p010: Files の欄を libkeiland の部品へ（名前の変更を kl_field に）

Status: test-wait → Q1 の判定待ち（2026-10-08 q902 P1 の照合: T1-261 の regress PASS、T1-373 (2) で grid・list の日本語の改名（a.txt から 日本.txt）、同名の toast、Esc で取り消し、欄の中の click で caret、外の click で確定、desktop の icon の改名（2 回目で 日本、1 回目は IME が切れていた T1 の操作）を確認）（旧: planned（2026-10-06 P1 が作成、q817））
Disposition: normal
Parent: [WS090](../ws.md)、設計 [design.md](../design.md) §10
Queue: q817
依存: p009（Files の描画が `kl_canvas`）。窓は WS131 p020 で `kl_app` に移り済み（design.md の「窓の土台を kui_window に」は済み）
所有 path: `userland/desktop/files/`、`plan/ws090/`、Files の host 試験

## 範囲

- Files の名前の変更の欄（grid・list・desktop、自前の `fm_field`）を libkeiland の `kl_field` に。Files は自前の hit と入力の model を持つので、欄の間だけ
  `kl_ui` を 1 つ持ち、窓の input を `kl_ui_window_input` で渡し、frame の後に `kl_ui_window_text` で text input と caret を伝える。q816 の暫定の IME
  （`fm_field_text_input`・`main_text_input`・preedit の描画）を除く。
- 場所と検索の欄は compositor の title bar の欄（既に IME あり）。自前の `fm_field` が残る所（場所の欄の model など）は使われ方を見て決める。
- list・sidebar・dialog・chip の部品への置き換えは範囲の外（見た目が変わる物は別の Phase、ユーザーに確かめてから）。

## 受け入れ

- 改名の欄が `kl_field`（IME・選択・caret・Enter で確定・Esc で取り消し、`/` は入らない）。暫定の IME の code が無い。
- build、host（files-render の改名、files-model）、QEMU（T1）で日本語の改名。

## q817（P1、2026-10-06）

Status: 実装と host の確認まで済み、T1 の結果待ち。

- `files/rename.c`（新規）: 名前の変更の欄を libkeiland の `kl_field` に。Files は自前の hit と入力の model を持つので、欄のための `kl_ui` を 1 つ持つ（`app->rename_ui`、改名の初めに作る）。
  - `fm_rename_start`（名前と拡張子の前の部分を選び、focus を欄に）、`fm_rename_select_all`、`fm_rename_input`（改名の間、key・pointer・IME の `FM_EVENT_TEXT*` を `kl_ui` へ）、`fm_rename_draw`（grid・list・desktop で名前の所に `kl_field` を描く。Files の自前の白地と縁は除いた）、`fm_rename_hit`（欄の上の press は欄の物: caret を置き、改名を終えない）、`fm_rename_take`（frame の後に Enter（`KL_FIELD_SUBMITTED`）で確定、Esc で取り消し、desktop は `fm_desktop_rename_end`）。
  - text input: frame の後に `kl_ui_window_text(app->rename_ui, kui)`（main.c）。
  - q816 の暫定の IME（`fm_field_text_input`・`fm_field_delete_before`・`fm_field_draw` の preedit・`app->preedit`・caret）を除いた。`fm_field` は場所と検索の欄の model に残る（title bar の欄の文字を持つ）。
  - `/` は確定の時に今までどおり拒む（`fm_action_rename_end`）。欄の文字の大きさは libkeiland の欄の 14 px（Files の名前は 12 px）、grid の欄の高さ 22→24。
- 試験: `plan/tools/files/host-build.sh` に libkeiland の `ui.c`・`field.c`・`input.c`・`theme.c`・`scroll.c`・`text-touch.c` と appearance の stand-in（`plan/tools/files/host-appearance.c`、新規）、`host-render.c` は改名の間は入力ごとに frame を描いて `fm_rename_take`。`plan/ws094/tests/host-desktop.c` の改名を `kl_field_set` に、font を tree の `userland/desktop/fonts/` に（`build/ws035-fonts` は無い、2 件の FAIL の原因）。

### 確認

- build: zedBSD amd64 の files（exit 0、warning 0）、`make keiland-linux` の gcc と clang（exit 0、warning・error 0）、`keiland-os-boundary/check.sh` PASS。
- host: files-render で F2 → `commit=日本` `compose=語`（下線）→ Enter で `report.txt` が `日本.txt` に（`build/ws090-p010/field.png`・`compose.png`）、files-model PASS、run-filestouch ok (20)、host-desktop PASS（host の rm を避けて直に）。
- 未実施（T1）: grid・list・desktop で改名（日本語の IME、Enter・Esc・欄の click で caret・外の click で確定）、files-regress、files-desktop-guest。
