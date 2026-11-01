<!-- awesome-plan project=zedbsd record=ws334-p033 -->

# ws334-p033 統合: 後半の Windows の口（TSF・drag と drop・touchpad と pen・accent・Mica・DPAPI・arm64）

- 親: [WS334](../ws.md)、設計 [design.md](../design.md) の §4.4 の「後」の列、§4.9、決定 21（U14: DPAPI は後半）。
- 状態: in-progress（2026-11-02 q1886-i01、P5: d・DPAPI・c・b の drag out・system の theme の prefers-color-scheme を実装し試験 PASS、a の TSF・b の drop の受け・c の DirectManipulation・d の modal の描き直しと題の帯の透過は未実装（理由と案は末尾）。cleared か分けるかは Q1。2026-11-01 p003 で作成）。
- 依存: p032。
- **設計の残り（この Phase の前に要る）**: b の貼り付けの公開の口（libkeiland の header の変更）と d の modal な loop の間の描き直しの口は、まだ決めていない。p032 の後に Q1 がこの 2 つの設計の小さな Phase（Opus）を立ててから、この Phase を起動する（2026-11-01 design-reviewer の指摘 #23 で明記）。a・c と d の残りは下の通り決めてある。
- 担当の想定: Opus。大きい（約 14 LW）ので、p032 の後に Q1 が次の 4 つに分けて起動してよい（各々の phase.md の節をそのまま使う）。どれも実機の Windows でしか確かめられない部分が多く、Wine の確かめは build と起動まで。

## a. TSF（N2 の後の段）

- `ITfThreadMgr`・`ITfDocumentMgr`・`ITextStoreACP2` を C の vtable で（`libkeiland/win32/tsf.c`、COM は `CoInitializeEx(COINIT_APARTMENTTHREADED)` を窓の thread で）。text store は `kl_window_text_context` の周りの文字と caret・選び、composition は `ITfContextOwnerCompositionSink` で PREEDIT・COMMIT、`GetTextExt` は `kl_window_text_cursor` の矩形。TSF が使えない時（初期化の失敗）は p029 の IMM32 に落ちる。再変換・touch keyboard（`ITfInputScope`）・絵文字の panel（Win+.）は TSF の上で動く。

## b. drag と drop・clipboard の残り（N6）

- 貼り付け（page への）の口が libkeiland に無ければ、keiland_window.h に足すかを Q1 に（公開の header の変更。WS074 の browser の UI の要る物を P8・Q1 と調整）。`CF_HTML` の copy と貼り付け（`cf-html.c`）。OLE の `DoDragDrop`（`kl_window_start_drag_icon`・`kl_window_drag_fill` の Windows の実装、`IDataObject`・`IDropSource` を C の vtable で）と drop の受け（`RegisterDragDrop`、`IDropTarget`）。

## c. touchpad・pen・touch（N7）

- `EnableMouseInPointer(TRUE)` で `WM_POINTER*` に寄せ、`POINTER_INPUT_TYPE`（`input.h` の定数）で mouse・touch・pen を分ける。pen は `GetPointerPenInfo` の圧力（0〜1024 → 0〜1）・傾き → `kl_window_event` の pressure・tilt。精密 touchpad の慣性と pinch は DirectManipulation（`IDirectManipulationManager`、C の vtable）で `KL_WINDOW_AXIS`（axis_source FINGER）と `KL_WINDOW_AXIS_STOP`。

## d. 見た目と保存

- accent の色（`DwmGetColorizationColor`）を theme の accent に、`WM_SETTINGCHANGE`（"ImmersiveColorSet"）で暗い mode と accent を追従。Mica（`DwmSetWindowAttribute(DWMWA_SYSTEMBACKDROP_TYPE = 38, DWMSBT_MAINWINDOW)`、Windows 11 22H2 以降、無ければ何もしない）と題の帯の背景の透過。
- modal な大きさの変更の間の描き直し（p028 の制限）: `WM_ENTERSIZEMOVE` の間 `SetTimer` の `WM_TIMER` の中から app の描画を進める口（`kl_app` の内部の callback）を足すかを、p028 の結果と合わせて Q1 が決める。
- DPAPI（U14）: files の backend とは別に、cookie の space の record を `CryptProtectData`（current user）で包む backend の包み（`storage/` ではなく browser の UI の側の、`browser_storage_calls` を包む program の backend。libbrowser を変えない）。
- arm64: `make browser-winarm64` の zip と、Wine（arm64 の Wine は host に無い）での確かめは未実施。実機の Windows 11 arm64 で M1〜M8（ユーザーの一覧に）。

