<!-- awesome-plan project=zedbsd record=ws334-p037 -->

# ws334-p037 本物の Windows が要る口: TSF・DirectManipulation・題の帯の透過

- 親: [WS334](../ws.md)。p033 から分けた（2026-11-02 02:44 Q1）。
- 状態: in-progress（2026-11-14 q2095-i01、P7: 実装と Wine で確かめられる所の試験まで。本物の Windows の確かめは下の一覧。cleared は Q1）。
- 依存: p033。

## 範囲（2026-11-14 に直した）

- a: TSF（Text Services Framework）の text store。TSF が無い・始められない時は今の IMM32 の入力に戻る。
- c: DirectManipulation による精密 touchpad の scroll と、指を離した時の `KL_WINDOW_AXIS_STOP`。DirectManipulation の無い環境では今の wheel の message の scroll。
- d: 題の帯の透過（Mica の上の題の帯の DIB の alpha）。
- 本物の Windows 10・11（amd64 と arm64）での確かめは、下の「本物の Windows の確かめの一覧」に置く（Q1 が cockpit の「ユーザーが拾える試験」に）。

範囲から外した物（2026-11-14 の乖離の確かめ）:

- modal な resize・move の loop の間の描き直し（前の `kl_app_set_modal_draw` の案）: WS345 p013 の `kl_window_on_resize`（`keiland_window.h`・`libkeiland/win32/window.c` の `window_sizing`、browser の `shell/shell.c` の `shell_resize_live`）で、modal の loop の間に program の thread が描ける形が済んでいる。別の口は足さない。
- Vulkan の swapchain の alpha: 題の帯は libkeiland の DIB で描き、program の Vulkan は題の帯の下の content（子の HWND）だけに描く。content は頁の地を全部描くので Mica は見えず、Win32 の surface の composite alpha も opaque だけの driver が多い。透過は題の帯の DIB だけで足りる。

## Q1 の判断（2026-11-02 02:44）

- p033 は実装した所で cleared にし、Wine で確かめられない口をこの Phase に分けた。Wine には TSF の composition・DirectManipulation・DWM が無く、未試験の text store を入れると今動いている IMM32 の入力を壊す恐れがあるため。
- drop の受け（b の残り）はこの WS に入れない。browser の shell が drop を受けないので今は益が無く、libkeiland の全体の移植（WS234）の側の物。
- （2026-11-14 に外した）modal の間の描き直しの口 `kl_app_set_modal_draw`。上の通り `kl_window_on_resize` で済み。
- 2026-11-14 ユーザーの決定（Q1 経由）: p037 も実装する。確かめは Wine でできる所まで、本物の Windows の確かめは一覧に残す。

## 実装（2026-11-14 q2095-i01、P7）

新しい .c は作らず、今の file に足した（`apps/browser-win32/Makefile` の source の一覧を変えないため）。公開の header（`keiland_window.h` ほか）・HAL・Makefile・`mk/` は変えていない。

