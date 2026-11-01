<!-- awesome-plan project=zedbsd record=ws334-p027 -->

# ws334-p027 統合: browser の UI の準備（browser_main、libkeiland の 3 つの口、CPU の道、鍵の text、時計、files の backend の場所）

- 親: [WS334](../ws.md)、設計 [design.md](../design.md) の §4.1・§4.5（header の追加）・§4.6・§4.8・§6.5。
- 状態: in-progress（実装済み・試験 PASS、cleared は Q1。2026-11-01 q1856-i01、Sonnet。結果は末尾）。前: planned（2026-11-01 p002 で作成）。
- 依存: p023。
- 担当の想定: Opus（phase-runner）を 1 回。zedBSD の browser の挙動を変えない（`--no-gpu` を足すだけ）。

## 調整（始める前）

- `userland/desktop/browser/`（WS074 の物、重なり中）と `plan/ws074/tests/host-build.sh`・`host-shell.c`（高）を触ることを Q1 に送る。libkeiland の公開の header は WS074 の外だが他の WS（Keiland の app）も使うので、KL_VERSION を上げることも Q1 に。

## libkeiland の公開の header（KL_VERSION 104 → 105、加えるだけ。今の main の版を確かめ、上がっていれば +1）

- `include/keiland/keiland_version.h`: `KL_VERSION 105U` と、版の一覧の comment に 1 行（「KL_VERSION 105: kl_window_vulkan_extensions, kl_app_storage_directory, kl_data_directory; a key event's text」）。
- `include/keiland/keiland_window.h`（`kl_window_vulkan_surface` の近く、`VK_VERSION_1_0` の条件の外）:
  ```c
  /*
   * The instance extensions a Vulkan instance needs for kl_window_vulkan_surface
   * on this system (KL_VERSION 105): VK_KHR_surface and the system's surface
   * extension.  names receives a static array of count names.  Returns 0, or
   * EINVAL for NULL arguments.
   */
  int kl_window_vulkan_extensions(const char *const **names, uint32_t *count);
  ```
  Wayland の実装（`libkeiland/ui/window.c` か present の file）は `VK_KHR_surface`・`VK_KHR_wayland_surface`。`kl_window_vulkan_surface` の comment の「(which enabled VK_KHR_wayland_surface)」を「(which enabled the extensions kl_window_vulkan_extensions names)」に。
- `struct kl_window_event` の `text` の comment に: 「KEY の押下（pressed 1）の text は、その鍵が打つ文字の UTF-8（backend が分かる時。分からない backend は空）（KL_VERSION 105）」。Wayland の backend は今のまま空（変えない）。
- `include/keiland/keiland_app.h`:
  ```c
  /*
   * Writes the directory where the application keeps its data (KL_VERSION 105):
   * Wayland systems use $XDG_DATA_HOME/keiland/<application> (with
   * $HOME/.local/share when XDG_DATA_HOME is unset or not absolute); the
   * directory is not created.  application is the kl_app_options application
   * the app was opened with.  Returns 0, EINVAL (NULL arguments, size 0, no
   * application name), ENOENT (neither variable set), ERANGE (buffer too small;
   * it then holds the empty string).
   */
  int kl_app_storage_directory(const struct kl_app *app, char *buffer, size_t size);

  /*
   * Writes the directory of the bundled read-only data (KL_VERSION 105):
   * KEILAND_DATADIR ("/usr/share") on the Wayland systems.  Returns 0, EINVAL,
   * ERANGE.
   */
  int kl_data_directory(char *buffer, size_t size);
  ```
  実装は `libkeiland/ui/app.c`。`libkeiland/exports.map`・`exports.py`（export の一覧の生成があれば）に 3 つを足す。
- Linux の Keiland（`Makefile.linux`）と FreeBSD でも同じ実装が build される。

## browser の UI（`userland/desktop/browser/`）

