<!-- awesome-plan project=zedbsd record=ws334-p028 -->

# ws334-p028 統合: libkeiland の Win32 その 1 と OmniGrammaton.exe（窓・loop・入力・枠・描画・入口）

- 親: [WS334](../ws.md)、設計 [design.md](../design.md) の §4.1〜§4.7、§6.2・§6.4・§6.5、決定 14・15・20。
- 状態: planned（2026-11-01 p003 で作成）。
- 依存: p014（keys-table）・p015（frame）・p016（platform-win32）・p017（wide・input）・p025・p026・p027。
- 担当の想定: Opus（phase-runner）を 1 回（約 14 LW。UI なので標準廉価実装手順の module にしない。使う module の約束は各 header）。

## 決めてある形（実装の間に変えない。変えるなら止めて Q1 に）

- **置き場**: `userland/desktop/libkeiland/win32/` に `app.c`（kl_app）・`window.c`（kl_window と window procedure）・`present.c`（Vulkan の surface と CPU の表示）・`places.c`（場所）・`theme.c`（暗い mode）・`unsupported.c`（stub）と private の `win32/window-win32.h`（`struct kl_window` と `struct kl_app` の Windows の形。Wayland の `ui/window.h` は include しない）。`windows.h` を include してよいのはこの directory と `platform/*-win32.c` だけ。
- **include の path**: libkeiland の directory（`-Iuserland/desktop/libkeiland`）を include の path に入れない。libkeiland の `process.h` が CRT の `<process.h>`（winpthreads の `pthread.h` が include する）を隠すため（p002 で確かめた）。libkeiland の file は今の通り tree の上からの path で include する。
- **共通の file の棚卸し**: `libkeiland/sources.mk` の `LIBKEILAND_SOURCES` のうち、`wayland`・`wl_`・`xdg_`・`zwp_`・`<unistd.h>`・`<poll.h>`・`<sys/`・`<dlfcn.h>`・`ui/window.h`・`ui/app.h`（Wayland の構造体）を include も参照もしない file を grep で選び（script `plan/ws334/tests/keiland-common.sh`、結果を `plan/ws334/keiland-common.txt` に）、Windows の libkeiland は「共通の file ＋ win32/*.c ＋ platform/platform-win32.c・thread-win32.c・socket-win32.c」。共通の file が mingw で compile できない時は、その file を共通から外して `unsupported.c` の stub に回す（file を直さない。直しが要る物は一覧にして Q1 に）。
- **公開の関数の全部**: `libkeiland/exports.map`（か `exports.py` が作る一覧）の全部の関数が `libkeiland.dll` にある。下の「最小の部分」と共通の file に無い物は `unsupported.c` の stub: int を返す関数は ENOTSUP、pointer は NULL、void は何もしない。stub の file は一覧から生成する script（`plan/ws334/tests/keiland-stubs.py`、`python3 -I`）で作り、生成物を commit する（手で直さない）。Wayland の型を返す関数（`kl_app_display` など）も NULL。
- **最小の部分**（design §4.5 の表の通り。browser の UI が呼ぶ物）:
  - app: `kl_app_open`（`kl_app_options.application` を窓の class の副名と storage の名前に）・`kl_app_window_create`（`KL_PRESENT_NONE`・`KL_PRESENT_SHM`・`KL_PRESENT_VULKAN`。VULKAN は SHM と同じ CPU の表示で出す）・`kl_app_dispatch(timeout)`（`MsgWaitForMultipleObjectsEx` で wake の event〈`keiui_wake_event`〉と message を待ち、`PeekMessageW`・`TranslateMessage`・`DispatchMessageW` で全部を流す）・`kl_app_take`・`kl_app_post`（内部の queue と wake。`PostMessageW` は使わない）・`kl_app_close`・`kl_app_system`（NULL）。
  - window: 窓の class "KeilandWindow"、`WS_OVERLAPPEDWINDOW`、DWM の影（`DwmExtendFrameIntoClientArea` に上 1 px）、`WM_NCCALCSIZE` で client を窓の全部に（最大化の時は monitor の作業域に収める）、`WM_NCHITTEST` は `keiui_win32_frame_hit`（controls は p029 まで空）、`WM_GETMINMAXINFO`、`WM_DPICHANGED` で示された矩形へ `SetWindowPos`、`kl_window_size`・`kl_window_set_title`（UTF-8 → UTF-16 は `wide.h`）・`kl_window_close`。
  - 入力: `WM_KEYDOWN`・`WM_SYSKEYDOWN`・`_UP` → `KL_WINDOW_KEY`（code は `keiui_win32_key_code`、flags は `keiui_win32_key_flags`、modifiers は `keiui_win32_modifiers`（`GetKeyState` から）、押下の text は `TranslateMessage` の後に queue にある `WM_CHAR`・`WM_SYSCHAR` を `PeekMessageW(PM_REMOVE)` で取って `keiui_win32_char_unit`・`keiui_win32_utf8_from_utf16` で UTF-8 に。制御の文字（U+0000〜U+001F・U+007F）は text に入れない）。押下に続かない `WM_CHAR`（Alt+数字など）は `KL_WINDOW_TEXT_COMMIT`。`EnableMouseInPointer(TRUE)` は使わず（第 1 段は mouse の message で十分。pointer の口は p033）、`WM_MOUSEMOVE`・`WM_*BUTTON*`（`keiui_win32_button_code`、`SetCapture`・`ReleaseCapture`）・`WM_MOUSELEAVE`（`TrackMouseEvent`）・`WM_MOUSEWHEEL`・`WM_MOUSEHWHEEL`（`keiui_win32_wheel_pixels`、axis_source WHEEL。座標は `ScreenToClient`）、`WM_SETFOCUS`・`WM_KILLFOCUS` → `KL_WINDOW_FOCUS`、`WM_SIZE` → `KL_WINDOW_RESIZE`、`WM_CLOSE` → `KL_WINDOW_CLOSE`（窓は app が閉じるまで残す）。時刻は `kl_clock_us`。
  - event の queue: 窓ごとに 512 の環。満ちたら最も古い MOTION を捨て、MOTION が無ければ新しい event を捨てて数を数える。
  - 大きさの変更の modal の loop（`WM_ENTERSIZEMOVE`〜`WM_EXITSIZEMOVE`）: 第 1 段では loop の間は app が描かない（DWM が前の絵を伸ばす）。`WM_EXITSIZEMOVE` で最後の RESIZE を出す。結果の節に制限として書く（p033 で直すかは Q1）。
  - 描画: `kl_window_vulkan_extensions`（`VK_KHR_surface`・`VK_KHR_win32_surface`）、`kl_window_vulkan_surface`（`vkCreateWin32SurfaceKHR` を `vkGetInstanceProcAddr` で引く。libkeiland は Vulkan を import しない）。CPU の表示（`kl_window_present`・`_part`・`_parts`・`_present_resize`）は DIB section に写し `InvalidateRect` → `WM_PAINT` で `BitBlt`。
  - 暗い mode: `HKCU\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize` の `AppsUseLightTheme`（DWORD、無ければ light）を読み、`DwmSetWindowAttribute(DWMWA_USE_IMMERSIVE_DARK_MODE = 20)`、libkeiland の theme の暗い・明るいを選ぶ（libkeiland の theme の口が Wayland の settings に頼るなら、Windows では起動の時の 1 回だけ。`WM_SETTINGCHANGE` の追従は p033）。
  - 場所: `kl_app_storage_directory` は `SHGetKnownFolderPath(FOLDERID_LocalAppData)` + "\\" + application（UTF-8）。`kl_data_directory` は .exe の directory + "\\share"。
  - 題の帯（titlebar の controls）・IME・clipboard・DPI の倍率は p029。この Phase の窓の題の帯は libkeiland の canvas で題の文字と 3 つの button（`keiui_win32_frame_buttons` の場所、hover と押下の色は theme から）だけを CPU で描いた帯（Vulkan の窓では browser の UI が描く content の上に帯の分を空ける: client の上の caption の高さ 32 px を content から除く）。
- **apps/browser-win32/**:
  - `main.c`: `wWinMain`（`OmniGrammaton.exe`、`-mwindows`）と `wmain`（`omnigrammaton-cli.exe`、console）。どちらも最初に `SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32)`、`CommandLineToArgvW` か wmain の argv を `wide.h` でなく `WideCharToMultiByte(CP_UTF8)` で UTF-8 に（apps の file は libkeiland の private の header を使わない）、cli は `_setmode(_fileno(stdout), _O_BINARY)` と stderr も（O29）、`browser_main(argc, argv)` を呼ぶ。
  - `single.c`: GUI の時だけ。名前付きの mutex "Local\\OmniGrammaton-SingleInstance"。既にあれば `FindWindowW(L"KeilandWindow", NULL)` のうち property "OmniGrammaton" を持つ窓に `WM_COPYDATA`（dwData 0x4F474D31、UTF-8 の URL の列）を送って終わる。受け手は libkeiland の window procedure が `WM_COPYDATA` を `KL_WINDOW_POST`（code 0x4F474D31、data は malloc の写し）にし、browser の UI が URL を開く（`browser/shell/` の POST の扱いに 1 つ足す。p027 の後の browser の UI の小さな変更、Q1 に伝える）。
  - `resources/OmniGrammaton.rc`（`VERSIONINFO`: ProductName "OmniGrammaton"、FileDescription "OmniGrammaton Web Browser"、CompanyName "Awe Morris"、LegalCopyright "(C) 2026 Awe Morris"、版は `RELEASE` の版の番号の決め方を Q1 に聞いて 1 つの変数に）・`OmniGrammaton.manifest`（PerMonitorV2、activeCodePage UTF-8、supportedOS Windows 10・11、longPathAware）。icon は p031 まで無し（rc に入れない）。
  - `Makefile`: `libkeiland.dll`（共通の file・win32・platform-win32・thread-win32・socket-win32、`-lws2_32 -liphlpapi -ldwmapi -lshcore -limm32 -lshell32 -lole32 -luuid -lgdi32 -luser32`）、`libbrowser.dll`（p023 の .def）、`libmedia.dll`（p026）、`OmniGrammaton.exe`・`omnigrammaton-cli.exe`（browser の UI の source から `main-posix.c` を除いた物 ＋ main.c・single.c ＋ resource の .o、`-DSHELL_APPLICATION_ID=\"OmniGrammaton\"`）、`OmniGrammaton/` の directory に並べる（share/fonts と share/browser の写しを含む。license と zip は p031）。Vulkan は libbrowser・UI の `vk*` が遅延の import（p020 の `libvulkan-1.a` と `-Wl,--delayload=vulkan-1.dll`）。
- **Wine の試験の script** `plan/ws334/tests/wine-run.sh`（1 本で: build → `xvfb-run -a -s "-screen 0 1280x800x24"` の中で Wine に .exe を起動 → 決めた秒数の後に `import -window root` で PNG → 終わりの status と log を集める → 要約の最後の行）。Wine の prefix は `build/tmp/ws334-wine-gui`（unit の物と別）。Vulkan は host の lavapipe（`VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json`）。

## 試験（受け入れ）

- `cross-build.sh` PASS: `libkeiland.dll` と 2 つの .exe が両方の arch で warning 0、`libkeiland.dll` の export の名前が Linux の `libkeiland.so` の export と同じ集まり（`nm -D` と `objdump -p` を比べる）、依存の DLL が許す表の中。
- Linux の Keiland の build と libkeiland の host の試験の件数が前と同じ（`make keiland-linux`）。
- Wine＋xvfb-run（`wine-run.sh`）: (1) `omnigrammaton-cli.exe --js` と `--dump=layout`（同梱の start.html）が Linux の host の `/bin/browser` と同じ bytes、(2) `OmniGrammaton.exe` が起動し start.html を lavapipe の Vulkan で描いた PNG（文字が読める）、(3) `OmniGrammaton.exe --no-gpu` の PNG、(4) 2 つ目の起動が 1 つ目の窓に URL を渡して終わる（log）。PNG は `build/review/ws334-p028/` に写し、path を Q1 に（ユーザーに見せるのは Q1）。Wine の結果は参考で、実機の Windows の判定ではない（決定 20、design §7.1）。
- `run-unit.sh all`・`check-skeleton.sh`（BASE）PASS。`git diff --check`。

## 変えてはいけない物と終わり

- module の header と .c、試験の道具（modules.sh・run-unit.sh・check-skeleton.sh・unit/）。Wayland の libkeiland の file（`ui/` ほか）。compositor・libkeiland-backend。HAL・toolchain・共有の build/。rm をしない。push しない。
- file の群ごとの commit、SHA・PNG の path・制限（modal の loop）を Q1 に、結果の節と ws.md の行。
