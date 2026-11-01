<!-- awesome-plan project=zedbsd record=ws334-p029 -->

# ws334-p029 統合: libkeiland の Win32 その 2（題の帯の controls と location の欄、IMM32、clipboard、DPI）

- 親: [WS334](../ws.md)、設計 [design.md](../design.md) の §4.4 の N1〜N3・N6、§4.5 の titlebar の controls・text input・clipboard の行。
- 状態: in-progress（実装済み・試験 PASS、cleared は Q1。2026-11-01 q1867-i01、P5）。前: planned（2026-11-01 p003 で作成）。
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

- `cross-build.sh` PASS（両方の arch で warning 0）。 `plan/tools/keiland-os-boundary/check.sh` に WS334 の file の新しい失敗が無い。Linux の Keiland の build と試験の件数が同じ。`run-unit.sh all`・`check-skeleton.sh`（BASE）PASS。
- Wine＋xvfb-run（`wine-run.sh` に項を足す）: 題の帯の PNG（4 つの control と 3 つの button）、location の欄に xdotool で英字の URL を打って Enter で移る（log と PNG）、Back の button で戻る、DPI 144 の Wine（`HKCU\Control Panel\Desktop\LogPixels` = 144 の prefix）で帯が 1.5 倍の PNG、clipboard: 欄の copy の後 Wine の clipboard（`xclip -o` が使えれば）に URL。IME の確かめは Wine ではできない（実機の Windows で、p032 の後）と結果に書く。PNG は `build/review/ws334-p029/` に。
- `git diff --check`。

## 変えてはいけない物と終わり

- module の header と .c、試験の道具。Wayland の libkeiland の file と compositor（読むだけ）。HAL・toolchain・共有の build/。rm をしない。push しない。
- file の群ごとの commit、SHA・PNG の path・未実施（IME の実機）を Q1 に、結果の節と ws.md の行。

## 結果（2026-11-01 23:53、P5、q1867-i01、基点 9dae26145）

commit（`git commit -m WIP -- <path>`）:

| SHA | 中身 |
| --- | --- |
| 5448562e5 | `libkeiland/win32/`: 新しい `titlebar.c`（controls と location の欄）・`ime.c`（IMM32）・`clipboard.c`（CF_UNICODETEXT）、`window.c`（題の帯の mouse・欄への鍵・IME の message・DPI の変更で RESIZE・題の帯の font）、`present.c`（題の帯に controls を描く）、`window-win32.h`、生成し直した `unsupported.c`（stub 319、公開 294）。`ui/text.c` に `O_BINARY`（無い system では 0） |
| d5c3e6863 | `apps/browser-win32/Makefile`（win32 の 3 つの file、`ui/text.c` を C runtime の POSIX の file の口だけを使う file として Windows の libkeiland に）、`plan/ws334/tests/wine-run.sh`（項 5〜7）・`allowed-imports.txt`（clipboard と imm32） |

確かめ:

| 確かめ | 結果 |
| --- | --- |
| `timeout 3500 sh plan/ws334/tests/wine-run.sh` | `wine-run: PASS`（p028 の 1〜4 も PASS のまま）。5: 題の帯（Back・Forward が灰色、location の箱に今の path、Reload の pill、3 つの caption の button）、xdotool で箱を押し `about.html` を打って Enter で about.html へ、Back の button で start.html へ戻る（`ZBROWSER NAVIGATE` の順で確かめた）。6: Ctrl+L・Ctrl+C の後 `xclip -o -selection clipboard` が箱の path。7: LogPixels 144 の prefix で題の帯が 48 px（y 40 の画素が題の帯の地の色、96 dpi では頁） |
| Wine の PNG（worktree の中） | `build/review/ws334-p029/titlebar.png`・`typing.png`（欄に about.html と caret）・`typed.png`・`back.png`・`titlebar-144dpi.png`・`dpi144.png` |
| `timeout 3600 flock /tmp/zedbsd-wine.lock sh plan/ws334/tests/cross-build.sh` | `cross-build: PASS`（両 arch の OmniGrammaton.exe が warning 0、libkeiland.a と libbrowser-ui.a の import 188（win64）・184（winarm64）が全部表の中、libkeiland.a が公開の関数 573 を全部定義、.exe の import は system の DLL 24 個と遅延の vulkan-1.dll） |
| `keiland-common.sh`・`keiland-stubs.py --check` | PASS・PASS（共通 33 は変わらず。`ui/text.c` は規則では除外のままで、Makefile の別の一覧で入れる） |
| Linux の Keiland（`make -f userland/desktop/keiland-linux.mk all`）と libkeiland の host の試験 | exit 0・warning 0。`keiland-layers/host-test.sh` pass=8 fail=0、`run-host-present-damage.sh` 25 checks 0 failures（p028 の時と同じ）。`ui/text.c` の直しの前後を host の cc で compile して `objdump -d` が同じ（Linux では `O_BINARY` が 0） |
| zedBSD amd64（`libkeiland.so`・`bin/browser`） | exit 0、warning 0 |
| `keiland-os-boundary/check.sh` | FAIL 62（p028 と同じ数、WS334 の file と `ui/text.c` の失敗は 0） |
| `run-unit.sh all` | PASS |
| `check-skeleton.sh`（BASE d393d3267） | FAIL（platform.h だけ、基点でも同じで p035） |
| `git diff --check` | 出力なし |

自分で決めた点・phase.md と code の食い違い:

