<!-- awesome-plan project=zedbsd record=ws314-p005 -->

# ws314-p005 `kl_process_*` の核と Installer・Terminal・Phone の移行

- 親: [WS314](../ws.md)。設計: [design.md](../design.md) §4。Queue: q1690 / q1690-i01（担当 P2、2026-10-30）。
- Status: cleared（2026-10-30。zedBSD（QEMU）で kl_process の試験と Terminal が PASS。Installer の probe と Phone の viewer の QEMU は未実施）
- Disposition: normal

## 目的・受け入れ

- design.md §4 の `kl_process_*`（KL_VERSION 98）: 起動（argv・env の変更・directory）、stdin への書き、stdout・stderr の byte と行
  （`KL_PROCESS_LINE_CUT`）、level の出力 event、終了の event と code、kill（HANGUP・TERMINATE・KILL）、terminal の大きさと mode、close。
  fd・pid を公開しない。Installer・Terminal・Phone を移す（review 23 で p013・p014 を分けた後の (a)）。
- build warning 0（zedBSD 3 arch・Linux）、host の試験、zedBSD の上での動き。

## 実装

- 公開: `include/keiland/keiland_process.h`（新、`keiland.h` から）、`keiland_app.h`（`KL_APP_PROCESS`）、`keiland_version.h`（98）、
  exports.map（`kl_process_*` 11）。
- `libkeiland/process.c`（OS に依らない部分）: 記録・stream の ring（`KL_PROCESS_BUFFER`）・行の切り出し・参照の数・dispatch の
  `keiui_processes_queue`（印か byte のある process で queue に未取得の event が無い物に 1 つ。出力は level、入力の空きと終了は 1 回）・
  `kl_app_take` の `keiui_process_taken`・`kl_app_close` の `keiui_processes_close`・close で queue から除く（`keiui_app_forget_process`）。
- `libkeiland/platform/process-posix.c`: path（PATH の検索）・argv・envp を親で作る。pipe は `posix_spawn`（null device・`adddup2`・
  `addchdir_np`・`addclosefrom_np`・`SETSIGDEF`（全 signal）・`SETSIGMASK`（空））、terminal は `forkpty`（子は signal を既定へ・mask を空・
  `chdir`・`closefrom`・`execve` だけ）。thread `kl-process-wait`（`waitid(WNOWAIT)` → lock の下で印と `waitpid`）と `kl-process-io`
  （`poll`、制御の pipe は非 blocking、終わりの後は pipe を読み切り terminal は最長 100 ms、満ちた stream は program が読むまで待つ）。
  FreeBSD は `<libutil.h>` と `-lutil`（Makefile.freebsd）。
- Installer（`helper.c`・`installer.h`・`flow.c`・`main.c`）: probe・session を `kl_process`、行は `read_line`（CUT は続きごと捨てる）、
  要求は `kl_process_write`、session の終わりは `kl_process_close_input`、終わりの status は wait の status の形（EXITED は code << 8）。
  `inst_helper_watch` を外した。
- Terminal（`main.c`・`window.c`・`terminal.h`）: tab の shell を `KL_PROCESS_TERMINAL`（`TERM=xterm`、grid の大きさ）、読みは
  `kl_process_read`（1 回 16 × 8 KB の上限のまま）、書きは `kl_process_write`、大きさは `kl_process_resize`、password の判定は
  `kl_process_terminal_mode`、tab を閉じる時は HANGUP と close、`READY_EXIT` と読み切りで tab を閉じる。`window.c` の watch を外した。
  `forkpty`・`ioctl`・`tcgetattr`・`waitpid`（tab）の include と呼び出しを外した（新しい窓の二重の fork は p013）。
- Phone（`main.c`）: viewer を stream 0 の `kl_process`、`READY_EXIT` で slot を空ける（16 の上限と告知はそのまま）。`posix_spawn`・`waitpid` を外した。

## 確かめ（2026-10-30）

