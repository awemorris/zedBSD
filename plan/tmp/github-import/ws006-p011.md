<!-- awesome-plan project=zedbsd record=ws006-p011 -->

既存計画の取り込み。本文はローカルの現行記録、リポジトリ資料へのリンクは基準コミットの履歴です。Issue作成・open状態は実行承認や未完了判定を意味しません。Awesome Planの状態同期導入前のため、実際の状態は本文を参照してください。

元資料: `plan/ws006/phase011/phase.md`

親: [ws006](https://github.com/awemorris/zedBSD/issues/7)

# WS006-p011: terminal identity for graphical PTYs

Date: 2026-09-09
Status: completed (q147)
Parent: [WS006](https://github.com/awemorris/zedBSD/issues/7)
Timebox: 60 active minutes
Dependency: p009 q146 actual Xzed/zterm failure.

Evidence: [q147 results](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws006-input/phase011-terminal-identity/results.md).

## Established defect

The GUI receives pointer focus and executes `tty > /dev/console`, but reports
`not a tty`. `userland/base/libc/posix.c` implements isatty using only the console
private ioctl, and ttyname/ttyname_r always name `/dev/console`. PTY slaves
implement the same TCGETS/termios contract as other terminals.

## Implementation and acceptance

1. Use the common termios query for isatty, preserving EBADF vs ENOTTY.
2. Resolve terminal names by fstat/stat device/inode identity under `/dev` and
   `/dev/pts`, including console/VT and PTY paths. Do not hardcode a PTY rdev
   encoding or invent an ioctl. Return proper errors and respect buffer bounds.
3. Add a native PTY fixture to the maintained USB/GUI image: master/slave/dup,
   path identity, short output, invalid descriptor and non-terminal cases.
4. Run explicit three-platform builds, native PTY checks and actual Xzed USB
   pointer/keyboard acceptance. Keep the timed pre-return PTY proof so queued
   ordinary-console input cannot falsely pass the GUI gate.
5. Investigate any observed duplicate delivery to the sleeping outer console
   separately. A broader graphics/TTY ownership change needs its own phase;
   do not silently alter evdev grab or restore legacy console-event ioctls.