- **並べ方は compositor（`wayland/titlebar-shell.c` の `shell_layout`・`shell_side`・`shell_item_width`）の規則に合わせた**。phase.md の「左から PRIMARY、NORMAL の button、箱」と違い、compositor は役割で側を決める: BACK・FORWARD・UP・HOME は左、BREADCRUMB は間、それ以外（Reload の GENERIC、mail の code の GENERIC）は右（caption の button の前）。大きさも compositor の値: 正方 30 px（phase.md の 32 でなく）、間 4、端 10、icon 16、GENERIC は label の幅 + 両側 12 の pill、箱の最小 120。足りない時は右の最後の SECONDARY、次に右の最後の NORMAL を隠し、箱（BOXED は欄）と PRIMARY は残す。compositor の「…」の overflow の menu は無い（隠した control は出ない）。
- **location の欄**: phase.md の `kl_edit_*`（keiland_edit.h）は on-screen keyboard の編集の protocol で、文字の欄の部品ではなかった。`kl_field`（kl_ui の即時の部品）は kl_ui の frame の仕組みごと要るので、`titlebar.c` に 1 行の欄を小さく書いた: UTF-8 の文字列・caret・選択、文字の挿入、Backspace・Delete、左右・Home・End（Shift で選択）、Ctrl+A・C・X・V、Enter（`KL_TEXT_SUBMITTED`）・Esc（`KL_TEXT_CANCELLED`）、変わるたびに `KL_WINDOW_CONTROL_TEXT`、content を押すと CANCELLED。箱を押すと全部を選んで編集。欄が鍵を持つ間は鍵を page に送らない。
- **題の帯の文字**: `ui/text.c` に `O_BINARY`（定義の無い system では 0）を足し、Windows の libkeiland に「C runtime の POSIX の file の口だけを使う file」として入れた（`LIBKEILAND_WIN32_CRT_SOURCES`。共通の規則は変えていない）。font は `kl_data_directory` の `fonts\keiland.ttf` と `keiland-fallback.ttf`。controls が無い時は題を左に描く。emoji の font の path（`KEILAND_DATADIR`）は Windows に無く、黙って使わない。
- **文字の大きさの倍率**: libkeiland の canvas に倍率の口は無いので、theme の `text_body`（13）に dpi / 96 を掛けた。
- **DPI**: `WM_DPICHANGED` で示された矩形に移し、`resize_waiting` を立てて RESIZE を出す。題の帯・button・箱・icon・線の太さを `keiui_win32_scale` で描き直す。page の中身は 1 px のまま（U4）。
- **IMM32**: `kl_window_text_input` か欄の編集の間だけ `ImmAssociateContextEx(IACE_DEFAULT)`、他は context 無し。`WM_IME_SETCONTEXT` で `ISC_SHOWUICOMPOSITIONWINDOW` を外す、`WM_IME_STARTCOMPOSITION`・`_COMPOSITION`・`_ENDCOMPOSITION` は `DefWindowProcW` に渡さない。`GCS_RESULTSTR` → COMMIT（欄の編集中は欄へ挿入）、`GCS_COMPSTR` → PREEDIT（begin・end は `GCS_CURSORPOS` の UTF-16 の位置を UTF-8 の byte に）、終わりは空の PREEDIT（begin・end -1）。欄の編集中の組み立ての文字は caret の後に下線付きで描く。`VK_PROCESSKEY` の `WM_KEYDOWN` は鍵の event にしない。候補の窓は `kl_window_text_cursor` の矩形（題の帯の高さを足した frame の座標）か欄の箱に `CFS_POINT`・`CFS_EXCLUDE`。`kl_window_text_context` は受けて捨てる。
- **clipboard**: `kl_window_copy`・`kl_window_paste`・`kl_window_can_paste` を CF_UNICODETEXT で。CF_HTML の口は公開の API に無いので使わない（`cf-html.c` は前から libkeiland.a に入っている）。
- 押せる control（DISABLED でない）の上で hover・押下の地を描く。DISABLED は `text_faint` で押せない。HIDDEN は描かず数えない。`kl_window_set_control_parts` は受けるだけ（BOXED は箱の文字）。`kl_window_focus_control` は edit の mode と同じ。

気づいた事（この Phase の外）:

- browser の UI（`shell/titlebar.c` の `titlebar_scheme_length`）は Windows の drive の path `Z:\...` を scheme "Z:" の URL と見て、`file://` を付けずに箱に出す（PNG の通り）。Enter で開くことはできる（`shell_follow_typed` が絶対の path として読む）。file: の URL で見せるかは browser の UI の判断（Q1 へ）。
- 144 dpi の幅の狭い箱で `kl_text_draw_fit` が末尾に出す省略の記号が四角（font に無い字）に見える（`titlebar-144dpi.png`）。libkeiland の text の側の事で、Wayland でも同じ font なら同じ。

未実施:

- IME（MS-IME・Google 日本語入力）は Wine で確かめられない。実機の Windows で（p032 の後）。組み立ての文字・候補の窓の位置・確定は未確認。
- 本物の Windows での DPI の変更（monitor の間の移動）、snap、題の帯の controls の hover の見た目の確認、page の text 欄への貼り付け（libkeiland に page への貼り付けの口が無い。p033）。
- aarch64 の Windows の実行。

消してよい物（Q1 へ）: `build/tmp/p029`・`build/tmp/p029-*`（Windows の試しの build・log・text.c の比較）、`build/tmp/ws334-wine-gui144`（144 dpi の Wine の prefix、次の試験でも使える）。p028 の一覧（`build/tmp/ws334-wine-build`・`ws334-wine-run*` ほか）は今回も上書きして使った。