1. `main.c`: 今の `main` の中身を `int browser_main(int argc, char **argv)`（新しい `browser/browser-main.h` に宣言）にし、`main.c` に残す。`main` 自身は新しい `browser/main-posix.c`（`browser_main` を呼ぶだけ）へ移す。`browser/sources.mk` の `KEILAND_BROWSER_SOURCES` に `main-posix.c` を足す（zedBSD・Linux の build）。Windows の Makefile（p028）は `main-posix.c` を除き、`apps/browser-win32/main.c` の入口から `browser_main` を呼ぶ。
1b. `shell/shell.c:782`（設計の時の行。typed の location が絶対の path かの `typed[0] != '/'`）: libkeiland の側に path の判定は無いので、`browser/shell/` に static の `shell_path_is_absolute`（POSIX は '/' で始まる、Windows の形（drive の文字・':'・区切り、または 2 つの区切り）も同じ関数で見る。system の `#ifdef` は書かず、両方の形を常に path と見る: zedBSD で "C:\\x" を打つ人はいないので害が無い）を書いて使い、続く `"file://"` の組み立ては `browser/` の今の file: URL の作り方のまま（Windows の drive の形は "file:///C:/x" に: '\\' を '/' に替え、先頭に '/' を足す）。
2. `shell/present.c`: 拡張の名前（:64-65）を `kl_window_vulkan_extensions` から。`pApplicationName`（:68）を `SHELL_PRODUCT_NAME`。
3. **CPU の道**（新 `shell/present-cpu.c`、`browser/sources.mk` に足す）: `--no-gpu` の引数、または Vulkan の instance・surface・device の作りが失敗した時、窓を `KL_PRESENT_SHM` で作り（失敗が窓の後に分かった時は窓を閉じて SHM で作り直す）、`browser_view_draw_pixels` で描いて `kl_window_present` で出す。大きさの変化は `kl_window_present_resize`。描く時は今の Vulkan の道の redraw の契機と同じ（view の redraw の callback と大きさの変化）。`--no-gpu` は `main.c` の引数の表に足す（usage の文も）。zedBSD でも使える。
4. `shell/keys.c`: event の `text` が空でなく、1 つの印字できる文字（制御の文字でない）の時はそれを DOM の `key` に、それ以外は今の US の表（:9-13・:27-39）。
5. `shell/window.c:210`・`shell/touch.c:369`: `clock_gettime(CLOCK_MONOTONIC)` を `kl_clock_us()`（`keiland_window.h:348`、単位の換算を合わせる）。
6. **名前の定数**（design §6.5）: `browser/shell/shell.h`（今の shell の header）に `#define SHELL_PRODUCT_NAME "OmniGrammaton"`、`#define SHELL_APPLICATION_ID "browser"`（Windows の Makefile が `-DSHELL_APPLICATION_ID=\"OmniGrammaton\"` で替える。`#ifndef` で包む）。`window.c:78` の `app_options.application = "browser"` を `SHELL_APPLICATION_ID`。窓の題は今のまま（Windows の「page の題 - OmniGrammaton」は p028 で libkeiland の Win32 の側が付けない。shell の題の作りを `<題> - SHELL_PRODUCT_NAME` に変えるかは zedBSD の見た目を変えるので変えない）。
7. **files の backend の場所と font**: `shell/shell.c` で p023 の XDG の求め方を `kl_app_storage_directory`（→ `wb_os_make_directories` は libbrowser の private なので、shell は `browser_storage_files_open` が親ごと作るのに任せる）。`browser_fonts` は NULL のまま（libbrowser の既定が `wb_os_data_directory`）。
8. `picture/png-write.c`（`--render` の PNG）は変えない。

## WS074 の host の build

- `plan/ws074/tests/host-build.sh`・`host-shell.c`: `browser/main.c` を `host-shell.c` と compile する形が `browser_main` と `main-posix.c` の分けで壊れないように直す（`main-posix.c` を compile に入れるか、`host-shell.c` が `main` を持つ）。WS074 の file なので差分を Q1 に見せてから。

## 試験（受け入れ）

- zedBSD amd64 の `libkeiland.so`・`bin/browser` と、Keiland の他の app（`make -pn` で libkeiland に依る program の target を 3 つ: files・terminal・settings）の build が warning 0。
- Linux の Keiland（`make keiland-linux`）が build できる。
- `host-build.sh plain`・`asan` の件数が前と同じ（headless の試験は CPU の道で、`--no-gpu` を使わない物は同じ）。
- libkeiland の host の試験（`plan/tools.md` の Keiland の項）の件数が同じ。
- zedBSD の guest の browser の確かめ（Vulkan の道と `--no-gpu` の道の 2 枚の画面の PNG、localStorage が残る）は Q1 の割当（design §7.1 段 5、`plan/ws074/tests/browser-guest.sh`）。
- `cross-build.sh` PASS（この Phase では Windows の browser の UI は compile しない。p028 で）。`git diff --check`。

