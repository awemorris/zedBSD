<!-- awesome-plan project=zedbsd record=ws334-p033 -->

# ws334-p033 統合: 後半の Windows の口（TSF・drag と drop・touchpad と pen・accent・Mica・DPAPI・arm64）

- 親: [WS334](../ws.md)、設計 [design.md](../design.md) の §4.4 の「後」の列、§4.9、決定 21（U14: DPAPI は後半）。
- 状態: planned（2026-11-01 p003 で作成）。
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