| file | 中身 |
| --- | --- |
| `userland/desktop/libkeiland/win32/ime.c` | TSF: 窓ごとに `ITfThreadMgr`（`CoInitializeEx` は apartment-threaded）・`ITfDocumentMgr`・context。text store は `ITextStoreACP` と `ITfContextOwnerCompositionSink` を 1 つの object の 2 つの vtable で（C）。store は input method が書いている文字だけを持つ: lock の後に、composition の前の文字（composition が無ければ全部）を `KL_WINDOW_TEXT_COMMIT`（題の帯の欄の編集中は欄へ）として渡して store から抜き（TSF に `OnTextChange`・`OnSelectionChange`）、composition を `KL_WINDOW_TEXT_PREEDIT`（cursor は選びの端、UTF-8 の byte）に。`GetTextExt` は caret の矩形（`kl_window_text_cursor`、欄の編集中は欄の箱）を screen の座標で、caret が動くと `OnLayoutChange`。text が要る間だけ frame の focus を窓の document に（`AssociateFocus`、要らない時は NULL で input method は off）。行き先（program・欄・無し）が変わった時の composition は `TerminateComposition` で終え、文字は捨てる。lock は同期で渡し、lock の中の頼みは非同期に積む（同期の頼みは `TS_E_SYNCHRONOUS`）。始められない時（COM が MTA、TSF が無い、どれかの段の失敗）は `window->tsf` が NULL のまま、IMM32 の今の道（`WM_IME_*`）。IMM32 の context の置き場所は両方の道で今の通り |
| `userland/desktop/libkeiland/win32/pointer.c` | DirectManipulation: content の HWND に viewport（`INTERACTION`・`TRANSLATION_X`・`TRANSLATION_Y`、慣性・拡大・rail 無し、`MANUALUPDATE`）。touchpad の接触（`DM_POINTERHITTEST`、`GetPointerType` が `PT_TOUCHPAD`）を `SetContact`。viewport が動く間は 8 ms の `WM_TIMER` で `Update`、content の translation の差を `KL_WINDOW_AXIS`（`axis_source` FINGER、dx・dy は content の pixel、指と逆向き）、指を離して READY（か INERTIA）になると `KL_WINDOW_AXIS_STOP` を出し、viewport を content（1000000 px 四方）の真ん中へ戻す（端に着かない）。慣性は program（shell の `kl_scroller`）が `AXIS_STOP` の後に自分で付ける（Wayland と同じ形）。始められない時は今の `WM_MOUSEWHEEL` |
| `userland/desktop/libkeiland/win32/window.c` | 窓を作る時に TSF と DirectManipulation を始め、閉じる時に止める。`keiui_win32_window_theme` で Mica（`DWMWA_SYSTEMBACKDROP_TYPE`）を受けたか・system の build が 22621（Windows 11 22H2）以上か（`RtlGetVersion`）・high contrast でないかで `window->glass` を決め、glass の時は frame を題の帯の下端まで client に広げる（`DwmExtendFrameIntoClientArea`、それ以外は今の 1 pixel）。layout ごとに広げ直す（dpi・全画面で題の帯の高さが変わる）。content の procedure で DirectManipulation の message を先に見る |
| `userland/desktop/libkeiland/win32/present.c` | glass の題の帯: 地を透明（0、premultiplied）にし、caption button の帯だけ theme の地で塗る（frame を広げると DWM が出す system の caption button を隠すため）。その上に controls と button。`kl_window_vulkan_extensions` の comment の「遅延の import」を今の形（普通の import）に直した |
| `userland/desktop/libkeiland/win32/window-win32.h` | `struct kl_window` に `tsf`・`manipulation`・`glass`、内部の関数の宣言 |
| `plan/ws334/tests/tsf-main.c`（新） | 試験の program `tsf-test.exe`: libkeiland の窓を開いて text を求め、thread の manager から窓の context を見つけ、自分の edit session で input method のように store を動かす（挿した文字の commit、text が要らない時に渡らない、composition の表示・commit・捨て、`GetTextExt` が caret の大きさ）。その TSF に無い機能（Wine の `ITfContextComposition`・`GetActiveView`）は not run |
| `plan/ws334/tests/wine-run.sh` | 項 M5t（`tsf-test.exe`）と、本物の Windows の項 M5c・M5d を足した。M7m の文を題の帯の透過に広げた |
| `plan/ws334/tests/allowed-imports.txt` | TSF・DirectManipulation・timer・high contrast の名前（cross-build の検査 3） |

設計の判断（担当）:

- store に周りの文字（`kl_window_text_context`）を入れない。入れると TSF が composition の外の範囲を書き換えた時に `KL_WINDOW_TEXT_DELETE` へ写す byte の数の計算と、program が文脈を送り直すまでの食い違いが要る。今は再変換（変換済みの文字を選んで変換し直す）を出さない。必要になれば別の Phase で。
- pinch（拡大）は DirectManipulation に持たせない。browser の shell に Ctrl と wheel の拡大の扱いが無く（`shell/window.c` の `window_axis`）、今の益が無い。
- 横だけの指の scroll は shell が wheel の道で扱う（`window_axis` は FINGER の dy だけを touchpad の道に。Wayland と同じ）。

## 結果（Wine と cross-build）

| コマンド | 結果 |
| --- | --- |
| `timeout 3000 make -j4 browser-win32`（素の top、gcc の mingw-w64、x86_64） | rc 0、`warning:` 0 |
| `WIN32_ARCH="x86_64 aarch64" sh plan/ws334/tests/cross-build.sh`（llvm-mingw） | `cross-build: PASS`。両 arch の compile の warning 0、libkeiland.a・libbrowser-ui.a の未定義の名前は表の中（x86_64 228・aarch64 224、1 回目は TSF・DirectManipulation の名前が表に無く FAIL → allowed-imports.txt に足した）、libkeiland.a が公開の関数 577 を定義、OmniGrammaton.exe の import は system の DLL と vulkan-1.dll（25）、winpthreads の名前無し、zip 18 file。aarch64 は link まで（arm64 の Wine は無い） |
| `sh plan/ws334/tests/wine-run.sh`（Wine 10.0、lavapipe） | `wine-run: 21 passed, 0 failed, 8 not run`、`wine-run: PASS`。今までの項（M8・M1・M2a・second・resize・M3a〜c・M6・M6p・M4・clipboard・M5・caption の 3 項・M7・M7p・M7t・dpi）は全部 PASS のまま（TSF が始まった窓で英字の入力・clipboard が今まで通り）。新しい M5t: `tsf-test: PASS (5 checks, 2 not run)` |
| `tsf-test.exe` を Wine で単独に | Wine の msctf で TSF が始まる（`ThreadMgr_Activate`・`CreateDocumentMgr`・`Push`・`AssociateFocus`）。edit session の `InsertTextAtSelection` が store に届き、`KL_WINDOW_TEXT_COMMIT` に「あい」、text が要らない時は何も渡らない。Wine の context は `ITfContextComposition` が無く（`Context_QueryInterface unsupported interface`）、`GetActiveView` は `E_NOTIMPL`（0x80004001）なので、composition と `GetTextExt` の項は not run |
| DirectManipulation の Wine での振る舞い（`WINEDEBUG=fixme+all`） | Wine の `directmanipulation` は `GetPrimaryContent` で `IDirectManipulationContent` を出さないので開く途中で止め、今の wheel の道に戻る（`viewport_Abandon` まで呼んで片付け）。Wine では DirectManipulation の道は通らない |
| 題の帯の glass | Wine は Windows 10 の build（19045）を名乗るので glass にならず、今の不透明の題の帯（M7・dpi・caption の PNG が今まで通り）。glass の道は本物の Windows 11 22H2 だけ |

