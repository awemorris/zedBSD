<!-- awesome-plan project=zedbsd record=ws331-p006 -->

# ws331-p006 規約の全文の見直し

- 親: [WS331](../ws.md)。Queue: q1737（担当 P3、worktree `/home/awe/zedBSD-worktrees/p3-seq`、branch `agent/p3-seq`、2026-10-31）。
- Status: in-progress
- 目的: WS331 の commit（p002〜p005・p007〜p011）が変えた C（`src/drivers`・`include/drivers`・`src/hal`・`include/hal`・`src/kern`・`include/kern`・`tests/*-fdt-host-test.c`、223 file）を [coding-style.md](../../coding-style.md) の全文で見直し、書き方だけを直す（動作は変えない）。sparcv9 の file（`include/hal/arch/sparcv9.h`・`include/hal/arch/atomic/sparcv9.h`・`src/drivers/platform/sun4u/`）の変更は別の commit。
- 受け入れ: 変えた file を含む全 port の kernel の build が warning 0、`git diff --check`、主な port の QEMU の起動（login prompt、`plan/tools/boot-test.sh`）。

## 進め方

- 対象の commit: WS331 の phase.md が挙げる WIP（merge でない物）27 個（acd880a9a・6f31f7e03・b7e70b3be・c956a7615・fae1fcd11・157f65d69・84e3e2698・609d97bd5・fde6f9ece・1d950e03c・d796224d3・977f43204・627bd83ff・6888c59b7・e51965db1・74d6760d2・c95f1c4f6・52910a04e・aab1df180・4c1c9c3d7・ae0b1b031・0e4a71447・22e324fc9・c1ced3014・ee7fcb04b・d272f0a05・de67657dc）。今も在る file だけ。
- 見直しは 3 つに分けた（file は重ならない）: drivers（90 file）、hal（85 file）、kern と include/kern と tests（48 file）。それぞれ conformance-reviewer が書き方だけを直し、P3 が差分を読んで commit する。

## 再開点（2026-10-31、P3 の返却の時）

- hal の組（7 file）と kern の組（12 file＋sleep.c の段落の分け）は commit 済み（下の SHA）。helper の報告: 15 port の vmunix の build が rc 0・warning 0（3 つの組の編集が混ざった状態）、rpi4・rpi5 の fdt の host 試験 PASS、odroid-c5 の fdt の host 試験は DTB の取得が HTTP 429 で未実施。
- drivers の組（90 file）の helper は返却の時に build の途中で、その差分（src/drivers・include/drivers）は未 commit・未確かめ。sun4u の file の変更があれば別の commit にする。
- 残り: drivers の差分を読んで commit、全 port の build（warning 0）、主な port の QEMU の起動（amd64 の login prompt、raspi4b、armv7-virt・riscv64-virt の HAL READY）、WS331 の完了の形への書き直し（p005 の Q1 の判定を含む）。