## 受け入れ（各々）

- `cross-build.sh` PASS（両方の arch）、`wine-run.sh` の今の項が退行しない、新しい口は Wine で起動と build まで、実機の確かめの項をユーザーの一覧に（Q1）。commit、結果の節、ws.md の行。

## 追加の範囲（2026-11-02 Q1）

- p032 の Q1 の判断で、system の theme（暗い mode）を `prefers-color-scheme` で頁に渡す件をこの Phase に入れた（zedBSD の Keiland でも同じ口で theme に従う形。libbrowser の変更は最小に）。
- 同日 Q1 が、matchMedia の追従のために `plan/ws074/phase230/prelude.js` の 2 行と `prelude-source.inc` の作り直し・`bind/prelude.c` の native 1 つ、公開の口 `browser_view_set_color_scheme` の追加（BROWSER_API_VERSION は上げない）を範囲に入れた。
- 同日 Q1 が、DPAPI のために `<browser/storage.h>` に他の backend を包む backend 用の sink の 3 関数（create・bytes・destroy、BROWSER_STORAGE_VERSION は上げない）を足す案 A を選んだ（今の公開の口では包む backend が中の backend の record の bytes を受け取れないため）。

## 結果（2026-11-02 02:50、P5、q1886-i01、基点 4f58e7b9b）

Wine の結果は参考で、実機の Windows の判定ではない（決定 20）。

### 実装した物

| 項 | 中身 |
| --- | --- |
| theme の頁への受け渡し | libbrowser: `css/media.c` の `prefers-color-scheme: light|dark` を構文の時の定数から style の時の試験（`MEDIA_FEATURE_COLOR_SCHEME`）にし、`css_media_matches` に scheme を渡す。css の engine・page・bind の window に scheme を持たせ、変わると計算済みの style を捨てる。公開の口 `browser_view_set_color_scheme(view, BROWSER_COLOR_SCHEME_LIGHT|DARK)`（view の頁と後の頁に、変われば頁を組み直す）。JS の `matchMedia('(prefers-color-scheme: dark|light)')` は native の `colorScheme()` を見る。iframe は作られた時の親の scheme（走行中の変更は子の frame に伝えない）。browser の shell（zedBSD・Linux・Windows 共通）: view を作った直後に `kl_appearance_get(NULL)` を渡し、`KL_APP_THEME` で渡し直す（`ZBROWSER SCHEME dark=0|1` の行）。同梱の start・about・text.html に `@media (prefers-color-scheme: dark)` の色 |
| d accent と追従 | `libkeiland/win32/theme.c`: `DwmGetColorizationColor` の色を 8 つの accent の最寄りに（彩度 0.2 未満は graphite、他は色相の最寄り）。`WM_SETTINGCHANGE "ImmersiveColorSet"` と `WM_DWMCOLORIZATIONCOLORCHANGED` で読み直し、変われば各窓の暗い題の帯を直して `KL_APP_THEME` を program に（`app.c` の post の道を共通の `app_post_event` に） |
| d Mica | `window.c`: `DWMWA_SYSTEMBACKDROP_TYPE`(38) に `DWMSBT_MAINWINDOW`（無い system は拒むだけ） |
| DPAPI | `<browser/storage.h>` に `browser_storage_sink_create`・`_bytes`・`_destroy`（`storage/records.c`）。`apps/browser-win32/protect.c`: files backend を包み、cookie の space の record を `CryptProtectData`（今の利用者、UI 無し）で包んで magic `OGDPAPI1` の後に置く。magic の無い record（前の平文）はそのまま読み、次の変更で包む。解けない物は無い record。他の space はそのまま。shell は `shell_storage_wrap`（POSIX は `main-posix.c` で何もしない） |
| c touch と pen | 新 `libkeiland/win32/pointer.c`: content の `WM_POINTER*` のうち touch を `KL_WINDOW_TOUCH_*`、pen を `kl_window_accept_tablet` した窓に `KL_WINDOW_TABLET_*`（圧力 0〜1024 → 0〜1、傾き、barrel・eraser）。他は `DefWindowProcW`（mouse の message になる）。**phase.md との違い**: `EnableMouseInPointer(TRUE)` は呼ばない（process の全ての窓の mouse を pointer の message に移し、確かめ済みの mouse の道を実機で確かめられないまま変えるため） |
| b drag out | 新 `libkeiland/win32/drag.c`: `kl_window_start_drag_icon`・`kl_window_start_drag`・`kl_window_drag_text`・`kl_window_drag_fill` を OLE の `DoDragDrop`（C の vtable の `IDataObject`・`IDropSource`）で。start は型と data を写して frame に message を post し、次の dispatch で（`drag_fill` の後に）`DoDragDrop`、終わりに `KL_WINDOW_DRAG_DONE`。`image/png` は登録の形式 "PNG"、文字は `CF_UNICODETEXT`、`text/html` は `CF_HTML`（cf-html.c）。drag の絵は出さない（system の cursor） |
| arm64 | `cross-build.sh` で winarm64 の .exe と zip を作る（下）。実行は未実施 |

