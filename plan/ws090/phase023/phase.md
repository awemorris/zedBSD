# ws090-p023: Files・Settings の残りの自前の UI 部品を libkeiland の部品へ

Status: cleared（2026-10-08 Q1 の判定（sweep-beta2-rc §2）。copy の取り消しの × と Wi-Fi の Disconnect は 5330 の UAT へ）（旧: test-wait → Q1 の判定待ち（2026-10-08 q902 P1 の照合: T1-266 の regress PASS。T1-373 (1) で Favorites の ×・tab の ×・Help の ×・Quick Look の ×・Get Info の × を確認。copy の取り消しの ×（QEMU では copy が速すぎる）と Settings の Wi-Fi の Disconnect（QEMU に Wi-Fi が無い）は未、5330 で）（旧: planned（2026-10-06 Q1 が作成）））
WS: [WS090](../ws.md)
Related: ws090-p007・p009・p010（q817、canvas と文字の欄）

## 出典

2026-10-06 ユーザー（クリック）「置き換える（別の Phase）」: Files と Settings の残りの自前の UI 部品（Settings の button・switch・slider、Files の list・sidebar・dialog・chip）も libkeiland の部品に置き換える。見た目の差は libkeiland の部品に合わせ、前後の撮影をユーザーに見せる。

## 範囲

- q817（p009 → p010 → p007）の後。Settings の button・switch・slider・card・row、Files の list・grid・sidebar・dialog・chip を libkeiland の部品（ws090-p005 の部品、kl_ui）へ。足りない部品・機能は libkeiland に足す。
- 試験: host の描画の前後の比較、既存の Settings・Files の試験の回帰、T1 で前後の撮影をユーザーへ（`build/review/`）。

## q818（P1、2026-10-06）

Status: 実装と host の前後の撮影まで済み、T1 の撮影待ち。

### 作り（app の hit と動作は自前のまま、部品の描画と光り方は libkeiland）

Files と Settings は自前の hit の model（`fm_ui_hit`・`se_ui_hit`）で押下の動作を決める。部品はそのままにして、描画と hover・押下の光り方を libkeiland の部品（1 つの `kl_ui` に記録）にした。部品の click の返り値は使わず、押下は今までどおり app の hit から（試験の log・座標は変わらない）。

- libkeiland（KL_VERSION 48）: `kl_slider_flags`（`KL_BUTTON_DISABLED` で薄く・入力なし）、`kl_sidebar_place`（`KL_PLACE_FAINT`（無い所）・`KL_PLACE_QUIET`（mount していない device））。
- Settings（`settings/widgets.c`）: 頁の見出し `kl_header`、card `kl_card`（Settings の card は自分の地の上なので glass の見た目で）、行 `kl_row`、button `kl_button`、switch `kl_switch`、slider `kl_slider_flags`。kl_ui は欄（p007）と共有。
- Files（`files/ui-widgets.c` 新規: `fm_widgets_begin`・`_end`・`_input`・`_release`、`fm_style`、`fm_button`・`fm_button_width`）: sidebar を `kl_panel`・`kl_sidebar_section`・`kl_sidebar_place`、問いと作業の一覧の card を `kl_panel`（白）、問いのボタン・ごみ箱のボタン・情報の panel のボタンを `kl_button`（削除は `KL_BUTTON_DANGER`）、message の chip を `kl_chip`（Files の暗い pill から libkeiland の白い chip へ）。名前の変更の欄（p010）も同じ kl_ui に。
- host の試験: `plan/tools/files/host-build.sh` に libkeiland の `widgets.c`・`cards.c`・`list.c`、`plan/ws089/tests/host-build.sh` に `widgets.c`・`cards.c`。

### 見た目の差（前後の host の絵、`build/review/ws090-p023/`）

- Settings: slider（track が細く、knob に影）、switch（libkeiland の knob）。card・行・見出し・button は byte で同じ。
- Files: sidebar の節の見出しの色（faint → secondary）、message の chip（暗い pill → 白い chip）、問いの「Cancel」button（灰の pill → libkeiland の白い button）。

### 範囲の外

- Files の list・grid、icon だけの小さなボタン（sidebar の削除・取り出し、Settings の icon の button、Files の tabs・Quick Look・Help・情報の閉じる）は自前のまま。libkeiland に該当の部品が無い。積み残しは [WS177 の P1 の一覧](../../ws177/backlog-p1.md)。

### 確認