- Linux（`keiland-linux.mk` の all、`build/ws314-p005-linux`）: make 0・warning 0。
- zedBSD（`plan/ws314/tests/build-zedbsd.sh`）: amd64（desktop 19 と userland/tests 9）・arm64（19）・riscv64（19）・installer の image の
  installer: make 0・warning 0。（riscv64 は最初、sysroot の `keiland.h` が古いまま（p004 の build の途中の写し）で止まった。header を touch して
  再 build で 0。）
- host の試験 `plan/ws314/tests/process-test.c`（`sh plan/ws314/tests/host-test.sh process build/ws314-p005-linux`）: 行（"\r\n"、終わり無しの
  最後の行、stderr、exit 3）、入力（cat、close_input、終わりの後の EPIPE）、洪水（1000000 byte を 1000 ずつ、level）、CUT（300 字の行を
  99・99・99・3）、stream 0 の TERMINATE（SIGNALED 15、終わりの後の kill は 0）、環境（設定・削除）と directory、ENOENT（PATH と path）、
  terminal（`stty size` 24 80 → resize → 30 100、`stty -echo` の mode、exit 4 と最後の出力）、HANGUP（SIGNALED 1）、close の後に event 無し。
  PASS（3 回）。TSan の libkeiland でも PASS・警告 0。post-test も PASS。
- `plan/tools/keiland-os-boundary/check.sh`（Q1 が check.sh と app-allow.tsv を q1690 の所有に足した、2026-10-30）: platform の層を path で
  例外にした（C2 と L1 は check.sh で `userland/desktop/libkeiland/platform/` を除く、A4 と A1（`/dev/null`）は app-allow.tsv の行、理由
  「libkeiland's platform layer (starts processes without showing the app an fd or pid; Guardrail app と設定)」）。phone/main.c の A4 の行を外した。
  terminal/main.c の A4 の行は新しい窓の二重の fork（p013 で kl_launch へ）が残るので p013 まで残す。L1 の Terminal の pty の header の特例
  （D14）は Terminal から `#if` が消えたので外した。結果: 基点（6485c1ddc の git archive）と比べて新しい FAIL は B5 の
  `keiland_process`（p001 の分割から続く B5 の類。基点の archive は git が無く B5 を数えないので差に出る）だけ。消えた FAIL: installer/helper.c の
  A4（fork・execv）。前からの FAIL（installer/admin・installer の A1・A5・C4、libbrowser の A1、B5）はそのまま。
- zedBSD（QEMU、2026-10-30、`plan/ws314/tests/guest.sh build/p2-q/run4 build/ws314-img`、image は
  `plan/tools/guest/test-image.sh plan/ws314/tests/config-amd64-guest.mk build/ws314-img`、Venus、boot disk は `qemu-unsafe.sh` で cache=unsafe、
  ssh・gdb の port は guest.py が空きから選ぶ、runtime は build/p2-q/run4/runtime）: compositor（`/bin/wayland --testing`）の desktop client
  （`guest-client.sh`）として post-test・process-test・instance-test を zedBSD 向けに build して走らせ、全て PASS。Terminal
  （`--command='echo zterm-42 > /tmp/zterm.txt; exit 3'`）は command を走らせ、shell の終わりで `reason=shell-exited` で閉じた。
  判定は SSH で取った各試験の結果の行（console の log は読まない）。途中の失敗と直し: process-test の terminal の mode の確かめは zedBSD の sh が
  行の編集の間 ECHO・ICANON を切るので、`read` の待ちの間に確かめる形にした（Linux の dash では前の形でも通っていた）。instance-test は
  compositor が client に `XDG_RUNTIME_DIR=/tmp`（他人も書ける）を渡すので ENOTSUP。試験の側で私的な directory を渡した（library の動きは正しい）。
- 未実施: Installer の probe・session（installer の image、`plan/tools/installer/p011-install.py`）と Phone の viewer の QEMU の動き。
