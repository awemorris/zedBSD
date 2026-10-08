<!-- awesome-plan project=zedbsd record=ws090-p007 -->

# ws090-p007: Settings を libkeiui へ

Status: cleared（2026-10-08 Q1 の判定（sweep-beta2-rc §2）。Wi-Fi の鍵の欄は 5330 の UAT へ）（旧: test-wait → Q1 の判定待ち（2026-10-08 q902 P1 の照合: q817 で実装。T1-263 settings-regress・volume PASS、T1-260・T1-338 で Full name の IME と User name、T1-373 (3) で有線の手動（数字と点・15 字）・Languages の password・Users の password と Show を確認。PIN の欄の crash（T1-373）は BUG-257 で直り T1-376 (b) PASS。Wi-Fi の鍵の欄は QEMU に Wi-Fi が無く未、5330 で）（旧: planned（2026-10-06 q817 で範囲を今の形に合わせた。下の「2026-10-06 の範囲」が正）））
Disposition: normal
Parent: [WS090](../ws.md)
Queue: q817（2026-10-06 ユーザーの決定「今 libkeiland の canvas へ移す」）
依存: p005（cleared）、**WS089 の完了**（ws.md・design.md §9 の 3 番、2026-09-29 main）

この file は 2026-10-01 に手引き（[../guide.md](../guide.md)）と一緒に作った。範囲は [design.md](../design.md) §9・§10 の p007 の行で、下の手順と
完了の条件はそこから起こした案である。実行の前に main が範囲と、WS104 との順（下の「始める前」）を確かめる。

## 2026-10-06 の範囲（q817、これが正）

窓は WS131 p019 で `kl_app` に移り済み（下の古い手順の「窓の土台を替える」は済み）。描画の層は p009 で Files と一緒に libkeiland へ移す。この Phase は欄:

- Settings の自前の欄（`se_field`: Wi-Fi の鍵・接続の鍵・有線の設定・言語の頁の password・利用者の頁・管理の頁・PIN）を libkeiland の `kl_field` に。
  秘密の欄（password・鍵・PIN）は `kl_field` の secret（点で見せ、IME 無し）。Settings は自前の hit と入力の model を持つので、`kl_ui` を 1 つ持ち、
  欄の hit と key と text input の `KL_WINDOW_TEXT_*` を渡し、frame の後に `kl_ui_window_text`。q816 の暫定の IME（`se_users_admin_text`・
  `main_text_input`・preedit の描画）を除く。秘密の文字の消去（`se_field_clear` の volatile）は `kl_field` でも保つ。
- button・switch・slider・card・row の部品への置き換えは範囲の外（見た目が変わる物は別の Phase）。

受け入れ: Settings に `se_field` が無い、各欄が `kl_field`（秘密は IME 無し、管理の氏名で日本語）、host（host-settings・host-account-admin・settings-render）、
build、QEMU（T1）で各欄の入力と日本語の氏名。

## 以下は 2026-10-01 の古い計画（参考）

## 範囲（design.md §10）

- 描画の層: Settings が `userland/desktop/files/canvas.c`・`text.c`・`icons.c` と `artwork/mark.c` を source のまま compile している（F-038、
  `userland/desktop/settings/Makefile` の `KEILAND_SETTINGS_SOURCES` の最後の 4 つ）のを、libkeiui の `kui_canvas`・`kui_text`・`kui_icon` に替える。
  Settings の線の絵（`settings/glyphs.c`）は p002 で libkeiui の `icons-line.c`（`KUI_ICON_TILES`〜`KUI_ICON_DISCLOSURE`、Settings の順のまま、
  GRID → `KUI_ICON_TILES`、CHEVRON → `KUI_ICON_DISCLOSURE`）に入っている（[phase002](../phase002/phase.md)）。
