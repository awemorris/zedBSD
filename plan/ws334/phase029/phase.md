<!-- awesome-plan project=zedbsd record=ws334-p029 -->

# ws334-p029 統合: libkeiland の Win32 その 2（題の帯の controls と location の欄、IMM32、clipboard、DPI）

- 親: [WS334](../ws.md)、設計 [design.md](../design.md) の §4.4 の N1〜N3・N6、§4.5 の titlebar の controls・text input・clipboard の行。
- 状態: planned（2026-11-01 p003 で作成）。
- 依存: p018（cf-html）・p028。
- 担当の想定: Opus（phase-runner）を 1 回（約 12 LW、UI）。使う module の約束: `win32/frame.h`・`wide.h`・`input.h`・`cf-html.h`。

## 決めてある形

1. **題の帯の controls**（新 `libkeiland/win32/titlebar.c`）: browser が置く 4 つ（`browser/shell/titlebar.c:43-46`: BACK・FORWARD（PRIMARY）、GENERIC の Reload（NORMAL）、BREADCRUMB の Location（NORMAL、`KL_BREADCRUMB_BOXED`））と、mail の code の時の GENERIC（PRIMARY、`titlebar.c:163`）を描いて操作できる所まで。並べ方は Keiland の compositor の題の帯（`userland/desktop/wayland/titlebar.c`・`titlebar-shell.c`、zedBSD の Zlib の code。規則を読み、Windows の帯の上に同じ規則で並べる。compositor の code を libkeiland に写す時は compositor の内部に依る所を外す）: 左から PRIMARY の button（32 × 32 at 96 dpi の正方）、NORMAL の button、BREADCRUMB の箱が残りの幅を取り、右に 3 つの caption の button（`keiui_win32_frame_buttons`）。幅が足りない時は SECONDARY → NORMAL の button の順に隠す（compositor の規則と違えば compositor に合わせ、違いを結果に書く）。icon は role ごとに libkeiland の今の icon の描き方（`kl_icon_*` か theme の描画の口。compositor が使う物を探す）、無ければ label の文字。controls の矩形を `struct keiui_win32_frame` の `controls` に渡す（hit test で CLIENT）。
   - `kl_window_set_controls`・`_set_action_state`（DISABLED は灰色で押せない、HIDDEN は描かない）・`_set_control_text`（箱の文字と placeholder）・`_set_control_parts`（BOXED では使わない。受けて保つだけ）・`_set_control_value`（BOXED）・`_focus_control_mode(KL_FOCUS_EDIT)`（欄の編集を始め、全部を選ぶ）。押した button は `KL_WINDOW_ACTION`（code = action、id = control の ID）。
   - **location の欄の編集**: libkeiland の今の text の編集の部品（`libkeiland/edit.c` の `kl_edit_*`、keiland_edit.h）で 1 行の欄を作る。Enter で `KL_WINDOW_CONTROL_DONE`（code `KL_TEXT_SUBMITTED`、text は欄の UTF-8）、Esc で `KL_TEXT_CANCELLED`（名前は keiland_window.h の定数を確かめる）、打つたびに `KL_WINDOW_CONTROL_TEXT`。focus が欄にある間の鍵は欄へ（page へ行かない）。
2. **IMM32**（N2 の第 1 段）: `kl_window_text_input(enabled)` で `ImmAssociateContextEx`（有効: 既定の context、無効: NULL）。`WM_IME_STARTCOMPOSITION`・`WM_IME_COMPOSITION`（`GCS_COMPSTR` → `KL_WINDOW_TEXT_PREEDIT`、text は `ImmGetCompositionStringW` の UTF-16 を `wide.h` で UTF-8、begin・end は `GCS_CURSORPOS` を UTF-8 の byte に換算、`GCS_RESULTSTR` → `KL_WINDOW_TEXT_COMMIT`）・`WM_IME_ENDCOMPOSITION`（空の PREEDIT）。既定の window procedure に composition の窓を描かせない（`WM_IME_COMPOSITION` を処理した時は `DefWindowProcW` に渡さない、`WM_IME_SETCONTEXT` で `ISC_SHOWUICOMPOSITIONWINDOW` を外す）。候補の窓の位置は `kl_window_text_cursor(x, y, w, h)` の矩形を `ImmSetCompositionWindow(CFS_POINT)`・`ImmSetCandidateWindow(CFS_EXCLUDE)` に。location の欄の編集の時は欄の caret の矩形。`kl_window_text_context` は受けて保つだけ（周りの文字は TSF の p033）。
3. **clipboard**（N6 の第 1 段）: `kl_window_copy(text, length)`: `OpenClipboard`（窓）・`EmptyClipboard`・`CF_UNICODETEXT`（`wide.h` で UTF-16、`GlobalAlloc(GMEM_MOVEABLE)`）。HTML の copy の口が libkeiland の公開の API に無ければ text だけ（CF_HTML は貼り付けの p033 と合わせて使う。`cf-html.c` はこの Phase で link し、`kl_window_copy` の text が HTML かは判断しない）。貼り付け（location の欄の Ctrl+V）: `CF_UNICODETEXT` を読んで欄へ。page への貼り付けの口が libkeiland に無ければ p033。
4. **DPI**（N3）: 窓を作る時に `GetDpiForWindow`、`WM_DPICHANGED` で示された矩形へ移し、題の帯・button・欄の大きさを `keiui_win32_scale` で描き直し、`KL_WINDOW_RESIZE` を出す。page の中身は 1 px = 1 物理 px のまま（決定 21 の U4）。libkeiland の canvas の文字の大きさの倍率の口があれば使い、無ければ font の大きさに倍率を掛ける（どちらにしたかを結果に）。

## 試験（受け入れ）

- `cross-build.sh` PASS（両方の arch で warning 0）。Linux の Keiland の build と試験の件数が同じ。`run-unit.sh all`・`check-skeleton.sh`（BASE）PASS。
- Wine＋xvfb-run（`wine-run.sh` に項を足す）: 題の帯の PNG（4 つの control と 3 つの button）、location の欄に xdotool で英字の URL を打って Enter で移る（log と PNG）、Back の button で戻る、DPI 144 の Wine（`HKCU\Control Panel\Desktop\LogPixels` = 144 の prefix）で帯が 1.5 倍の PNG、clipboard: 欄の copy の後 Wine の clipboard（`xclip -o` が使えれば）に URL。IME の確かめは Wine ではできない（実機の Windows で、p032 の後）と結果に書く。PNG は `build/review/ws334-p029/` に。
- `git diff --check`。

## 変えてはいけない物と終わり

- module の header と .c、試験の道具。Wayland の libkeiland の file と compositor（読むだけ）。HAL・toolchain・共有の build/。rm をしない。push しない。
- file の群ごとの commit、SHA・PNG の path・未実施（IME の実機）を Q1 に、結果の節と ws.md の行。