## 変えてはいけない物と終わり

- module の header と .c、試験の道具。libbrowser（p023 で済み）。compositor・libkeiland-backend。HAL・toolchain・共有の build/。rm をしない。push しない。
- file の群ごとの commit、SHA と結果を Q1 に、結果の節と ws.md の行。

## 結果（2026-11-01、Sonnet、q1856-i01、基点 3c076f4e8）

commit（`git commit -m WIP -- <path>`、file の群ごと）:

| SHA | 中身 |
| --- | --- |
| 2b501fe030 | libkeiland: `keiland_version.h`（KL_VERSION 105 と版の一覧の 1 行）、`keiland_window.h`（`kl_window_vulkan_extensions`、`kl_window_vulkan_surface` と key の `text` の comment）、`keiland_app.h`（`kl_app_storage_directory`・`kl_data_directory`）、`ui/window-declare.c`・`ui/app.c`（Wayland の実装）、`exports.map`（`exports.py` で生成、`--check` OK） |
| cb2a7ae598 | browser の UI: `main.c`（`browser_main`、`--no-gpu`）、新 `browser-main.h`・`main-posix.c`、`sources.mk`、`shell/`（新 `present-cpu.c`、`present.c`・`keys.c`・`window.c`・`touch.c`・`shell.c`・`shell.h`・`internal.h`）、`plan/ws074/tests/host-build.sh`（2 行。差分の要点は Q1 に送って了解済み） |

試験（実機・QEMU は使っていない。host と zedBSD の amd64 の build）:

- zedBSD amd64（`make ZEDBSD_CONFIG=config/ci/config-amd64.mk ZEDBSD_TARGET_LLVM_BIN=/home/awe/zedBSD-claude1/build/llvm/bin ZEDBSD_SYSROOT_AMD64=<写し> -o <写し>/.zedbsd-sysroot-complete -j8 build/amd64/dynamic/libkeiland.so build/amd64/bin/browser build/amd64/bin/files build/amd64/bin/terminal build/amd64/bin/settings`、写しは main の `build/amd64/sysroot` を `build/tmp/p027/sysroot-amd64` に写し `include/libc`・`userland/desktop/include` を重ねた物）→ exit 0、warning 0。`llvm-nm -D` で libkeiland.so に `kl_window_vulkan_extensions`・`kl_app_storage_directory`・`kl_data_directory` が export されている。
- Linux の Keiland（`make -f userland/desktop/keiland-linux.mk all -j6 KEILAND_LINUX_BUILD=build/tmp/p027/keiland-linux`、`make keiland-linux` の `all` の部分。install の確認は流していない）→ exit 0、warning 0（-Werror）。
- `sh plan/ws074/tests/host-build.sh plain`・`asan` → 両方 exit 0、warning 0、`built` 94 本（前の 94 と同じ）。plain の browser に `--version`・`--help`（`--no-gpu` が usage に出る）と `--render`（font 指定、exit 0）。`run-js-tests.py` 17/17。
- `sh plan/tools/keiland-layers/host-test.sh build/tmp/p027/keiland-linux`（libkeiland の host 試験）→ SUMMARY pass=12 fail=0（thread 28 checks ×5、socket 53 checks ×6、全部 PASS）。
- 使い捨ての試験（commit しない）: `shell_key_names` を host で 15 case（US の表、JIS の `@`、Ctrl で制御文字の text、Enter の CR、UTF-8 の 2 byte、2 文字・C1・不正な UTF-8・Alt・未知の key・空白・DEL）→ 全部 期待どおり。
- `timeout 3600 flock /tmp/zedbsd-wine.lock sh plan/ws334/tests/cross-build.sh` → `cross-build: PASS`（exit 0。この Phase では Windows の browser の UI は compile しない）。
- `git diff --check` → 出力なし。