- 窓の土台: `settings/window.c`（1347 行）・`present.c`（1218 行）を `kui_window`（`KUI_PRESENT_VULKAN`、glass のため see-through）に替える。
  menu（`menu.c`）・titlebar（`titlebar.c`）・glass（`glass.c`）は libkeiland のまま app に残し、`kui_window_display`・`kui_window_surface`・
  `kui_window_toplevel`・`kui_window_seat` の accessor で作る（PDF Viewer・Image Viewer の p008 と同じ）。
- 部品（任意、時間があれば）: `settings/widgets.c` の button・switch・slider・field・card・row を libkeiui の部品に。見た目が変わる物は変えない。
- 見た目と機能は変えない。

## 始める前（2026-10-01 追記）

- WS089 の完了の処理（試験を `plan/tools/settings/` へ移すか）の後に始める。WS089 の [guide.md](../../ws089/guide.md) の p012（提案）を見る。
- **WS104 と同じ file に触れる**: ws104-p002（`settings/look.c`・`settings.h`・`page-home.c`・`page-input.c`、`keiland_audio_available`）、ws104-p003
  （`plan/ws089/tests/host-build.sh:37` の path）、ws104-p007（install の path の macro、settings の `main.c` などの path の文字列）。
  **WS104 の p001〜p003・p007 が main に入った後に始める**か、main に順を決めてもらう（同時に走らせない）。
- KUI_VERSION を上げる要があれば（足りない accessor など）、適用の直前に main の値を確かめ、main の最新の次にする（2026-10-01 は 11、p015 が 12 を使う予定）。
- 2026-10-10 までに終わらなければ merge しない（design.md J5）。Settings はデモの S7 の app。

## 手順（2026-10-01 追記）

`<W>` は `ws090-p007`。command は repo の root から。一般の build・boot test は [plan/ws104/commands.md](../../ws104/commands.md) の §1・§4、
Settings の guest の回帰は同 §8。WS089 の試験が `plan/tools/settings/` に移っていたら、下の `plan/ws089/tests/` を読み替える。

1. 前の絵（基準）を host で取る（変更の前の tree で）:
   ```
   mkdir -p build/ws090-p007/before
   sh plan/ws089/tests/host-build.sh
   for page in home wifi ethernet network appearance wallpaper sound display storage keyboard mouse about bluetooth; do
   	build/ws089-host/settings-render --page=$page draw=build/ws090-p007/before/$page.ppm > /dev/null
   done
   ls build/ws090-p007/before | wc -l
   ```
   最後の行が `13`。`--page=` は `settings/pages.c` の表の小文字の語（home・wifi・…・about）。
2. 前の guest の画面: Settings の image（commands.md §8 の 1〜3 行目、`build-settings-image.sh build/ws090-p007-settings-before`）で
   `settings-regress.sh build/ws090-p007/regress-before` を流し、`settings-regress: PASS` と画面（各 test の directory の PNG）を残す。
3. 描画の層を替える: `userland/desktop/settings/*.c` の `fm_`→`kui_`・`FM_`→`KUI_`（`grep -c "fm_\|FM_" userland/desktop/settings/*.c` で 2026-10-01 は
   widgets.c 63・page-network.c 76 など計 12 file）、`#include` を `<keiui.h>` に。`glyphs.c` の線の絵は `kui_icon_draw(..., KUI_ICON_TILES + n, ...)` の対応に
   替え、`glyphs.c` を消す（icon の名前の対応は phase002 の記録）。Makefile の `KEILAND_SETTINGS_SOURCES` から files・artwork の 4 file と `glyphs.c` を除き、
   依存に `desktop/libkeiui` を足す。`platform/amd64/vmunix.mk:1361-1374` の `$(BUILD)/bin/settings` の規則に `$(DYNAMIC_DIR)/libkeiui.so`・`-l:libkeiui.so`・
   `--needed libkeiui.so` を足す（pdfviewer の規則 `:1401-1414` と同じ形。**vmunix.mk は main の許可が要る**。p008・p011 では Q1 が許可した）。
