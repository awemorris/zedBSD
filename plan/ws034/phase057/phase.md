<!-- awesome-plan project=zedbsd record=ws034p057 -->

# ws034-p057: userland/base のプログラムを動的リンクに

Phase ID: `ws034-p057`
Parent: [WS034](../ws.md)
Status: **cleared**（q370-i01、2026-09-24）
Phase disposition: normal
Queue: q370（q370-i01）
実行: メインセッション

## 経緯

2026-09-24 ユーザー指示「userland/base/ にスタティックリンクのバイナリが残っていたら、ダイナミックリンクに変更しておいてください。」

調べると、amd64 の image の実行 file 171 のうち **167 が静的**だった（sh・init・login・基本の command・network の command・
sysctl・mount・Xzed・zterm・noct など）。動的なのは toolchain wrapper で作る package の 4 つだけ。pcat・pc98・rpi4 も同じ。

## 決定

- 実行 file は **PIE**（`-pie`、`/lib/ld.so`、`NEEDED libc.so`）。zedBSD の loader は copy relocation を持たないので、libc の data
  （`environ`・`optarg` 等）を参照する実行 file は位置独立でなければならない。object は既存の `$(BUILD)/dynamic/obj`（`-fPIC`）を使う。
- 対象: sh（readline を含む）・sysctl・mount/umount・network の command・基本の command（`ZEDBSD_USERLAND_PACKAGE` の basic と network）、
  Noct（CMake の build）。4 platform（amd64・pcat・pc98・rpi4）。
- 静的のまま残す: `POSIX-R1/R2`・`SUSV4-XSI`・`SMP-STRESS`・`NOCT-JIT-VM-PROBE` 等の試験用 ELF（静的 runtime そのものの試験。
  image の program ではない）。

## 変更

- `platform/amd64/vmunix.mk`・`platform/pcat/vmunix.mk`・`platform/pc98/vmunix.mk`・`platform/arm64/vmunix.mk`: 上の program の link を
  `*_APP_LINK`（crt1.o ＋ object ＋ `-l:libc.so`、`-pie`、`--dynamic-linker=/lib/ld.so`、`-z now,relro,separate-code`、`--gc-sections`）に。
  検査は `nm -u` と `check-user-elf` から `check-dynamic-elf.py --role application --needed libc.so` へ。arm64 は `ld.lld` の既定の
  64 KiB 境界を `-z max-page-size=4096` に（付けないと 1 つが 130 KB になった）。
- `userland/base/noct/zedbsd.cmake`・`Makefile`: Noct を `-fPIE`（`noct`・`noctapi` の library にも。toolchain の `-fno-pie` の後に置く）、
  crt1.o と build の `libc.so`（`ZEDBSD_DYNAMIC_DIR`）で link。静的な compiler-rt の bundle（PIC でない）は外した（soft-float は libc.so にある）。

## 検証

| 試験 | 結果 |
| --- | --- |
| 静的な実行 file の数 | amd64 の CI image の rootfs 全体で **0**（動的 188）。pcat 0/168、pc98 0/165、rpi4 0/166 |
| amd64（serial の image `plan/ws034/tests/config-amd64-stream.mk`） | login、init・syslogd・networkd・cron が動く、ps・ls・sed・tr・sort・uname・df、Noct の script（42） |
| amd64 の CI image | `plan/tools/boot-test.sh`（UEFI）で login PASS |
| pcat | `BOOT_MODE=bios-ide` の boot-test で login PASS |
| pc98（PC-98 fork の QEMU、`plan/ws034/tests/pc98-dynamic.py`） | login、`uname -m`・`date`・`ls /lib` |
| rpi4（`plan/ws036/tests/boot-rpi4.sh`） | login、`dyntest` PASS。root の使用量 19664 → 9296 block |
| build | 4 platform とも error 0（warning は既存の package の Noct・LLVM 由来のみ） |

## 残り

- **process の起動が遅くなった**: 200 回の `/bin/uname` が 静的 1.0〜1.2 秒 → 動的 2.6〜2.8 秒（1 回 約 5 ms → 13 ms）。libc.so は
  自分の関数への JUMP_SLOT 431・GLOB_DAT 36 を毎回 `-z now` で解決し、ld.so と libc.so の page を毎回 fault する。→ ws034-p058。
