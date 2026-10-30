# WS313 の実機の試験（ODROID-C5）

夜の実機の試験の手順（2026-10-29、ユーザーの指示「今日の夜、まとめてテストします。」）。code は実機なしで書いたので、全てが未試験。
判定は UART の対話（`c5-serial.py` が生きた line を読む）。ユーザーの判断（2026-10-29 §8「許可する」）で AGENTS.md §9 の例外を ODROID-C5 の実機に広げた（network と画面が出た後は SSH と画面の写真）。

## 準備（ユーザー）

- USB-UART の adapter（3.3 V、921600 baud を出せる物。Hardkernel の USB-UART kit、CP2102・FT232 など）を C5 の UART の connector（TX・RX・GND）につなぎ、centris の USB へ。pin の配置は wiki.odroid.com が読めず未確認（基板の silk か Hardkernel の資料で）。
- 12 V/2 A の電源（5.5/2.1 mm）。SwitchBot の plug を使うなら、その ID。
- microSD（8 GB 以上）、HDMI の display、USB の keyboard・mouse、LAN の cable（DHCP のある網）。
- bootloader の donor（下の「媒体」）か Hardkernel の ODROID-C5 の公式の image（bootloader の区画を写すため。`ODROID_C5_BOOTLOADER_FROM` に展開した `.img` の path を渡す。`.xz` などの圧縮のままでは読めない）。
- eMMC の module は外す（U-Boot は起動した媒体の boot.scr を先に読むが、eMMC から起動すると eMMC の OS が先に起動する）。

## 媒体

image は main（merge の後）か P7 の worktree で作る。sysroot が無い checkout では先に `make ZEDBSD_CONFIG=config/ci/config-odroid-c5.mk sysroot-arm64`。
bootloader は、2026-10-30 から Hardkernel の Yocto の layer（github.com/mdrjr/meta-odroid-aml の commit 548e2f037e の `recipes-bsp/u-boot-odroid-c5/files/u-boot.bin.signed`、SHA-256 8c2e66919a05921d79e130cc00a977a07cf8df6b34153e66c18c3241c0809e3c）を、512 byte の 0 の後に付けた donor（sector 1 から置く形、[phase003](../phase003/phase.md) の q1709-i01）にして `ODROID_C5_BOOTLOADER_FROM` に渡す。Hardkernel の公式の image の展開した `.img` も同じ変数で使える。試験だけに使い、image と donor を公開しない（Amlogic の blob の再配布の条件が不明）。無しでも image はでき（2026-10-29 P7 が確かめた）、その時は bootloader の区画が空で、SD からは起動しない。
desktop の image は Arm の CSF の firmware（`mali-csf-firmware`）を入れる。その license（`LICENCE.mali_csffw`、Arm の EULA）への同意はユーザーが端末で一度だけ答える（`make ZEDBSD_CONFIG=config/ci/config-odroid-c5-keiland.mk firmware-licenses`。担当は代わりに答えない）。

```sh
cd /home/awe/zedBSD-claude1
# text の console の image（GPU なし）
make ZEDBSD_CONFIG=config/ci/config-odroid-c5.mk ODROID_C5_BOOTLOADER_FROM=/path/to/hardkernel-odroid-c5.img disk-image
# （bootloader 無しの image は ODROID_C5_NO_BOOTLOADER=y を明示した時だけ作る。SD からは起動しない）
# desktop の image（Mali の driver つき）
make ZEDBSD_CONFIG=config/ci/config-odroid-c5-keiland.mk BUILD=build/odroid-c5-keiland ODROID_C5_BOOTLOADER_FROM=/path/to/hardkernel-odroid-c5.img disk-image
# 書き込み（/dev/sdX は microSD。間違えると他の disk を壊す）
sudo dd if=build/odroid-c5/hdd-image.img of=/dev/sdX bs=4M conv=fsync status=progress   # desktop の image は build/odroid-c5-keiland/hdd-image.img
```

image の形: MBR、先頭 8 MiB は bootloader（Hardkernel の image から写した物）、part 1 FAT32（8 MiB から、`boot.scr`・`vmunix`・`odroid-c5.dtb`・`data.img`・`swapfile`）、part 2 UFS の root。

## 確かめる順と判定

