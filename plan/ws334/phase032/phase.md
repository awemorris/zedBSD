<!-- awesome-plan project=zedbsd record=ws334-p032 -->

# ws334-p032 試験: Wine＋llvmpipe での確かめの第 1 回

- 親: [WS334](../ws.md)、設計 [design.md](../design.md) の §7.1・§7.3（最小の合格の組 M1〜M8）、決定 20（host の Wine と lavapipe で開発する。実機の Windows はだいぶ後）。
- 状態: in-progress（実装済み・試験 PASS、cleared は Q1。2026-11-02 q1874-i01、P5。2026-11-01 p003 で作成）。
- **2026-11-01 の変更（DLL を作らず 1 つの .exe、libkeiland の Windows は pthread 無し。ws.md のユーザーの決定、design §6.2・§6.6、build の形は p036）**: `omnigrammaton-cli.exe` は作らない（2026-11-01 ユーザーの答え: 配るのは `OmniGrammaton.exe` だけ）。本文の `omnigrammaton-cli.exe` は試験の program（`plan/ws334/tests` の配らない .exe、p036 の `test-program` で同じ static library を link）に読み替える。
- 依存: p031。
- 担当の想定: Opus（phase-runner）を 1 回（約 8 LW）。直しは見つけた不具合ごとに小さく、元の Phase の範囲（その file）の中で。範囲の外の直しが要れば Q1 に。

## 中身

- `plan/ws334/tests/wine-run.sh`（p028 から育てた物）を 1 本の受け入れの script にする: zip を展開 → xvfb-run の中で Wine（prefix `build/tmp/ws334-wine-gui`、lavapipe）→ 各項の操作（xdotool）と PNG・log の回収 → 要約（項ごとに PASS・FAIL・未実施、最後の行 `wine-run: PASS` か `FAIL`）。
- 項（design §7.3 の M1〜M8 を Wine でできる形に。できない物は「未実施（実機）」）:
  - M1 `OmniGrammaton.exe` が同梱の `start.html` を Vulkan（lavapipe）で描く PNG。
  - M2 `--no-gpu` と、`WINEDLLOVERRIDES=vulkan-1=d` で CPU の道に落ちて描く PNG。
  - M3 https: host の `openssl s_server` の試験の CA の頁（p030 の方法）。外の site は使わない。Disallowed の拒み。
  - M4 location の欄に英字を打って移る（日本語の IME は未実施（実機））。
  - M5 page の text の欄に英字を打つ（IME の composition は未実施）。
  - M6 再起動の後に期限付きの cookie と localStorage が残る（試験の頁が値を表示し PNG で読む。文字を読むのは Q1 かユーザー。script は頁の title を `--dump` の道か log で確かめる）。
  - M7 snap の配置・暗い mode の題の帯: Wine は snap の配置を出さない（未実施（実機））。暗い mode は Wine の registry の値で PNG。
  - M8 `omnigrammaton-cli.exe --js`・`--dump=layout` が Linux の host と同じ bytes（数本の頁と js）。setjmp と GC（R4）の js。
  - 2 つ目の起動が URL を 1 つ目に渡す。窓の大きさの変更（xdotool）で描き直る。
- PNG は `build/review/ws334-p032/` に写し、一覧を Q1 に（ユーザーに見せるのは Q1）。

## 受け入れ

- `wine-run.sh` の最後の行が `wine-run: PASS`（Wine でできる項が全部 PASS、できない項は「未実施（実機）」として数えない）。Wine の結果は実機の Windows の判定ではない（決定 20）と結果に書く。
- 実機の Windows（またはユーザーの決定 19 の QEMU の Windows の guest）での M1〜M8 の確かめは、ユーザーが拾える試験の一覧に置く（Q1 が）。

## 終わり

- 直した file ごとの commit、SHA・PNG・項ごとの結果を Q1 に、結果の節と ws.md の行。

## 結果（2026-11-02 01:40、P5、q1874-i01、基点 bcb7124a6）

Wine の結果は参考で、実機の Windows の判定ではない（決定 20）。

commit（`git commit -m WIP -- <path>`）:

| SHA | 中身 |
| --- | --- |
| 235c52d60 | `libkeiland/win32/present.c`: `kl_window_vulkan_extensions` が system の Vulkan の loader（`vulkan-1.dll`、`GetModuleHandleW` か `LoadLibraryExW(LOAD_LIBRARY_SEARCH_SYSTEM32)`）を確かめ、無ければ `EIO`。公開の `include/keiland/keiland_window.h` の comment に「EIO: Vulkan の loader の無い system（vulkan-1.dll の無い Windows）」を足した（Q1 が認めた、KL_VERSION は上げない） |
| 0a39602da | `plan/ws334/tests/`: `wine-run.sh`（zip の展開からの 1 本の受け入れ）、新しい `cert-store-main.c`（試験の program: 今の利用者の system の store に証明書を足す・抜く）、`js/gc-parse.js`（parser の setjmp・longjmp と heap の回収）、`wine-pages/input.html`（M5 の text の欄） |

