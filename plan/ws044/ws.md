<!-- awesome-plan project=zedbsd record=ws044 -->

# WS044: rpi4 を開発に使える形にする（console の font、FAT32 の boot、lldb）

<!-- awesome-plan-current:start -->
Status: incomplete
Primary Milestone: MG008
Related Milestones: MG001, MG003
Objectives: O2, O4
Parent: [Master](../master.md)
Queue: なし
Resume point: p001・p005 cleared。実機の起動の結果を待つ。p002・p003 は rpi4 の kernel の安定化の後
<!-- awesome-plan-current:end -->

## 目標

aarch64 は主対象（2026-09-24 ユーザー決定）。rpi4 を、画面で読める console、標準的な boot partition、
user program の debugger を持つ開発機として使えるようにする。

2026-09-24 ユーザー指示: 「rpi4 でlldbが動くようにしてほしいです。rpi4ではbootパーティションはFAT32にしてください。
シリアル以外は先にカーネルを安定してからでいいです。あとrpi4のコンソールのフォントはPC/ATのものを複製して使ってください。
パブリックドメインなので大丈夫です。」シリアルは QEMU でのデバッグ用で、既に PL011 の console で使えているので対応不要（同日の回答）。

## 完了の条件

- framebuffer の console が PC/AT の 8x16 font（public domain）で描かれる。
- SD の boot partition が FAT32 で、firmware が起動できる。
- rpi4 の guest で lldb が user program に breakpoint を置き、止めて backtrace を出せる。
- 変更した source の規約の確認。

## 制約と依存

- HAL（`src/hal/arm64/bsp-rpi4/`）の変更は差分ごとの承認が要る。p001 の font の差分（font の追加と描画の置換、hal.h は不変）は
  2026-09-24 にユーザーが承認した。
- p002・p003 は **rpi4 の kernel の安定化の後**（ユーザー指示）。安定化の基準はまだ決まっていない（ユーザーとの合意が要る）。
- lldb は WS032 の package（amd64 で breakpoint・watchpoint を確認済み）。aarch64 には zedbsd target の LLVM と sysroot が要る
  （ws036-p026）。kernel 側は ptrace が amd64 専用（`struct reg` が `__x86_64__` だけ）なので aarch64 の ptrace とレジスタの定義が要る。

## Phase 一覧

| Phase | 内容 | Status | 依存 |
| --- | --- | --- | --- |
| [ws044-p001](phase001/phase.md) | console の font を PC/AT の 8x16（`vgafont`）の複製へ置き換える（HAL、承認済み） | cleared（q377-i01。QEMU raspi4b の画面で login prompt を読めた） | — |
| ws044-p002 | SD の boot partition を FAT32 にする（`make-rpi4-hdd-image.py`、検査の script、kernel の FAT の読み取り） | planning | rpi4 の kernel の安定化 |
| ws044-p003 | aarch64 で lldb を動かす（aarch64 の ptrace と `struct reg`、lldb の package の aarch64 build、guest での breakpoint・backtrace） | planning | rpi4 の kernel の安定化、ws036-p026 |
| [ws044-p005](phase005/phase.md) | 実機起動の準備: firmware の読み込み番地（`kernel_address=0x80000`）、EMMC2 の 32-bit だけの register access。実機との違いの洗い出し | cleared（q378-i01。QEMU で起動・SD の読み書き。実機はユーザー確認待ち） | p001 |
| ws044-p004 | 変更した source の全文の規約確認と rpi4 の回帰 | planning | p001〜p003、p005 |
| [ws044-p006](phase006/phase.md) | 実機で framebuffer に出ない: 描いた後に data cache を memory へ（HAL、承認済み） | cleared（QEMU まで。実機はユーザーの確認待ち） | p005 | `src/hal/arm64/bsp-rpi4/framebuffer.c` |
| [ws044-p007](phase007/phase.md) | 実機の表示を 1920x1080 に固定（config.txt と HAL の framebuffer の大きさ、承認済み） | cleared（QEMU まで。実機はユーザーの確認待ち） | p006 | `platform/arm64/config.txt`、`src/hal/arm64/bsp-rpi4/framebuffer.c` |
| [ws044-p008](phase008/phase.md) | 実機の起動の診断（ACT LED の段階、テスト模様、SCTLR_EL1 の初期化）と boot 設定の洗い直し（HAL、承認済み） | cleared（原因は config.txt の hdmi_force_hotplug が空の HDMI0 を display 0 にしていたこと。HDMI0 で表示を確認。HDMI1 と UART は実機の結果待ち） | p007 | `src/hal/arm64`、`platform/arm64/config.txt` |
| [ws044-p009](phase009/phase.md) | 実機で init が SIGILL: 命令 cache の同期（実行可能にする page に `hal_sync_instruction_stream`）と EL0 の SCTLR の権限（HAL、承認済み） | cleared（QEMU まで。実機はユーザーの確認待ち） | p008 | `src/hal/arm64/space.c`・`locore.S` |