`plan/ws313/tests/c5-night-test.sh [--device /dev/ttyUSBn] [--plug ID] [--gpu]` が login prompt まで読み、各 stage の行を全ての行の中で探し、次の stage が時間の内に来なければ終わる。最後に stage ごとの PASS・FAIL・MISSING・SKIP・NOTE を出す。
U-Boot の 2 stage（1・2）は、silent の build の U-Boot では echo が出ないので、欠けても MISSING として先を待つ。login の後の命令は、出力の中身（`df /` に mmc、`lsusb` に HID、`ifconfig -a` に inet、`--gpu` の時 `Mali-G310`）で PASS・FAIL を決める。framebuffer の PASS は HAL が描いた事だけなので、画面は写真で確かめる。
U-Boot の prompt（`odroidc5#`）から kernel の引数を足す時は `setenv zedbsd_extra_args cpus=1` の後に `run bootcmd`（boot.scr が bootargs に足す）。boot.scr は最初に `powershow` で電源の domain の状態を出し、`powerset 5 0`・`10 0`・`11 0` で USB・SD・Ethernet を入れる。

| 順 | stage（UART の行） | 判定と、FAIL の時に見る所 |
| --- | --- | --- |
| 1 | `zedBSD: boot.scr on` | U-Boot が SD の boot.scr を読んだ。出なければ bootloader の区画（`--bootloader-from`）か boot の順。U-Boot の prompt（`odroidc5#`）で `printenv`・`fatls mmc 1:1` |
| 2 | `zedBSD: booti` | kernel と DTB を読めた。この後 U-Boot の `Starting kernel` が出るか |
| 3 | `ODROID-C5 ENTRY` | HAL の最初の行（UART は U-Boot の速さのまま）。出なければ kernel の置き場所（booti の移動）、`fdt_high` |
| 4 | `ODROID-C5 EL1 PASS`・`FDT PASS` | DTB を読めた。`FDT parse error: …` ならその名前 |
| 5 | `ODROID-C5 FRAMEBUFFER PASS` | HDMI に三色（左から赤・緑・青）の 3 秒の模様と白い枠。色の順が違えば format（a8r8g8b8）、何も出なければ U-Boot の `osd`・`vout` の命令（boot.cmd）。`framebuffer unavailable` は UART だけで進む（FAIL と記す） |
| 6 | `ARM64 MEMORY MAP PASS` | memory の量（2 GiB の見込み、DTS の linux,usable-memory） |
| 7 | `ARM64 IRQ READY`・`ARM64 SMP cpu 3 online` | GIC と PSCI の CPU_ON。止まれば `ARM64 SMP cpu N … did not come up` |
| 8 | `login: ` | SD の driver（`s905x5m-…` の行）と root の mount。止まれば SD の行。`s905x5m-usb: about to read the controller` の後で止まれば USB の電源の domain（U-Boot の `powershow` の出力の 5 番） |
| 9 | login の後の命令 | `uname -a`、`df /`、`dmesg | grep s905x5m`、`lsusb`（keyboard・hub）、`ifconfig -a`（en0 の link と DHCP）、`dmesg | grep mali`（GPU_ID） |
| 10 | desktop | keiland の image で HDMI に Keiland が出るか（写真）。Valhall の compiler と Vulkan の executor（p010・p011）は今夜は初めての実機なので、出なくても GPU の行（`mali:` の GPU_ID・firmware・MCU・queue・self-test、[gpu.md](../gpu.md) §9）を判定にする |

固まった時は、UART の log を取り直さず、board の状態を写真と UART の最後の行で Q1 に送る（QEMU ではないので gdbstub は無い）。

## script

- `c5-night-test.sh`: 電源（SwitchBot の plug を名指した時）→ `c5-serial.py` → 要約。各 command に時間の上限。何も消さない。
- `c5-serial.py`: 921600 baud の生きた line を読む（`--scale` で時間を伸ばす）。`--login USER:PASSWORD`（既定は image の kei:kei）・`--commands`・`--gpu`（keiland の image）。
  2026-10-29 P7 が pty の偽の board（正常・silent の U-Boot・中身の無い出力）で、summary が期待どおりになる事を確かめた（実機の line では未試験）。