commit（`git commit -m WIP -- <path>`）: 4f005bf5f（上の全部と試験の道具）、末尾の commit（`allowed-imports.txt` の追加・`wine-run.sh` の M7p の直し・この記録）。

### 確かめた事

| 確かめ | 結果 |
| --- | --- |
| `timeout 5400 sh plan/ws334/tests/wine-run.sh`（3 回目） | `wine-run: 18 passed, 0 failed, 5 not run (a real Windows)`、`wine-run: PASS`。p032 の 15 項は全部 PASS のまま。新しい項: M7p（暗い prefix で頁の地の明るさ 29、明るい prefix で 240、最初の `ZBROWSER SCHEME` が dark=1 と dark=0）、M7t（走行中に `theme-set light` → 頁 240・題の帯 253、`theme-set dark` → 頁 29、SCHEME の行 dark=1 dark=0 dark=1）、M6p（M6 の後の cookie の record が全部 `OGDPAPI1` で始まる、`protect-test` 38 checks PASS）。新しい未実施: M7m（Mica と system の accent。Wine に DWM の色が無い） |
| 1・2 回目の wine-run | 1 回目 M7p・M7t FAIL: 頁の地を測る点（x 1200）が 1024 幅の窓の外だった（PNG では頁は暗かった）→ x 950 に。2 回目 M7p FAIL: theme を切り替える回は SCHEME の行が 3 つあり数え方が誤り → 最初の行を見る。どちらも script の誤り |
| `timeout 5400 flock /tmp/zedbsd-wine.lock sh plan/ws334/tests/cross-build.sh`（2 回目） | `cross-build: PASS`。両 arch の OmniGrammaton.exe warning 0、import は system の DLL 24 と遅延の vulkan-1.dll（p032 と同じ数）、libkeiland.a と libbrowser-ui.a の import 205（win64）・201（winarm64）が表の中、libkeiland.a が公開の関数 573 を定義、zip 23 file・作り直して同じ bytes（winarm64 も）。1 回目は新しい import（DPAPI・OLE・pointer・DWM の accent）が表に無く FAIL → `allowed-imports.txt` に足した |
| `sh plan/ws334/tests/scheme-test.sh plain`（新） | `test-scheme: PASS (83 checks)`: 16 の media query を両 scheme で、light の view・先に dark にした view（matchMedia と getComputedStyle と style の dump）・読み込んだ後の切り替えと戻し。変異（`media_test_holds` で dark を無視）で 12 件 FAIL を確かめて戻した |
| 段 1 の前後（`stage1.sh TREE plain`、前は `git archive 4f58e7b9b`、後は `git archive 4f005bf5f`） | 同じ: host-build exit 0・warning 0・built 94、host-* 75 exit 0・17 その他（試験ごとの exit が diff なし、`host-color-media` 39/39）、js 17/17、loader 11/11、http 15/15、http-async 19/20（前からの async-sheets）、fetch 38/38、component 83 checks |
| 段 1 の asan の前後（`stage1.sh TREE asan`、同じ 2 つの tree） | 同じ: host-build exit 0・warning 0・built 94、host-* 75・17（試験ごとの exit が diff なし）、js 17/17、loader 11/11、http 15/15、http-async 19/20（同じ）、fetch 38/38、component 83 checks |
| `PORT=18535 sh plan/ws334/tests/storage-test.sh plain` | `test-storage: PASS (222 checks)`。1 回目は `short=5; Max-Age=2` の cookie が host の負荷（同時に stage1 と cross-build）で 2 秒を過ぎ 2 件 FAIL、基点の tree でも同じ試験は PASS、負荷を下げた 2 回目 PASS（時間に依る試験。この Phase の変更と無関係） |
| `python3 -I plan/ws334/tests/keiland-stubs.py` | `unsupported.c` を作り直した（`kl_window_accept_tablet`・`start_drag`・`start_drag_icon`・`drag_text`・`drag_fill` の stub が消えた、314 stubs）、PASS |
| Linux の Keiland（`make -f userland/desktop/keiland-linux.mk all KEILAND_LINUX_BUILD=build/tmp/p033/keiland-linux`） | exit 0、warning 0 |
| zedBSD amd64（`make ZEDBSD_CONFIG=plan/ws334/tests/config-amd64-p027-guest.mk BUILD=build/p033 build/p033/dynamic/libkeiland.so build/p033/dynamic/libbrowser.so build/p033/bin/browser`） | exit 0、warning 0 |
| `BASE=d393d3267 sh plan/ws334/tests/check-skeleton.sh` | FAIL: `libkeiland/platform/platform.h`（基点からの物、p035）と `include/browser/storage.h`（Q1 が認めた sink の 3 関数の追加） |
| `git diff --check` | 出力なし |
| PNG（`build/review/ws334-p033/`） | 新: `m7-dark.png`（暗い mode の頁）・`m7-to-light.png`・`m7-to-dark.png`（走行中の切り替え）。他は p032 と同じ名前 |