`timeout 5400 sh plan/ws334/tests/wine-run.sh` の 4 回目の要約（`wine-run: 15 passed, 0 failed, 4 not run (a real Windows)`、最後の行 `wine-run: PASS`）:

| 項 | 結果 |
| --- | --- |
| 0 | `make -f apps/browser-win32/Makefile all zip`・試験の program 2 本（browser-cli・cert-store）が warning 0、host の browser。zip（9,883,259 bytes）を新しい directory に展開し、窓は全部そこから起動 |
| M8 | PASS: `--js` の 18 本（`plan/ws074/tests/js` の 17 本と `gc-parse.js`）と `--dump=layout` の 3 頁（start・about・text）が host と Wine で同じ bytes（21/21）。gc-parse は Wine で `list 2000 1999000`・`syntax errors 201`（eval・Function の構文の誤り 200 回と深い入れ子 1 回、間に約 6 万の object） |
| M1 | PASS: Vulkan（lavapipe）で start.html（1490 色） |
| M2a | PASS: `--no-gpu` で start.html（1473 色） |
| M2b | PASS（直した後）: `WINEDLLOVERRIDES=vulkan-1=d` で `browser: cannot draw with the GPU: kl_window_vulkan_extensions failed (-3): drawing with the CPU` と出して CPU の道で描く |
| 2 つ目の起動 | PASS: about.html を渡して exit 0、1 つ目が `ZBROWSER OPEN handed=...about.html` |
| 大きさ | PASS: 右の縁（frame の hit test の 8 px の中）を xdotool で 126 px 左へ drag すると幅 898 の frame、最大化の button で 1280×768 の frame |
| M3a | PASS: CA を誰も信じない時は `tls=handshake: certificate verify failed: unable to get local issuer certificate` で拒み、失敗の頁 |
| M3b | PASS: CA を今の利用者の ROOT の store に入れると https の頁が開く（`ZBROWSER NAVIGATE`・頁の console の行） |
| M3c | PASS: 同じ CA を Disallowed の store にも入れると `certificate rejected` で拒む（試験の後に両方の store から抜く） |
| M3d | 未実施（実機）: 公開の site（外の network は使わない） |
| M4 | PASS: location の箱に about.html を打って Enter で移る、Back で戻る、箱の file: URL を打ち直して開く |
| clipboard | PASS: Ctrl+L・Ctrl+C で `file:///Z:/.../share/browser/about.html` |
| M4j | 未実施（実機）: MS-IME の日本語・候補の窓 |
| M5 | PASS: 頁の text の欄を押して `hello wine` を打つと input の event が 10 回、最後の値が `hello wine`（頁が console に出す） |
| M5j | 未実施（実機）: IME の composition |
| M6 | PASS: `https://127.0.0.1:18444/set.html`（`Set-Cookie: kept=<時刻>; Max-Age=86400` と期限無しの `session=<時刻>`、JS が localStorage に書く）→ 閉じるの button で exit 0 → 新しい起動で check.html が `cookie=kept=<時刻> ls=<時刻>`（期限無しの cookie は消えた）。値は毎回の時刻なので前の回の残りでは通らない |
| M7 | PASS: `AppsUseLightTheme` 0 の prefix で題の帯の地の明るさ 46（明るい mode 253） |
| M7s | 未実施（実機）: snap の配置（Wine は出さない） |
| dpi | PASS: LogPixels 144 で題の帯 48 px |

他の確かめ:

| 確かめ | 結果 |
| --- | --- |
| `timeout 5400 flock /tmp/zedbsd-wine.lock sh plan/ws334/tests/cross-build.sh` | `cross-build: PASS`（両 arch の OmniGrammaton.exe warning 0、import は system の DLL 24 と遅延の vulkan-1.dll、libkeiland.a と libbrowser-ui.a の import 189（win64）・185（winarm64）が全部表の中） |
| Linux の Keiland（`make -f userland/desktop/keiland-linux.mk all KEILAND_LINUX_BUILD=build/tmp/p032/keiland-linux`） | exit 0、warning 0 |
| zedBSD amd64（`make ZEDBSD_CONFIG=plan/ws334/tests/config-amd64-p027-guest.mk BUILD=build/p032 build/p032/dynamic/libkeiland.so build/p032/bin/browser`） | exit 0、warning 0 |
| zedBSD・Linux の `kl_window_vulkan_extensions` | 実装は `libkeiland/ui/window-declare.c` で、この Phase は変えていない（loader の有る system では前と同じく 0 を返す。直しは Windows の `win32/present.c` だけ）。公開の header は comment だけ |
| `BASE=d393d3267 sh plan/ws334/tests/check-skeleton.sh` | FAIL（`libkeiland/platform/platform.h` だけ、基点からの物で p035） |
| `git diff --check` | 出力なし |
| PNG（worktree の中、`build/review/ws334-p032/`） | `m1-gpu.png`・`m2-nogpu.png`・`m2-no-vulkan.png`・`second.png`・`resized.png`・`maximized.png`・`m3-untrusted.png`・`m3-https.png`・`m3-disallowed.png`・`m4-typing.png`・`m4-typed.png`・`m4-back.png`・`m4-retyped.png`・`titlebar.png`・`m5-input.png`・`m6-after-restart.png`・`m7-dark.png`・`titlebar-dark.png`・`dpi144.png`・`titlebar-144dpi.png` |

