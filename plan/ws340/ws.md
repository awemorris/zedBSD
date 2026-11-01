<!-- awesome-plan project=zedbsd record=ws340 -->

# WS340 Raspberry Pi 4・Pi 5・VisionFive 2 の graphical boot（logo を出し log を出さない、PC/AT と同じ起動を menuconfig で選べる）

- Master: [plan/master.md](../master.md)（2026-11-01 20:49 作成）。Primary Milestone: MG006。
- Status: planning（担当は未割当）
- 目標: Pi 4（BCM2711）・Pi 5（BCM2712）・VisionFive 2（JH7110）で、menuconfig の `ZEDBSD_GRAPHICAL_BOOT`（今は選べるが、この 3 機では効かない）を y にすると、PC/AT（amd64 の BIOS・UEFI の loader）と同じく、起動の間は zedBSD の logo の画面だけを出し、kernel の log を出さない（`ZEDBSD_BOOT_KERNEL_MESSAGES` が n）。n の時は今の text の起動のまま。

## ユーザーの決定

- （2026-11-01）ユーザーから、Pi 4・Pi 5 の boot loader を改修して logo の画面を出し log を出さない PC/AT と同じ起動を menuconfig で選べるようにする（設定は既にあるが実際には使えない）、という趣旨の指示を受け、この WS を立てた。
- （2026-11-01）ユーザーから、VF2 でも同じ事をこの WS に足す、という趣旨の指示を受けた。

## 今の形（2026-11-01 Q1 の調べ）

- PC/AT: zedBSD の loader が `zedbsd.cfg` の `logo=logo.ppm`・`kmsg=quiet`・`login=` を読み、framebuffer（VBE・GOP）に logo（`userland/desktop/artwork/zedbsd-boot-splash.png` から `tools/build/make-boot-splash.py` で作る）を描いてから kernel に渡す（`platform/amd64/vmunix.mk` の AMD64_BIOS_ZEDBSD_CONFIG・AMD64_NATIVE_UEFI_ZEDBSD_CONFIG）。
- Pi 4・Pi 5: zedBSD の loader を通らない。Pi の firmware（Pi 4 は boot partition の start4.elf など、Pi 5 は EEPROM の bootloader）が `config.txt` と `cmdline.txt` を読んで kernel を直に起こす（`platform/arm64/bootloader.mk`・`firmware.mk`・`rpi5-firmware.mk`）。cmdline.txt の `login=` は `platform/arm64/vmunix.mk` が書く。`ZEDBSD_GRAPHICAL_BOOT` は arm64 の側では使われていない。firmware の虹色の splash（`disable_splash`）もある。
- VF2: 板の `/boot/boot0/zedbsd.cfg` で起動する（今の板の cfg は kernel・rootpart の 2 行、login=graphical を外したまま。master の「止まっている物」）。経路（U-Boot と zedBSD の loader の関係）と graphical boot の扱いは p001 で調べる。

- （2026-11-02）ユーザーの Pi 5 の UAT で、CI の image では graphical boot が有効になっていなかった、という趣旨の指摘を受けた。この WS で Pi で graphical boot を働かせた後に、CI の Pi 5（と Pi 4・VF2）の config で ZEDBSD_GRAPHICAL_BOOT を y にする。

## Phase

| Phase | 目的 | 状態 | 依存 |
| --- | --- | --- | --- |
| ws340-p001 | 調べと設計: 3 機の起動の経路、logo をどこで描くか（firmware の後の kernel の早い段の framebuffer か、loader を挟むか）、firmware の splash と log の抑え方、menuconfig と image の build の規則、試験の道（boot-test.sh の画面の PNG、実機） | planning | — |

## Resume point

- 2026-11-01 Q1: 作成。p001 の担当の割当を待つ。
