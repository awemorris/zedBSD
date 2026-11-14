<!-- awesome-plan project=zedbsd record=ws353-p017 -->

# ws353-p017: 規約の全文の見直し

Status: in-progress（2026-11-15 P1 q2113。実装済み・試験 PASS、cleared は Q1）
Parent: [WS353](../ws.md)。

## 目的

WS353 が変えた C の code（kernel の gpu・driver、libvulkan、compositor、backend）を [coding-style.md](../../coding-style.md) の全文と照らし、書き方だけを直す（動作は変えない）。source の comment・log に開発の管理情報（WS・Phase の番号など）が無い事も確かめる。

## 確かめ

- build warning 0、変えた所の host 試験の再実行。

## 依存

- p016。

## 結果（2026-11-15 P1 q2113-i01、branch `agent/p1-ws353p017`）

### 対象の範囲

WS352・WS353 の基点（`6aff283baa` の親）から main 5202274642 までの差分のうち、次の file の WS が足した・変えた行（`git diff -U0` の追加行）に絞った（153 file）。
他の WS の変更（sched・clock・libkeiland/win32・libbrowser・libmedia など）と既存の違反は直さない。

- compositor: `userland/desktop/wayland/` 全部（main・threads・render-thread・cursor-thread・output-switch・compose・shell・objects・shm ほか）
- libvulkan: `userland/desktop/libvulkan/`（sync・wsi-swapchain・wsi-display-control・wsi-display-nodes・objects ほか）、`include/libc/vulkan/vulkan_external.h`
- backend: `userland/desktop/libkeiland-backend/`、`libkeiland-backend-zedbsd/`（events・input・network・daemon-link・bluetooth ほか）、`-linux`・`-freebsd` の seat
- sessiond: `userland/desktop/sessiond/`
- kernel: `src/drivers/gpu/`（gpu.c・gpu-fence.c・各 driver の display・present・hpd）、`include/drivers/gpu/`、`include/uapi/gpu-display.h`、`src/kern/poll.c`・`filedesc.c`・`pipe.c`・`net/`・`system-event.c`、`src/drivers/generic/input.c`・`include/kern/poll.h`

### 見直しの方法

1. `plan/tools/style-check.py` を対象の file に走らせ、追加行だけに絞った（`git diff -U0` の追加行）。前: 59 件（blank-after-brace 20・paragraph-comment 29・call-in-condition 10）、後: 14 件（全て `vulkan_external.h` の Vulkan registry 形式の typedef。固定版の宣言をそのまま写した物で、書き換えない）。
2. 目で見る規則を script で追加行に当てた: 開発の管理情報の comment、`return f(...)` の直接の return、関数の最後の `return error;`、引数の中の入れ子の呼び出し、function 内の途中の宣言、新しい関数の header comment と forward declaration（278 関数）、新しい型・file 変数の comment、禁止の comment の形、`for` の宣言、`_local` の名前、複数行 comment の `/*` 行の文章。`git diff --check` は空。

### 直した件数（種類別）

- comment の中の開発の管理情報の削除: 19 か所（ws113-p013・ws132-p002・ws132-p003・ws164-p002・ws213-p001・ws113-p005・BUG-266・ws131-p006・BUG-183・ws113-p003・ws052-p012・ws035-p102・ws177-p033・ws074-p229・BUG-172。文の意味は保った）
- 条件の中の関数の呼び出しを名前の付いた変数に: 10 件（poll.c の `atomic_load_acquire`・`poll_follows` 2、unix-socket.c の `unix_peer_ref`、bluetooth-zedbsd.c の `bt_scan_may_start` 2、lock-key.c の `kwl_key_attempting`、main.c の `child_wake_install`・`sigaction`・`kl_backend_wakes`）
- 引数の中の入れ子の呼び出しを変数に: 5 件（gpu.c の `kern_ms_to_ticks`、filedesc.c・poll.c の `poll_slot_key`、greeter.c・shell.c の `kwl_clock_shown`）
- `return f(...)` を変数に受けて返す形に: 3 件（poll.c 2、backend.c 1）、関数の最後の裸の `return error;` を失敗と成功の return に: 1 件（poll.c の `poll_sleep`）
- 閉じ括弧の後の空行と段落の comment: 約 33 件（poll.c・system-event.c・gpu.c・libvulkan・apps-bar・clipboard・greeter・main・render-thread・scanout・sleep・threads ほか）
- 通知の呼び出し（`poll_notify_object`）の前の comment: 5 件（input.c・socket.c 3・pipe.c）

動作・API・ABI・計算の順序・error の値は変えていない（呼び出しの順序は保ち、短絡評価の順も保った）。HAL の宣言・Makefile・`mk/` は変えていない。

### 記録だけ（直さない）

- `plan/ws113/tests/host-display-events.sh` は `include/libc/vulkan/vulkan_zedbsd.h` が存在せず compile できない（WS353 以前から。WS113 の古い試験。消すかの判断は Q1）。
- `userland/desktop/libkeiland/win32/`（ime・pointer・present・window・titlebar ほか）は WS334 の変更で、この Phase の対象外。
- `userland/desktop/libbrowser/`・`libmedia`・`src/kern/sched.c`・`clock.c`・`sched-hint.c` は他の WS。この Phase では読んでいない。

### 確かめ

- build（warning 0、rc 0）: x86_64（既定）・rpi5・vf2 の各 `sysroot`、`build/<機種>/packages/{libvulkan,wayland,libkeiland,sessiond}/.stamp`、`vmunix`。ログは worktree の `build/p17-*.log`。
- host 試験 PASS: `plan/ws353/tests/{cursor-step,render-step,returns,shown,timer,printers}/run.sh`、`seat-host.sh`（53 checks）、`plan/ws217/tests/host-gpu-display-flags.sh`（34 checks、失敗 0）、`plan/ws113/tests/host-output-switch.sh`、`plan/ws197/tests/phone-shell-host-test.sh`、`plan/ws191/tests/host-audio-stream.sh`、`plan/ws309/tests/{daemon-link,print-file,io-file,seat-answer,clip-write}/run.sh`、`plan/tools/system-events/run-host-events.sh`（54 checks）。
- QEMU（Venus、P1 の out `build/p17-qemu-*`、実機は使わない）: `qemu-render-wait.sh` PASS（最後の行 `qemu-render-wait: PASS`、stop は jobs=1318 fence_waits=1318、errors なし）。
- QEMU `qemu-main-wake.sh`: 1 回目（image を build、host の load average 15）は stop の step だけ FAIL（`no KWL TIMER stopped or KWL EXIT within 20 s`。他の step は PASS、idle の timeout 0。WS353 の p005 の結果にある start・stop の時間の境の既知の問題）、同じ image で 2 回目（`IMAGE=build/x86_64/hdd-image.img`）は全 step PASS（最後の行 `qemu-main-wake: PASS`）。
- 未実施: 実機（使わない指示）、5330 の idle の再測定、cursor plane の実の確かめ（p016 からの持ち越しのまま）。

Status: 実装済み・試験 PASS（cleared は Q1 の判定）。
