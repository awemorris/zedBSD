<!-- awesome-plan project=zedbsd record=ws334-p028 -->

# ws334-p028 統合: libkeiland の Win32 その 1 と OmniGrammaton.exe（窓・loop・入力・枠・描画・入口）

- 親: [WS334](../ws.md)、設計 [design.md](../design.md) の §4.1〜§4.7、§6.2・§6.4・§6.5、決定 14・15・20。
- 状態: in-progress（実装済み・試験 PASS、cleared は Q1。2026-11-01 q1862-i01、P5）。前: planned（2026-11-01 p003 で作成）。
- **2026-11-01 の変更（DLL を作らず 1 つの .exe、libkeiland の Windows は pthread 無し。ws.md のユーザーの決定、design §6.2・§6.6、build の形は p036）**: `libkeiland.dll` を作らず static library `libkeiland.a`。export の照合（`nm -D` と `objdump -p`）の代わりに、`libkeiland/exports.map` の全部の関数が `libkeiland.a` の定義の名前（`llvm-nm --defined-only`、stub を含む）にある事を照合する。`omnigrammaton-cli.exe` は作らない（2026-11-01 ユーザーの答え: 配るのは `OmniGrammaton.exe` だけ）。`main.c` は `wWinMain` の 1 つで、console の出力（`AttachConsole` など）を作り込まない。Wine の (1)（`--js`・`--dump=layout` の bytes の比較）は試験の program（`plan/ws334/tests` の配らない .exe、p036 の `test-program` で同じ static library を link）で行う。.exe の link は p036 の `WIN32_PROGRAM` の形、libkeiland の Windows の object は pthread を参照しない（p036 の確かめを続ける）。下の本文はこの通りに読み替える。
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
  - **窓の形（design-reviewer の指摘 #6 で決めた）**: Keiland の窓 1 つ = 親の HWND（枠・題の帯・hit test）と、その client の題の帯の下を覆う子の HWND（`WS_CHILD | WS_VISIBLE`、class "KeilandContent"）。page の中身は子に出す: Vulkan の surface（`kl_window_vulkan_surface`）は子の HWND に作り、CPU の表示（DIB）も子に描く。題の帯は親が CPU（DIB と `BitBlt`）で描く。これで Vulkan の swapchain が題の帯を覆わない。`kl_window_size` と RESIZE の大きさ・pointer の座標は**子**の物（Wayland で窓の大きさが compositor の題の帯を含まないのと同じ）。子の mouse・key の message は親と同じ window procedure の道で event に（子は focus を親から受けたら親に返す。key は focus を持つ親で受ける: 子に `SetFocus` しない）。全画面では子が client の全部。
  - 題の帯（titlebar の controls）・IME・clipboard・DPI の倍率は p029。この Phase の題の帯は libkeiland の canvas で題の文字と 3 つの button（`keiui_win32_frame_buttons` の場所、hover と押下の色は theme から）だけを親に描く。caption の高さは 32 px（96 dpi）。
  - **caption の button の mouse**: hit test が MINIMIZE・MAXIMIZE・CLOSE を返すと、その上の mouse は `WM_NCMOUSEMOVE`・`WM_NCLBUTTONDOWN`・`WM_NCLBUTTONUP`・`WM_NCMOUSELEAVE`（`TrackMouseEvent` の `TME_NONCLIENT`）で来る。これらを自分で扱い（hover と押下を描き、button の上で離したら `ShowWindow(SW_MINIMIZE)`・最大化と戻し・`WM_CLOSE`）、`DefWindowProcW` に渡さない（渡すと古い形の button を描く）。`HTMAXBUTTON` を返すことで Windows 11 の snap の配置が出る（Wine では出ない）。