## 本物の Windows の確かめの一覧（Q1 が cockpit の「ユーザーが拾える試験」に）

担当にはできない（Wine に TSF の composition・DirectManipulation・DWM が無い）。Windows 10 と 11、amd64 と arm64（`WIN32_ARCH=aarch64` の zip）で:

p037 の新しい口:

1. TSF・日本語（MS-IME）: location の欄と頁の text の欄で、composition が欄の中に出る、候補の窓が caret の下に出て caret を隠さない、確定で文字が入る、Esc で composition が消える。Google 日本語入力でも同じ。
2. TSF: 頁の text の欄から外を押す・欄の編集を Esc で止める時、composition の文字が入らずに消える。
3. TSF: 英語の配列（IME が off）で打つ文字・Backspace・矢印・Ctrl+C／V が今まで通り。絵文字の panel（Win+.）の文字が入る。touch keyboard の文字が入る。
4. DirectManipulation: 精密 touchpad の 2 本指の縦の scroll が指に付いて動き、離すと慣性で流れて止まる。流れている間に指を置くと止まる。横の scroll（横に長い頁）。2 本指の pinch が何もしない（前も拡大は無い）。
5. DirectManipulation の無い／精密でない touchpad・mouse の wheel が今まで通り。
6. Mica（Windows 11 22H2 以降）: 題の帯の地に Mica が透け、location の欄・button・題が読める（明るい mode と暗い mode）。題の帯の右の caption button の帯が system の button と二重に見えない、最小化・最大化・閉じるが押せる、最大化の button の上で snap の配置が出る。窓が inactive の時の題の帯。
7. Windows 10・high contrast: 題の帯が今まで通り不透明。
8. dpi を変える（150%・200%、別の dpi の monitor へ移す）・全画面の出入りで、題の帯の透過の高さが合う。

p032 の M1〜M8（design §7.3）で Wine の結果が参考だった物:

9. M1 Vulkan（実の GPU の driver）で start.html を描く。M2 `--no-gpu` の CPU の道。
10. M3 https: 公開の site を Windows の根の store で開く（M3d）。Disallowed の store の拒み。
11. M4 location の欄（英字・日本語の IME、M4j）、clipboard。M5 頁の text の欄（英字・IME の composition、M5j）。
12. M6 閉じて開き直した後の cookie と localStorage。
13. M7 snap の配置（M7s）、題の帯の drag の Aero Snap と右 click の system の menu（caption-snap）、暗い mode。
14. M8 は Wine で host と同じ bytes（実機でも同じはず。省いてよい）。

p033 の未試験の項:

15. 暗い mode・accent（設定の色の変更で題の帯と頁が追従）と Mica（Windows 11 22H2）、Windows 10 で Mica の属性が無害なこと。
16. touch screen と pen（ブラウザでは pen は mouse、touch は shell の touch.c の scroll と tap）。
17. 頁の画像を Explorer・Word・Paint に drag して PNG が渡る、文字が渡る。
18. DPAPI: 別の利用者・別の機械では cookie が読めず（ログアウトの状態になる）、同じ利用者では残る。
19. winarm64 の .exe（Windows 11 arm64）で上の全部（arm64 の Wine は host に無く、link まで）。

## 受け入れ

- `make browser-win32`（gcc、x86_64）warning 0。`WIN32_ARCH="x86_64 aarch64" sh plan/ws334/tests/cross-build.sh` PASS。
- `sh plan/ws334/tests/wine-run.sh` PASS（今までの項と M5t）。
- 本物の Windows の確かめは上の一覧（未実施）。