自分で決めた点・phase.md と code の食い違い:

- **application を window より先に開く**（手順 7）: `kl_app_storage_directory` が `kl_app` を取り、instance は view より前、view の題が window の題に要る、という順の制約のため、`shell_window_open` を `shell_window_connect`（`kl_app_open`）と `shell_window_open`（窓。present の方式の引数 `KL_PRESENT_NONE`/`SHM` を足した）に分け、`shell_run` は connect → instance → view → 頁の読み込み → window の順にした。窓を開けない時の error の行は同じ（"cannot open a window"）だが、compositor に繋がらない時は頁の読み込みより前に終わる。
- **Vulkan が窓の後で失敗した時**（手順 3）: `shell_surface_open` が Vulkan の presenter を close し、titlebar を close し、`shell_window_drop`（窓だけ閉じ application は残す）、窓を `KL_PRESENT_SHM` で作り直して CPU の presenter を開く（stderr に "browser: cannot draw with the GPU: <操作> failed (<code>): drawing with the CPU" の 1 行）。mail と view の wake は application に付くので再設定しない。SHM の `kl_window_present` が EAGAIN（buffer が compositor 側）の時は `shell_frame` が 2 を返し、main loop が dirty を残して 8 ms（`SHELL_RETRY_MS`）以内に再び描く。
- **`internal.h` の `VK_USE_PLATFORM_WAYLAND_KHR` の define を外した**: 拡張の名前を `kl_window_vulkan_extensions` から取るので shell は Wayland の Vulkan の型を使わない（Windows の p028 で platform の define を替えなくてよい）。libkeiland 側の `window-declare.c` は従来どおり自分で使う。
- **`shell_key_names` の引数に `typed`（event の text）を足し、`struct shell_key_names` に `typed[]` を足した**（手順 4）: 印字できる 1 文字（well-formed な UTF-8、C0・DEL・C1 でない）なら DOM の key と text（Ctrl・Alt・Meta が無い時）にし、code は表のまま。それ以外は今の US の表。`window.c` の `window_key` が押下の時だけ `input->text` を `shell_event.text` に写す。Wayland の backend の text は空なので zedBSD の挙動は変わらない。
- **`kl_app_storage_directory`**: phase.md の文面どおり、`XDG_DATA_HOME` は絶対の path の時だけ使う（今までの shell は空でなければ使った。相対の値の時だけ差が出る）。`kl_data_directory` は `KEILAND_DATADIR`（`userland/desktop/paths.h`）。
- **`SHELL_PRODUCT_NAME` は `shell.h` に無条件、`SHELL_APPLICATION_ID` は `#ifndef` で**。窓の題・mail の名前（`mail.c` の "browser"）は変えていない。
- 手順 1b: `shell_path_is_absolute`（'/' 始まり、drive の文字＋':'＋区切り、先頭が '\\' で次が '\\' か '/'）。`file:` の URL は '/' 始まりは今のまま `file://<typed>`、drive は `file:///C:/x`（'\\' を '/' に）、'\\' 始まりは `file:` + typed（'\\' を '/' に、`file://host/x`）。
- 手順 8（`png-write.c`）・libbrowser・compositor・libkeiland-backend・HAL・toolchain は触っていない。`browser_fonts` は従来どおり（main.c の `--font=` のみ）。

未実施:

- zedBSD の guest の browser の確かめ（Vulkan の道と `--no-gpu` の道の 2 枚の画面の PNG、localStorage が残る、JIS でない US の鍵の入力）は Q1 の割当（design §7.1 段 5）。CPU の道（`present-cpu.c`）と窓の作り直し、`kl_app_storage_directory`・`kl_window_vulkan_extensions` の実行は、compositor の無い host では動かせず、build と読みだけ。
- `plan/ws334/tests/check-skeleton.sh` は 100 秒で打ち切り（p023 の記録のとおり基点でも FAIL する物。この Phase と無関係）。

消してよい物（Q1 へ）: `build/tmp/p027`（sysroot の写し・build の log・Linux Keiland の出力 `build/tmp/p027/keiland-linux`）、`build/ws074-host`、`build/amd64`、`build/tmp/ws327-host-test.run.*`。どれも使い捨て。