- build: zedBSD amd64 の libkeiland・files・settings・textedit・mailer・calendar・notes・phone（exit 0、warning 0）、`make keiland-linux` の gcc と clang（exit 0、warning・error 0）、`exports.py --check` OK、`keiland-os-boundary/check.sh` PASS。
- host: files-model PASS、run-filestouch ok (20)、host-desktop PASS、host-widgets 94/94、host-account-admin 34/34、host-settings 38/38、host-wired・host-dark・host-instance・host-pointer-accel PASS、host-users 10 checks、settings-render 15 頁の比較（上の差だけ）、files-render の home・grid・list・glass・chip・dialog の前後（前は HEAD の tree を `git archive` で `build/ws090-p023/old` に出して build）。
- 未実施（T1）: Files・Settings の前後の撮影（glass、1280x800）をユーザーへ、files-regress・settings-regress。

### q822 の追加（P1、2026-10-06）: host の描画の companions

- ユーザーが Settings の Sound の host の絵で音量の「—」（U+2014）が豆腐なのを見つけた。libkeiland の text が companions（Mahora Bold・monospace の fallback）を install の path から開くので、host では無い。
- libkeiland（KL_VERSION 48）に `kl_text_companions(bold, mono)`（以後に開く font の companions の file を名指す。NULL は install の物、既定の動作は変えない）。host の描画の道具（`plan/ws089/tests/host-render.c`、`plan/tools/files/host-render.c`、ws094 host-desktop、ws155・ws169・ws170 の host）が tree の `userland/desktop/fonts/Mahora-Bold.ttf`・`JetBrainsMono-Regular.ttf` を使う。
- Sound の頁の描き直し: `build/review/q822/settings-sound.png`（「—」が出る）。host の試験は mailer・calendar・phone・host-desktop・host-widgets 94/94・host-chooser 85/85・host-mahora PASS。

## q858（P1、2026-10-08）: icon だけのボタン

Status: 実装と host の確認まで済み、T1 未依頼（WS113 p015 の後に依頼文を Q1 へ）。

- libkeiland（KL_VERSION 60）: `kl_icon_button(ui, style, id, index, rect, icon, pixels, flags)`（control の角丸の四角と縁、`KL_BUTTON_ROUND` で円、`KL_BUTTON_QUIET` で pointer の下・押下の間だけ地を出す、`KL_BUTTON_DISABLED`、focus の輪、Enter・Space）。icon に `KL_ICON_DISCONNECT`（Settings の丸に×を line の pen へ）・`KL_ICON_EJECT`（Files の取り出し）。`kl_button`・`kl_icon_button`・`kl_sidebar_place` は NULL の kl_ui でも描く（光らず押されない）。
- Files（`fm_icon_button`、id `FM_WIDGET_ICON`・`FM_WIDGET_TAB_CLOSE`）: sidebar の削除・取り出し、tab の閉じる、作業の取り消し（quiet・round）、Help・情報・Quick Look の閉じる（round、Files の灰の円 → libkeiland の白い地と縁、×の色は text）。`fm_button` は kl_ui が無くても描く（backlog の異常系の行を解消: 前は描かず、`kl_sidebar_place` は NULL で落ちた）。押下は今までどおり Files の hit。
- Settings: `se_icon_button_draw` が `enum kl_icon` を取り `kl_icon_button` で描く。`SE_GLYPH_DISCONNECT` を除いた。
- 見た目の差: sidebar の削除の × は前は行の hover で薄い円が常に出た → 今はボタンの上でだけ円（quiet）。Help 等の閉じるは白い地と縁、× が濃い。
- 確認: zedBSD amd64 の libkeiland・files・settings・textedit・mailer・calendar（exit 0、warning 0）、`make keiland-linux` の gcc・clang（warning・error 0）、`exports.py --check` OK、host-widgets 94/94、files-render で Shortcuts の閉じるの通常と hover（`build/ws090-p023b/help-crop.png`・`help-hover-crop.png`）、style-check 0。`keiland-os-boundary/check.sh` は B3 の 2 件（sessiond・printd の Makefile、この変更の外）で FAIL。`plan/ws089/tests/host-build.sh` は `kl_system_printers_*` の未定義で link できない（この変更の前から、Settings の printers の頁の stand-in が無い）。
- 未実施（T1）: Files の sidebar の削除・取り出し・tab の閉じる・作業の取り消し・Help・情報・Quick Look の閉じるの click と hover、Settings の Wi-Fi の Disconnect、files-regress・settings-regress。