4. host の試験を合わせる: `plan/ws089/tests/host-build.sh` の compile の列（files の canvas・text・icons・mark の 4 file）を libkeiui の
   `canvas.c`・`text.c`・`icons.c`・`icons-line.c`・`theme.c`（と依存。`plan/ws090/tests/host-draw.sh` の列を見る）に替える（WS089 の file。**main の許可**、
   試験の中身は弱めない、窓の移行と別の commit）。
5. 1 と同じ command で `build/ws090-p007/after/` に描き、比べる:
   ```
   for f in build/ws090-p007/before/*.ppm; do cmp -s "$f" build/ws090-p007/after/$(basename "$f") && echo "same $(basename "$f")" || echo "DIFF $(basename "$f")"; done
   ```
   全て `same`。違う絵があれば `compare`（ImageMagick）で差の画素を見て、p002 の「Files と byte で一致」の前提が崩れた所を直す（意図した差は無いはず）。
6. 窓の土台を替える: `window.c`・`present.c`・`shaders.h`・`shaders/` を消し、`main.c` で `kui_window_open`（`KUI_PRESENT_VULKAN`、title「Settings」、
   app_id は今の値）・`kui_window_dispatch`・`kui_window_take`・`kui_window_repeat`・`kui_window_present_resize`・`kui_window_present` を使う。menu・titlebar・
   glass の action は `kui_window_post` で key と同じ queue に積む（PDF Viewer の `main.c` が見本、ws090-p008 の phase.md の「窓の移行」）。
   `window.h` の `struct se_window` は `struct kui_window *` を持つだけにする。
7. build（commands.md §1）: `make exit=0`、warning 0。`ls -la build/amd64/bin/settings`（大きさを前後で記録）。
8. 確かめ:
   ```
   sh plan/ws089/tests/host-build.sh
   sh plan/ws089/tests/host-preferences.sh
   sh plan/ws090/tests/host-draw.sh
   sh plan/ws090/tests/host-widgets.sh
   sh plan/ws100/tests/host-audio.sh
   ```
   PASS: `built build/ws089-host/settings-render`、`host-preferences: PASS`、`host-draw: 13/13 passed`、`host-widgets: 94/94 passed`、`host-audio: N/N passed`。
9. guest: commands.md §8 の Settings の 5 行（BUILD は `build/ws090-p007-settings`、runtime は `build/ws090-p007-settings-run`）で `settings-regress: PASS`（約 10 分）。
   画面を 2 の物と並べて目で確かめる（違いは時計と system bar だけ）。音の頁は commands.md §8 の `volume-p005.sh`（`volume-p005: PASS`、WS100 の試験、
   runtime は `build/ws100-run` に固定なので他の音の試験と同時に流さない）。
10. WS099 の C9（commands.md §5）と boot test（commands.md §4、`OUTPUT=build/ws090-p007/boot`）。
11. 規約: `python3 plan/tools/style-check.py userland/desktop/settings/*.c userland/desktop/settings/*.h` が 0、`git diff --check` 0。

## 完了の条件

1. Settings の source に `fm_`・`FM_` が無く、Makefile が files・artwork の source を compile しない（F-038 の解消）。`settings/window.c`・`present.c` が無い。
2. host の絵（手順 5）が前後で byte で同じ。
3. `settings-regress: PASS`、`volume-p005: PASS`、C9 の全行 PASS（BUG-125 の p076 の 1 回の FAIL は `C9_TESTS="p076"` で流し直す）、boot test PASS。
4. build warning 0、style-check 0。
5. 2026-10-10 までに 1〜4 を満たさなければ merge しない（J5）。

## 未知

- Settings の key の repeat と search の欄（titlebar の search の field の text は zdesktop の titlebar の物）が `kui_window` の queue の順で変わらないか（BUG-111 の型）。
  見る所: `settings/window.c` の repeat の扱いと `kui_window_repeat`（`userland/desktop/libkeiui/window.c`）。試験は `settings-p008.sh`（検索）。
