<!-- awesome-plan project=zedbsd record=ws348 -->

# WS348: Raspberry Pi 500 の dtb を image に入れる（そのまま Pi 500 で起動）

Status: incomplete
Primary Milestone: MG003
Parent: [Master](../master.md)
Owner: Q1

## 目標

ユーザーが Makefile の変更（`mk/platform-rpi5/firmware.mk` に `bcm2712-rpi-500.dtb` の 1 行）を承認した（2026-11-13）。

## ユーザーの決定（要点）

- 2026-11-13 ユーザーから、firmware は zedBSD-firmware の repository に入れてそこから取る、Q1 が zedBSD-firmware を push してよい、という趣旨の指示を受けた。→ Q1 が `rpi5-boot/bcm2712-rpi-500.dtb` と `source/…/bcm2712-rpi-500.dts`（raspberrypi/linux 89586905、既存の source に足して cpp と dtc 1.7.2 で byte 単位で同じ dtb を作れる事を確かめた）・README・SHA256SUMS を足して zedBSD-firmware 2670dc1 を push。`userland/firmware/zedbsd-firmware.mk` の REVISION をそれに、`mk/platform-rpi5/firmware.mk`（dtb の行、数 3→4、comment）と `mk/platform-rpi5/tools/` の image の作成・検査の表に dtb を足した（担当の変更は権限の判定で拒まれたので、ユーザーの承認の下で Q1 が当てた）。
- 2026-11-13 に外部の利用者から Raspberry Pi 500（BCM2712 D0、8 GB）で zedBSD（c4cede8、2026-11-03 の source）が起動したという報告を受けた（Pi 500 の dtb を FAT に足して起動、desktop・日本語入力・Wi-Fi・有線・ブラウザ・SSH・shutdown は動作、HDMI の音は無し）。
- 2026-11-13 ユーザーから、Pi 500 の報告の #1・#6・#7 をそれぞれ WS に、#2・#3・#4 を Bug に（修正済みの疑いが強い）、#5（起動の直後の時計）は仕様、#8（build の手順）は見直して誤りがあれば直す、という趣旨の指示を受けた。

## Resume point

- 2026-11-13 Q1 が立てた。P1 が処理する。
- 2026-11-13 P1: p001 cleared（[phase.md](phase001/phase.md)）。残り: Pi 500 の実機での起動の確かめ（外部の利用者）、cache の検査を「数ちょうど」から取り足しの形にするかの判断（mk、ユーザー）。

## Phase

| Phase | 中身 | 状態 | 依存 |
| --- | --- | --- | --- |
| [p001](phase001/phase.md) | `bcm2712-rpi-500.dtb`（raspberrypi/firmware `3d301dd` の `boot/`）を取得し大きさと SHA-256 を照合して `RPI5_BOOT_FIRMWARE_FILES` に足す。rpi5 の SD の image の FAT に入る事、config.txt の os_check の扱い（合う dtb が無いと止める）を確かめる | cleared（2026-11-13 P1。FAT に 3 つの dtb、mirror 2670dc1 から取得。Pi 500 の実機は未実施） | — |
