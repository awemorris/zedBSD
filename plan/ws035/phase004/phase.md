<!-- awesome-plan project=zedbsd record=ws035p004 -->

# ws035-p004: bootヘッダを `include/kern/boot/` へ

Phase ID: `ws035-p004`
Parent: [WS035](../ws.md)
Status: cleared（q322-i01、2026-09-23）
Phase disposition: normal
Queue: q322（q322-i01）
実行: メインセッション

## 移動（2026-09-23ユーザー指示）

| 移動前 | 移動後 |
| --- | --- |
| `include/kern/boot.h` | `include/kern/boot/boot.h` |
| `include/boot/parameter-handoff.h` | `include/kern/boot/parameter-handoff.h` |
| `include/boot/parameters.h` | `include/kern/boot/parameters.h` |
| `include/boot/provenance.h` | `include/kern/boot/provenance.h` |
| `include/boot/pc98-handoff.h` | `include/kern/boot/pc98.h` |
| `include/kern/rpi4/boot.h` | `include/kern/boot/rpi4.h` |
| `include/kern/sun4u/boot.h` | `include/kern/boot/sun4u.h` |

`include/boot/`、`include/kern/rpi4/`、`include/kern/sun4u/` はツリーから消えた。
`include/` の直下は `drivers`・`hal`・`kern`・`libc`・`uapi` の5つになった。

## 各ヘッダの役割（調査）

| ヘッダ | 役割 | 主な利用者 |
| --- | --- | --- |
| `boot.h` | bootloader→kernelの受け渡し（`struct boot_handoff`）とBIOS呼び出し | kernelの中核（`kernel.h`・`platform.h`・`vfs.h`・`main.c`・`boot.c`・`entry.c`・`sysctl.c`）、機種別ヘッダ3つ、x68kのbootloader、pc98のIDE driver |
| `parameters.h` | boot parameterの文字列上限（3071）と既定の文字列 | `boot.h`、3つの `vmunix.mk` |
| `parameter-handoff.h` | parameterを渡すrecordの形式（`BPR1`、版、flag、大きさ） | bootloaderの共通ヘッダと `.inc`、x86のHAL、3つの `vmunix.mk` |
| `provenance.h` | 起動元の記録（版、88 byte、partitionの識別） | amd64の受け渡し、UEFIのvolume探索、`boot.h` |
| `pc98.h` | PC-98固有の受け渡し（共通部24 byte＋parameter record） | pc98のbootloader（asm 2つ）、pc98のHAL、x86のHAL |
| `rpi4.h` | RPi4固有の拡張（`struct rpi4_boot_handoff`） | rpi4のHAL、kernelのrpi4 platform |
| `sun4u.h` | sun4u固有の拡張 | sparcv9のbootloader・HAL・kernelのplatform |

kernelとbootloaderの両方が読む。asmからも読めるよう `__ASSEMBLER__` で構造体定義を囲っている。

## include guardの整理

移動後の場所に合わせて `KERN_BOOT_*` に揃えた: `KERN_ABI_H`→`KERN_BOOT_H`、
`KERN_KERN_RPI4_BOOT_H`→`KERN_BOOT_RPI4_H`、`KERN_KERN_SUN4U_BOOT_H`→`KERN_BOOT_SUN4U_H`、
`KERN_BOOT_PC98_HANDOFF_H`→`KERN_BOOT_PC98_H`。他の3つは既に `KERN_BOOT_*` だった。

## 結果

- 参照の置換: 40ファイル＋引用符形の10ファイル。bootloader、文書、試験、kernel、HAL。
- **HALの変更は9ファイル10行で、すべて `#include` のパス置換**（承認済みの範囲。宣言・実装は不変）。
- **build**: kernel warning 0、`amd64 vmunix check: PASS`。`make -j32 disk-image` エラー0。
- **boot-test**: **PASS**（行頭の `login:`）。`boot-test/login.png`。

## 途中で直した取りこぼし

置換規則が `<kern/boot.h>`（山かっこ）だけを対象にしており、`"kern/boot.h"`（引用符）の10ファイルが漏れていた。
このプロジェクトは両方の書き方を使うので、以後の置換では両方を対象にする。

## 範囲外

amd64以外のbuild（編集のみ。WS036）、実機。host fixtureは流していない（移動だけのPhaseのため）。