- Settings の glass の 2 枚の card の panel の座標（`glass.c:79-107`）が `kui_window` の surface でも同じ原点か。試験は `settings-p004.sh` の画面。

## q817（P1、2026-10-06）

Status: 実装と host の確認まで済み、T1 の結果待ち。

- Settings の全ての欄（Wi-Fi の鍵、有線の 5 つ、言語の頁の password、利用者の password 3 つ、管理の 4 つ、PIN 3 つ）を libkeiland の `kl_field` に。`struct se_field` を除き、`struct kl_field` を直に持つ。
  - `widgets.c`: 全ての欄が 1 つの `kl_ui` の下（`se_fields_open`・`_close`・`_begin`・`_end`・`_input`・`se_fields_ui`）。`se_field_draw(app, canvas, field, rect, placeholder, kind, focused)` は頁の keyboard の持ち主（`focused`）を `kl_ui` の focus に写して `kl_field` を描く。`se_field_key` は欄の key（文字・Left・Right・Home・End・Backspace・Delete・Ctrl+A）を `kl_ui_key` に積み、1 画素の canvas の上で欄だけの frame を回してすぐに反映する（Enter が打鍵の直後に来ても新しい文字を読む）。Enter・Esc・Tab は今までどおり頁の物。`se_field_clear` は volatile で消して caret も 0 に。
  - 欄の種類（`SE_FIELD_TEXT`・`_SECRET`・`_PLAIN`）: 秘密（password・鍵・PIN）は点で IME 無し、plain（login 名・有線の address・Show で見せた password と鍵）は文字を見せて IME 無し、TEXT（管理の氏名）は IME あり。libkeiland の `kl_field` に `plain`（KL_VERSION 47、文字を見せるが IME を取らない）を足した。
  - 有線の欄: 数字と点以外と 16 文字目以降は key の時に落とす（`se_wired_type`、plain なので IME は来ない）。DHCP の時の欄は灰色の「From DHCP」のまま（欄ではない）。
  - 管理の操作の行が長すぎる時（libkeiland の欄は 511 byte まで持つ）は送らず「The names or the password are too long.」。
  - q816 の暫定の IME（`se_users_admin_text`・`_text_wanted`・`se_field_insert`・`se_field_delete_before`・preedit と caret の描画・`app->preedit`）を除いた。window の text input は frame の後に `kl_ui_window_text(se_fields_ui(), kui)`（main.c）。
- 見た目: 欄は libkeiland の物（白、角の丸み、縁の色、文字 14 px、caret・選択・preedit の下線、秘密の点は `•`。Mahora に `•` が無く、image では monospace の companion（JetBrains Mono）で描く。host の絵は companion が無く豆腐になる）。
- 試験: `plan/ws089/tests/host-build.sh` に libkeiland の `ui.c`・`field.c`・`input.c`・`theme.c`・`scroll.c`・`text-touch.c`・`scroll-bar.c`・`gesture.c`・`motion.c` と appearance の stand-in（files の `host-appearance.c`）。`plan/ws089/tests/host-wired.c` に `kl_key_character` の stand-in。

### 確認

- build: zedBSD amd64 の libkeiland・settings・files・textedit・mailer（exit 0、warning 0）、`make keiland-linux` の gcc と clang（exit 0、warning・error 0）、`exports.py --check` OK、`keiland-os-boundary/check.sh` PASS。
- host: host-account-admin 34/34、host-settings 38/38、host-wired PASS、host-dark PASS、ws160 の利用者の頁の確かめ（変更の依頼・長さだけ・errno 1/22・不一致で依頼なし、host の rm を避けて直に）、settings-render の 15 頁（p009 の後と storage の空きの差だけ）、host-widgets 94/94、host-chooser 85/85。
- 未実施（T1）: 各欄の入力（Wi-Fi の鍵と Show、有線の手動、言語の password、利用者の password と Show、管理の追加で日本語の氏名（IME）と login 名、PIN）、settings-regress、volume。
