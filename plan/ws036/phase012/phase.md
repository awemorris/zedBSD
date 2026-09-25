<!-- awesome-plan project=zedbsd record=ws036p012 -->

# ws036-p012: rpi4 の build と QEMU（raspi4b）での login

Phase ID: `ws036-p012`
Parent: [WS036](../ws.md)
Status: **cleared**（q366-i01、2026-09-24）
Phase disposition: normal
Queue: q366（q366-i01）
実行: メインセッション

## 経緯

2026-09-24 ユーザー指示「rpi4 カーネルはビルドできるように修正して、qemu でログインプロンプトが表示されるところまでもっていきましょう」。
受入は QEMU で login prompt が出ること。調査（p001 の rpi4 分）・uapi/kcrt の分離（p025・p024 の rpi4 分）・kernel の不足（p010）・
userland（p011）は、失敗を 1 つずつ直す流れで同じ作業の中で済んだので、この Phase にまとめて記録し、それぞれの行も cleared にした。

## 失敗の原因と直したこと

| 段 | 原因 | 変更 |
| --- | --- | --- |
| kernel の compile | arm64 だけ `-nostdinc -Iinclude/libc` で libc の header を読み、`include/uapi` の host 判定に当たっていた。さらに **LLVM の zedbsd target は x86 だけ**で、`aarch64-unknown-zedbsd` は `__ZEDBSD__` を定義しない | `platform/arm64/vmunix.mk`: amd64 と同じ `-nostdlibinc -Iinclude -Isrc -I.`。compiler が zedBSD を知らないときの既定の手段（`include/uapi/hosted.h`・`include/kern/kcrt.h` の説明どおり）として `-DKERN_UAPI_NATIVE -DKERN_KCRT_NATIVE` |
| kernel の link | libc の object と `locale-record.c` を link していた | kcrt（`src/kern/kcrt.c`・`heap.c`）へ。`setjmp-aarch64.S` を kernel から外し、`KERN_AUDIO_SOURCES` を足した |
| userland の compile | 同じ header の判定、`-mtls-dialect=trad`（clang の AArch64 は TLS descriptor だけ） | `-DKERN_UAPI_NATIVE`、`-mtls-dialect` を外した |
| userland の link | libc の source の一覧が古く（resolver 等が無い）、`__zed_setjmp` が無い。`.got` が linker script に無く、PT_GNU_STACK の segment に入った | static libc を sysroot の manifest（`ZEDBSD_SYSROOT_LIBC_SOURCES`）＋ `setjmp-aarch64.S` に。`platform/arm64/user.ld` に `.got`・TLS（amd64 と同じ） |
| 共有 libc | clang の driver が zedbsd の aarch64 の link を知らず `-m elf_i386` を渡す | `libc.so`・`dyntest` は `ld.lld` を直接呼ぶ。共有 libc の source 一覧を amd64 と揃えた |
| rtld | variant I（AArch64）の枝で未使用変数、`PT_GNU_*` の再定義 | `layout_static_tls()` を x86 と variant I で分けた。`src/rtld/elf.h` の値を `<link.h>` と同じ綴りに |
| 対象外の program | x86 専用の program（`zwl` の `rdtsc`、GPU の試験、Noct・zedinst は x86 の sysroot で build）が rpi4 にも入った | platform 欄を `amd64`（zwl・GPU 試験 4 つ）、`amd64 i386 pc98`（noct・zedinst）に |
| rootfs | 開発 file は sysroot から写すが aarch64 の sysroot は無い | sysroot の無い target では「開発 file を入れる」を n にする（`Makefile`）。`find` が無い `usr/lib` で失敗しないように |
| root の image | FAT16 の arch image（8.3 名の制約で `/sbin/sysctl` を置けない）を使っていた。`make-arch-overlay-ufs.noct` が aarch64 を知らない | SD は **FAT の boot（firmware・kernel）＋ UFS の root** に（`hdd-image.img`）。arch image は任意に。UFS の tool に aarch64。network の command の link rule を足した |
| SD の I/O | `rpi4-sdhci.c` に lock が無く、init 後に複数の I/O が register を取り合って timeout（error 42） | controller ごとの mutex |
| console | `/dev/console` の出力先（text backend）も入力も無く、getty の prompt が出なかった | 新規 `src/drivers/platform/rpi4/rpi4-console.c`: 出力は `hal_putc()`（PL011 と framebuffer）、入力は PL011 の受信 FIFO を 10 ms ごとに読む kthread |
| 起動の間欠 fault | `sched_init()` が CPU 数を配列の確保より先に公開し、arm64 では既に動いている timer の tick が NULL を読んだ（4 回に 1 回ほど `sched_clock_cpu` で data abort） | 配列を作ってから CPU 数を release で公開し、`cpu_online()` は acquire で読む（`src/kern/sched.c`、全 platform 共通） |
| firmware | `vendor/raspberrypi-firmware`（submodule）がこの checkout で未初期化 | 手元の `/home/awe/zedBSD` の clone から初期化（commit は `.gitmodules` のまま） |

HAL（`include/hal/`・`src/hal/`）は変えていない。

## 検証

| 試験 | 結果 |
| --- | --- |
| rpi4 の新しい build dir での `disk-image` | warning 0、`arm64 vmunix check: PASS`、`Raspberry Pi 4 image check: PASS`（`check-disk-image` も PASS） |
| QEMU raspi4b の起動（`plan/ws036/tests/boot-rpi4.sh`） | **login prompt**（[boot-serial.txt](evidence/boot-serial.txt)）。serial で root の login、`uname -a`（aarch64）、`id`、`df`、`ps`、pipe、`/bin/dyntest`（rtld・TLS・dlopen 全段 PASS）、SD への書き込みと読み戻し |
| 起動の繰り返し（sched の修正後） | 8/8 で login prompt（修正前は約 1/4 で起動中に fault） |
| amd64・pcat・pc98 の CI kernel | warning 0 |
| amd64 の disk-image（serial mirror 付き）の起動 | login、`dyntest` PASS（sched・rtld・Makefile の変更の回帰） |

`make run`（`platform/arm64/run.mk`）も QEMU に kernel と DTB を直接渡す形にした（QEMU は Pi の GPU firmware を動かさない）。

## 残り（新しい Phase）

- **ws036-p026**: LLVM の zedbsd target に AArch64 を足す（`__ZEDBSD__`、driver の link の emulation）と aarch64 の sysroot。できれば `KERN_UAPI_NATIVE` 等の指定と `ld.lld` の直接呼び出しを外し、Noct・zedinst・開発 file を rpi4 にも入れる。LLVM の再 build が要る。
- **ws036-p027**: 起動 parameter。arm64 の HAL は `hal_get_arch_handoff("boot.command-line")` を返さないので、kernel は legacy autoroot（FAT の boot の隣の UFS root）で起動している。DTB の `/chosen/bootargs`（`cmdline.txt`）を渡すには **HAL の差分が要る（承認待ち）**。
- console は teletype（位置指定の書き込みは捨てる）。USB keyboard は無い（rpi4 の USB は xHCI が PCIe の先で、driver が無い）。
- 実機（Raspberry Pi 4）での確認はしていない。