- **apps/browser-win32/**:
  - `main.c`: `wWinMain`（`OmniGrammaton.exe`、`-mwindows`）と `wmain`（`omnigrammaton-cli.exe`、console）。どちらも最初に `SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32)`、`CommandLineToArgvW` か wmain の argv を `wide.h` でなく `WideCharToMultiByte(CP_UTF8)` で UTF-8 に（apps の file は libkeiland の private の header を使わない）、cli は `_setmode(_fileno(stdout), _O_BINARY)` と stderr も（O29）、`browser_main(argc, argv)` を呼ぶ。
  - `single.c`: GUI の時だけ。名前付きの mutex "Local\\OmniGrammaton-SingleInstance"。既にあれば `FindWindowW(L"KeilandWindow", NULL)` のうち property "OmniGrammaton" を持つ窓に `WM_COPYDATA`（dwData 0x4F474D31、UTF-8 の URL の列）を送って終わる。受け手は libkeiland の window procedure が `WM_COPYDATA` を `KL_WINDOW_POST`（code 0x4F474D31、data は malloc の写し）にし、browser の UI が URL を開く（`browser/shell/` の POST の扱いに 1 つ足す。p027 の後の browser の UI の小さな変更、Q1 に伝える）。
  - `resources/OmniGrammaton.rc`（`VERSIONINFO`: ProductName "OmniGrammaton"、FileDescription "OmniGrammaton Web Browser"、CompanyName "Awe Morris"、LegalCopyright "(C) 2026 Awe Morris"、版は `apps/browser-win32/version.mk` の `OMNIGRAMMATON_VERSION := 0.1.0`（ユーザーが版の決め方を決めるまでの仮の値。p031 の zip の名前も同じ変数）・`OmniGrammaton.manifest`（PerMonitorV2、activeCodePage UTF-8、supportedOS Windows 10・11、longPathAware）。icon は p031 まで無し（rc に入れない）。
  - `Makefile`: `libkeiland.dll`（共通の file・win32・platform-win32・thread-win32・socket-win32、`-lws2_32 -liphlpapi -ldwmapi -lshcore -limm32 -lshell32 -lole32 -luuid -lgdi32 -luser32`）、`libbrowser.dll`（p023 の .def）、`libmedia.dll`（p026）、`OmniGrammaton.exe`・`omnigrammaton-cli.exe`（browser の UI の source から `main-posix.c` を除いた物 ＋ main.c・single.c ＋ resource の .o、`-DSHELL_APPLICATION_ID=\"OmniGrammaton\"`）、`OmniGrammaton/` の directory に並べる（share/fonts と share/browser の写しを含む。license と zip は p031）。Vulkan は libbrowser・UI の `vk*` が遅延の import（p020 の `libvulkan-1.a` と `-Wl,--delayload=vulkan-1.dll`）。
- **Wine の試験の script** `plan/ws334/tests/wine-run.sh`（1 本で: build → `xvfb-run -a -s "-screen 0 1280x800x24"` の中で Wine に .exe を起動 → 決めた秒数の後に `import -window root` で PNG → 終わりの status と log を集める → 要約の最後の行）。Wine の prefix は `build/tmp/ws334-wine-gui`（unit の物と別）。Vulkan は host の lavapipe（`VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json`）。

## 試験（受け入れ）

- `cross-build.sh` PASS: `libkeiland.dll` と 2 つの .exe が両方の arch で warning 0、`libkeiland.dll` の export の名前が Linux の `libkeiland.so` の export と同じ集まり（`nm -D` と `objdump -p` を比べる）、依存の DLL が許す表の中。
- Linux の Keiland の build と libkeiland の host の試験の件数が前と同じ（`make keiland-linux`）。
- Wine＋xvfb-run（`wine-run.sh`）: (1) `omnigrammaton-cli.exe --js` と `--dump=layout`（同梱の start.html）が Linux の host の `/bin/browser` と同じ bytes、(2) `OmniGrammaton.exe` が起動し start.html を lavapipe の Vulkan で描いた PNG（文字が読める）、(3) `OmniGrammaton.exe --no-gpu` の PNG、(4) 2 つ目の起動が 1 つ目の窓に URL を渡して終わる（log）。PNG は `build/review/ws334-p028/` に写し、path を Q1 に（ユーザーに見せるのは Q1）。Wine の結果は参考で、実機の Windows の判定ではない（決定 20、design §7.1）。
- `plan/tools/keiland-os-boundary/check.sh` を流し、WS334 の file（`libkeiland/win32/`・`apps/browser-win32/`）に新しい失敗が無い（main の今の失敗の数と比べる）。`win32/*.c` は thread と lock に `pthread_*` を直に使わず `keiui_lock`・`keiui_thread_*`（platform.h）を使う（checker の T1・N1）。
- `run-unit.sh all`・`check-skeleton.sh`（BASE）PASS。`git diff --check`。

## 変えてはいけない物と終わり

- module の header と .c、試験の道具（modules.sh・run-unit.sh・check-skeleton.sh・unit/）。Wayland の libkeiland の file（`ui/` ほか）。compositor・libkeiland-backend。HAL・toolchain・共有の build/。rm をしない。push しない。
- file の群ごとの commit、SHA・PNG の path・制限（modal の loop）を Q1 に、結果の節と ws.md の行。

## 結果（2026-11-01 23:18、P5、q1862-i01、基点 10fa79a93）

commit（`git commit -m WIP -- <path>`）:

| SHA | 中身 |
| --- | --- |
| 8e025e7fd | `libkeiland/win32/`: `window-win32.h`（Windows の `struct kl_app`・`struct kl_window`）、`app.c`（kl_app・loop・post・take・`kl_clock_us`）、`window.c`（親と子の HWND・window procedure・入力の queue）、`present.c`（CPU の表示の DIB・Vulkan の surface と拡張・題の帯の描画）、`places.c`（storage と data の directory）、`theme.c`（暗い mode・`kl_appearance_get`・`kl_accent_get`）、生成の `unsupported.c`（stub 343 個） |
| fa361064e | browser の UI（`shell/internal.h`・`window.c`・`shell.c`: `SHELL_POST_OPEN` の受け取りと `SHELL_EVENT_OPEN`）と libbrowser `page/page.c`（下の 1 か所） |
| ae95d3572 | `apps/browser-win32/`（`main.c`・`single.c`・`single.h`・`version.mk`・`resources/OmniGrammaton.rc`・`.manifest`・Makefile）、`plan/ws334/keiland-common.txt`、`plan/ws334/tests/` の `keiland-common.sh`・`keiland-stubs.py`・`wine-run.sh`・`browser-cli-main.c`・`cross-build.sh`（3e・6a）・`allowed-imports.txt` |

確かめ:

| 確かめ | 結果 |
| --- | --- |
| `timeout 3500 sh plan/ws334/tests/wine-run.sh` | `wine-run: PASS`。(1) `--js`（closures・builtins・class）と start.html の `--dump=layout`（tree の font）が host の `build/ws074-host/plain/browser` と同じ bytes（283・996・269・18160）。(2) Vulkan（lavapipe）で start.html（1210 色）、(3) `--no-gpu` で同じ（1192 色）、(4) 2 つ目の起動（about.html）が exit 0 で終わり、1 つ目が `ZBROWSER OPEN handed=...about.html` と出して about.html を描いた |
| Wine の PNG（worktree の中） | `build/review/ws334-p028/gpu.png`・`nogpu.png`・`second.png`（題の帯の 3 つの button、頁の日本語の文字が読める）。Wine の結果は参考で、実機の Windows の判定ではない |
| `timeout 3600 flock /tmp/zedbsd-wine.lock sh plan/ws334/tests/cross-build.sh` | `cross-build: PASS`。win64・winarm64 とも `make browser-win64`・`browser-winarm64` が OmniGrammaton.exe まで warning 0。新しい 3e: libkeiland.a と libbrowser-ui.a の import が win64 167・winarm64 163 で全部が表の中、libkeiland.a が `exports.py --list` の 573 の公開の関数を全部定義（自前・共通の file・stub）。新しい 6a: OmniGrammaton.exe の import は system の DLL と UCRT の 23 個、vulkan-1.dll は遅延の import だけ。libkeiland.a は `pthread_` を参照しない（5） |
| `sh plan/ws334/tests/keiland-common.sh`・`python3 -I plan/ws334/tests/keiland-stubs.py --check` | 共通 33・除外 40、Makefile の一覧と同じで PASS。stub 343（公開 318、内部 25: chooser・picker・launch の keiui_*）、生成物と同じで PASS |
| 段 1（`stage1.sh plain`）と libkeiland の host の試験（`plan/tools/keiland-layers/host-test.sh`・`plan/tools/keiui/run-host-present-damage.sh`）の前後（前は `git archive 10fa79a93` を `build/tmp/p028-base` に展開） | 前後とも host-build exit 0・warning 0・built 94、host-* 75 exit 0・17 other（host-exits.txt が一致）、js 17/17、loader 11/11、http 15/15、http-async 19/20（同じ async-sheets）、fetch 38/38、component 83、layers pass=8 fail=0、present-damage 25 checks 0 failures。page.c の直しで POSIX の挙動が変わらないことはこれで見た |
| Linux の Keiland（`make -f userland/desktop/keiland-linux.mk all KEILAND_LINUX_BUILD=build/tmp/p028/keiland-linux`） | exit 0、warning 0（browser はこの build に入らない） |
| zedBSD amd64（`build/amd64/dynamic/libbrowser.so`・`libkeiland.so`・`build/amd64/bin/browser`、sysroot は main の写しに tree の header を重ねた物） | exit 0、warning 0 |
| `plan/tools/keiland-os-boundary/check.sh` | FAIL 62（T1・N1 は PASS）。WS334 の file（`libkeiland/win32/`・`apps/browser-win32/`・browser の shell）の失敗は 0。失敗は installer（A1〜A5）と `plan/tools/powervr/p009/vk-host-test.c`（B5）で、この Phase の外（`git archive` の展開では git に頼る検査が流れず、前の数は取れなかった） |
| `LC_ALL=C.UTF-8 sh plan/ws334/tests/run-unit.sh all` | `run-unit: PASS` |
| `BASE=d393d3267 sh plan/ws334/tests/check-skeleton.sh` | FAIL（`libkeiland/platform/platform.h` だけ。基点でも同じで p035 で扱う、p023 の Q1 の判断） |
| `git diff --check` | 出力なし |

自分で決めた点・phase.md と code の食い違い:

- **共通の file の規則**: 不完全な型の名前だけ（`struct wl_surface *` の宣言・pointer、`ui/internal.h` の前方宣言）は Wayland を使うことにならないので数えない（`keiland-common.sh` の頭に書いた）。数えると `ui/internal.h` を include する theme.c・widgets.c ほか 17 の file が全部外れ、theme が使えない。最後の判定は mingw の compile（warning 0）。
- **`ui/text.c` は共通にできない**（`<sys/stat.h>`・`<unistd.h>`。mingw で compile は通るが `open` に `O_BINARY` が無く Windows で font を text の mode で読み、`KEILAND_DATADIR` の固定の path も Windows に無い）。直しは file を直さない約束なので一覧に（Q1 の判断の通り p029 で扱う）。このため **題の帯の題の文字は描けない**（`kl_text_*` は stub。3 つの button だけ）。
- **入力の queue**: 窓ごとに 512 の環と、app の post の 512 の環。全部に app の通し番号を付け、`kl_app_take` が一番古い物を返す（窓の入力と post の順を保つ）。満ちた窓の環は最も古い MOTION を抜き、無ければ新しい入力を捨てて `dropped` を数える。
- **Vulkan の描き直し**: Wine（X11）では窓が画面に出る前に present した最初の絵が消え、browser は描き直さない（黒いまま）。子の HWND の `WM_PAINT` で、CPU の絵が無く `KL_PRESENT_NONE` で modal の loop の外なら `KL_WINDOW_RESIZE`（同じ大きさ）を queue に入れて描き直させる。本物の Windows は DWM が絵を保つので稀にしか来ない見込み。RESIZE を「描き直し」に使うのは公開の約束の外の使い方なので、専用の event を足すかは Q1 へ（問い 1）。
- **単一の起動**: libkeiland は窓（親の HWND）に application の名前（`kl_app_options.application`、OmniGrammaton）の property（値 1）を付ける。`single.c` は class "KeilandWindow" の窓を `FindWindowExW` で順に見て property のある物に `WM_COPYDATA`（`SendMessageTimeoutW`、5 秒）。URL の列は UTF-8 の文字列ごとに NUL、存在する file の path は `GetFullPathNameW` で絶対に。受け手（`window.c`）は malloc の写しに NUL を足し `KL_WINDOW_POST`（code 0x4F474D31）に。browser の UI は最初の URL を location に打った物として開き（`shell_follow_typed`）、写しを free する。
- **libbrowser の 1 か所（Q1 が範囲に入れた）**: `page/page.c` の `page_load_location` が Windows の drive の path（`Z:\...`）を scheme "z" の URL と読み EPROTONOSUPPORT を返していた（start.html が開けない）。`wb_os_path_is_absolute` が真なら file として読む（POSIX の絶対の path は前も URL でなく file だったので同じ）。
- **console の試験の program** `plan/ws334/tests/browser-cli-main.c`（`wmain`、stdout・stderr を binary、`browser_main`）。配らない。`test-program` に `TEST_PROGRAM_LDFLAGS=-municode` を足した。
- **Makefile**: browser の UI を `libbrowser-ui.a`（`main-posix.c` を除く、`-Iuserland/desktop/browser -DSHELL_APPLICATION_ID="OmniGrammaton"`）、`OmniGrammaton.exe` は `-mwindows -municode`、resource は windres（版の文字列の引用は `\"`）、`dist` が `OmniGrammaton/` に .exe と `share/fonts`（5 つ、zedBSD の名前 keiland*.ttf）・`share/browser`（3 つの html）を並べる。system の library に `-liphlpapi -ldwmapi -lshcore -limm32 -luuid -lgdi32 -luser32 -ladvapi32` を足した。libkeiland の file は `windows.h` より前に libkeiland の header を include する（`windows.h` の `interface`・`near` の macro が field の名前を変えるため）。
- `kl_app_language` は "en"、`kl_app_system` は NULL、`kl_accent_get` は青。暗い mode は起動の時に 1 回だけ読む。

制限（p029・p033 で扱うか Q1）:

- 大きさの変更の modal の loop（`WM_ENTERSIZEMOVE`〜`WM_EXITSIZEMOVE`）の間は描かない（最後の RESIZE は loop の終わり）。
- 題の帯の題の文字が無い（ui/text.c、上）。titlebar の controls（`kl_window_set_controls` ほか）は stub の ENOTSUP で、browser は「ZBROWSER ERROR titlebar error=129」を出して続く（p029）。
- 全画面（`kl_window_set_fullscreen`）、key の repeat の時刻（Windows の自動の repeat の `WM_KEYDOWN` をそのまま渡す）、`KL_APP_THEME`、DPI の倍率の中身への反映（題の帯と枠だけ dpi で描く）、IME・clipboard（p029）。
- Wine の dwmapi は stub（`DwmExtendFrameIntoClientArea`・`DwmSetWindowAttribute` の fixme）なので、影と暗い題の帯は Wine で確かめられない。

未実施:

- 本物の Windows（x86_64・arm64）での起動、snap の配置、DPI の変更、最大化・最小化・閉じるの button の click、大きさの変更、キー入力と日本語（Wine の上でも手で操作していない）。
- aarch64 の Windows の実行（link と表だけ）。
- 変異の確かめ（統合の Phase なので受け入れに無い）。

Q1 への問い:

1. Vulkan の窓の描き直しに `KL_WINDOW_RESIZE`（同じ大きさ）を使ってよいか。代わりに公開の header に描き直しの event（例 `KL_WINDOW_EXPOSE`）を足すなら KL_VERSION と Wayland の側（来ない）の約束が要る。
2. `ui/text.c` の `O_BINARY`・data の directory の直しは p029 でよいか（Q1 の判断の通りと理解）。

消してよい物（Q1 へ）: `build/tmp/p028-base`（基点の展開と試験の出力）、`build/tmp/p028`・`build/tmp/p028-*`（Windows の試しの build・log）、`build/tmp/p028-gui`・`build/tmp/p028-dbg`（手での試しと path の調べの program）、`build/tmp/ws334-wine-build`・`build/tmp/ws334-wine-run*`・`build/tmp/ws334-keiland-*`（script の出力）、`build/tmp/ws334-wine-gui`（Wine の GUI の prefix、次の試験でも使える）、`build/tmp/x.res.o`・`build/tmp/common-list.txt`。