### 未実装（理由と案、Q1 へ）

- **a TSF**: 実装していない。Wine の msctf には TSF の IME が無く、`ITextStoreACP2`（約 28 の method）と composition の sink を一度も動かせない。実機で誤る text store は、今動いている IMM32（p029）の入力を壊す。案: Windows の host の session（決定 20）か、実機で試せる時に別の Phase で。
- **b drop の受け**（`RegisterDragDrop`・`IDropTarget`、`kl_window_accept_drops` ほか）: 実装していない。OmniGrammaton の shell は drop を受けない（`kl_window_accept_drops` を呼ばない）ので、ブラウザには今の効果が無い。libkeiland の全体の移植（決定 21 の U1、WS234）で。**頁への貼り付けの口**は前の通り未決（Q1 の設計の小 Phase）。HTML の copy は libkeiland・libbrowser に口が無い（drag の `text/html` だけ CF_HTML）。
- **c DirectManipulation**（精密 touchpad の慣性・pinch、`KL_WINDOW_AXIS_STOP`）: 実装していない。Wine に DirectManipulation が無く確かめられない。touchpad の scroll は今の `WM_MOUSEWHEEL`（axis_source WHEEL）で動き、慣性と AXIS_STOP が無いだけ。
- **d modal な大きさの変更の間の描き直し**: 実装していない。program の描画を libkeiland の `WM_TIMER` から呼ぶには公開の口（例 `kl_app_set_modal_draw(app, fn, data)`、`WM_ENTERSIZEMOVE`〜`WM_EXITSIZEMOVE` の間 `SetTimer` の `WM_TIMER` から呼ぶ）が要り、phase.md の通り Q1 の判断。
- **d 題の帯の背景の透過**（Mica を題の帯に見せる）: 実装していない。題の帯は不透明の DIB で、Windows の Vulkan の swapchain も不透明（composite alpha は opaque だけの driver が多い）。透過には題の帯を premultiplied の alpha で描き frame を題の帯の高さまで広げる必要があり、Wine では DWM が無く確かめられない。今は Mica の属性を置くだけ（描かない所と大きさの変更の隙間に見える）。

### 未実施（実機の Windows、ユーザーが拾える試験の一覧へ Q1 が）

- 暗い mode・accent（設定の色の変更で題の帯と頁が追従）と Mica（Windows 11 22H2）、Windows 10 で Mica の属性が無害なこと。
- touch screen と pen（`kl_window_accept_tablet` を使う program は今ブラウザに無いので、ブラウザでは pen は mouse、touch は shell の touch.c のスクロールと tap）。
- 頁の画像を Explorer・Word・Paint に drag して PNG が渡る、文字が渡る。
- DPAPI: 別の利用者・別の機械では cookie が読めず（ログアウトの状態になる）、同じ利用者では残る。
- winarm64 の .exe（Windows 11 arm64）で M1〜M8。

消してよい物（Q1 へ）: `build/tmp/p033`（試しの log・build・protect の dir）、`build/tmp/p033-base`・`build/tmp/p033-tip`（段 1 の前後の展開と出力）、`build/tmp/p033-*.log`、`build/p033`（zedBSD amd64 の試しの build）、`build/tmp/ws334-scheme.run.*`・`build/tmp/ws334-storage.run.*`・`build/tmp/ws334-wine-run.run.*`・`build/tmp/ws334-cross.run.*` のうち最後の物以外、`build/tmp/ws334-keiland-stubs`。prefix `build/tmp/ws334-wine-dark` は M7t が最後に dark に戻すので次もそのまま使える。
