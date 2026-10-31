<!-- awesome-plan project=zedbsd record=ws334-p027 -->

# ws334-p027 統合: browser の UI の準備（browser_main、libkeiland の 3 つの口、CPU の道、鍵の text、時計、files の backend の場所）

- 親: [WS334](../ws.md)、設計 [design.md](../design.md) の §4.1・§4.5（header の追加）・§4.6・§4.8・§6.5。
- 状態: planned（2026-11-01 p002 で作成）。
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

1. `main.c`: 今の `main` の中身を `int browser_main(int argc, char **argv)`（新しい `browser/browser-main.h` に宣言）に移し、`main` はそれを呼ぶだけ。`browser/sources.mk` の一覧はそのまま（main.c が両方を持つ）。Windows の入口（p028 の `apps/browser-win32/main.c`）は `main.c` を compile せず、`browser_main` を呼ぶ。そのため `main` だけを新しい `browser/main-posix.c` に移し、`main.c` には `browser_main` を残す（sources.mk に main-posix.c を足し、Windows の Makefile は main-posix.c を除く）。
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