見つけて直した不具合:

1. **vulkan-1.dll の無い機械で OmniGrammaton.exe が落ちる**（1 回目の M2b）: shell の `vkCreateInstance`（遅延の import）が loader を開けず、遅延の import の例外 `0xc06d007e` で process が終わり、CPU の道に落ちなかった。design §6.2 の「最初の `vk*` の前に libkeiland（`kl_window_vulkan_extensions` が拡張を返さない）が loader を確かめる」が実装に無かった。`win32/present.c` で確かめて `EIO`（shell は前から非 0 を CPU の道に落とす）。system32 から開いた loader は開いたまま残るので、後の遅延の import もこの loader を使う。

script の直し（1〜3 回目の FAIL のうち program の不具合でない物）:

- Wine は同じ class の 1×1 の隠れた窓を作るので、窓は class の中の一番大きい物を選ぶ（1 回目は大きさの変更と閉じるの button が外れた）。
- Wine は window manager の無い時、外から変えた X の窓の大きさ（`xdotool windowsize`）を program に伝えない（X の窓だけ切れて WM_SIZE が来ない）。大きさの確かめは program 自身の size の loop（右の縁の drag）と最大化の button にした。
- 3 回目は M4 で打ち直した URL の 1 字が二重になった（`worrktrees`、同じ時に host で zedBSD の build を流していた）。xdotool の押しと離しの間が開くと Xvfb の key の repeat が入るので、Xvfb の中で `xset r off`。
- 新しい worktree で prefix を作る時の Wine の出力が M8 の比較に混ざる件（q1872 の 1 回目）: 4 つの prefix を最初に `wineboot -i` で作ってから比べる。
- 結果の行を `printf` で（dash の `echo` が Windows の path の `\` を escape と読んでいた）。

気づいた事（この Phase の外、Q1 へ）:

- 失敗の頁と `ZBROWSER ERROR load` の行が Windows で `Unknown error: handshake: ...` と出る（`m3-disallowed.png`）。libbrowser が TLS の失敗に使う errno（mingw の `EPROTO` 系の番号）を UCRT の `strerror` が知らない。browser の shell（`shell.c` の `strerror`）か libbrowser の OS の層で文を用意するか、Q1 の判断。
- libbrowser の `--render-gpu`（`paint/vulkan.c` の offscreen）は loader の確かめをせずに `vkCreateInstance` を呼ぶ（design §6.2 の「libbrowser は `wb_os_library_open` で確かめる」）。窓の道では使わないので今回の項には出ないが、vulkan-1.dll の無い機械で試験の program の `--render-gpu` は同じ例外で落ちるはず（未確かめ）。p034 で扱うか。
- 頁の中身は暗い mode でも明るい（`m7-dark.png`、題の帯だけ暗い）。`prefers-color-scheme` を頁に渡すかは browser の判断（Wayland でも同じなら仕様）。
- s-ws334-strip の担当が zip-wine.sh を外の `flock /tmp/zedbsd-wine.lock` の中で流し、中の flock が自分を待って止まっていた（Q1 に送った）。wine-run.sh の頭に「script ごと lock を取らない」と書いた。

未実施:

- 実機の Windows（x86_64・arm64）と QEMU の Windows の guest での M1〜M8（ユーザーが拾える試験の一覧に置くのは Q1）。特に M3d（公開の site と Windows の根）、M4j・M5j（IME）、M7s（snap の配置）、DPI の違う monitor の間の移動、本物の DWM での暗い題の帯と影。
- winarm64 の .exe の実行。
- video（Vulkan Video・software）は p032 の項に無い。

消してよい物（Q1 へ）: `build/tmp/p032`（試しの script・log・exp の出力）、`build/p032`（zedBSD amd64 の試しの build）、`build/tmp/ws334-wine-run.run.*` のうち `build/tmp/ws334-wine-run` が指していない物。prefix `build/tmp/ws334-wine-dark` は次の wine-run でも使う。

