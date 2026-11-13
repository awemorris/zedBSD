<!-- awesome-plan project=zedbsd record=ws348-p001 -->

# ws348-p001 Pi 500 の dtb を image に入れる

- WS: [ws.md](../ws.md)。Queue: q2042-i01（P1、worktree `/home/awe/zedBSD-worktrees/p1-pi500`、branch `agent/p1-pi500`、基点 main 868de90ce8、途中で main f949ade4f2 を取り込んだ）。
- 状態: cleared（2026-11-13 P1。Pi 500 の実機での起動は未実施）

## 経緯

- 2026-11-13 ユーザーが Makefile の変更（Pi 500 の dtb を image に入れる事）を承認した。
- P1 が `mk/platform-rpi5/firmware.mk` に 1 行を足すと、それだけでは FAT に入らない（cache の数の検査 `RPI5_BOOT_FIRMWARE_ENTRY_COUNT` と、FAT に写す file の表 `mk/platform-rpi5/tools/make-rpi4-hdd-image.py`・`check-rpi4-hdd-image.py` の `FIRMWARE_FILES["rpi5"]`）と分かり Q1 に返した。P1 の mk の変更の一部は権限の判定で止められたので、P1 は mk を変えていない。
- 同じ日にユーザーの指示（firmware は zedBSD-firmware から取る）で、Q1 が zedBSD-firmware に Pi 500 の dtb と dts を足して push（2670dc1）し、main f949ade4f2 に mk の変更（firmware.mk の dtb の行・数 4・comment、tools の 2 つの py の表、`userland/firmware/zedbsd-firmware.mk` の REVISION を 2670dc1 に）を入れた。P1 は worktree の未 commit の変更を戻し、main を取り込んだ（fast-forward、HEAD は f949ade4f2、P1 の commit はまだ無かったので merge commit は出来ていない）。

## 確かめ（2026-11-13 P1）

1. dtb の照合: raspberrypi/firmware の commit `3d301dd924bcd758a4c8cb19fe8531031f033f43` の `boot/bcm2712-rpi-500.dtb` を `curl` で取り、78659 byte、SHA-256 `dd98c7c3780e4c88148401c95d2222ccb4e585ed4bfbe2b5c67531b0a06958b5`（報告と一致）。`fdtget` で `model` は `Raspberry Pi 500`、pin controller は `brcm,bcm2712d0-pinctrl`（D0）。
2. mirror の照合: zedBSD-firmware `2670dc1706748dc46df4ad7f5308499a7c305e8d` の `rpi5-boot/` の 4 file（`bcm2712-rpi-5-b.dtb` 78703、`bcm2712d0-rpi-5-b.dtb` 78707、`bcm2712-rpi-500.dtb` 78659、`COPYING.linux` 18693）を取り、大きさと SHA-256 が `firmware.mk` の表と全部一致。
3. image: ユーザーが worktree で rpi5-boot の license に同意した後（bcm43455 の firmware の license は同意が無いので、`bcm43455-firmware` だけを一覧から外した試験の config `build/p1-pi500/config-rpi5-image.mk` で）、`make CONFIGMK=build/p1-pi500/config-rpi5-image.mk BUILD_JOBS=4 -j4 disk` が exit 0。`build/rpi5/hdd-image.img`（1276116992 byte）。
   - cache `build/sources/firmware/rpi5-boot/3d301dd…/` に 4 file。build の log に「could not fetch … from the zedBSD firmware repository; fetching it from upstream」の warning は無い（mirror から取れた）。
   - FAT（`mdir`、volume `ZEDRPI5`）: `bcm2712-rpi-5-b.dtb`・`bcm2712d0-rpi-5-b.dtb`・`bcm2712-rpi-500.dtb`・`COPYING.linux`・`config.txt`・`cmdline.txt`・`vmunix`。3 つの dtb の SHA-256 は表と一致（`mcopy` で読み出して照合）。image の検査は `Raspberry Pi 4 image check: PASS`（rpi5 も同じ検査の script）と `canonical zedBSD UFS OK`。
   - build の warning は 4 行で、どれも toolchain の側（`userland/base/noct/noct/src/` の 3 つの `-Wunused-but-set-global` と、gmake の jobserver の注意）。この Phase の変更とは関係しない。
4. `config.txt` の os_check: `config.txt` に `os_check` と `device_tree` の行は無い。Raspberry Pi の `config.txt` の文書によれば、Pi 5 系の EEPROM の bootloader は既定（os_check=1）で、board に合う device tree が FAT に無いとその区画から起動しない。報告の Pi 500 で dtb を足す必要があったのはこのためと見る（実機では確かめていない）。今の image は Pi 500 の dtb を持つので、`os_check=0` を足す必要は無い。`config.txt` の comment（「C1 か D0 の dtb を選ぶ、両方ある」）は Pi 500 に触れていない（`mk/` の下で担当は変えないので、直すなら Q1 かユーザーへ）。

## 既存の cache の取り直し

- 同じ revision（3d301dd）の cache を前の tree で取った checkout では、`build/sources/firmware/rpi5-boot/3d301dd924bcd758a4c8cb19fe8531031f033f43/` に 3 file しか無く、build が `rpi5-boot-firmware: cache must contain exactly 4 entries` で止まる。
- 直し方: その `3d301dd…` の directory だけを消して、build をやり直す（`rpi5-boot-firmware` の target が 4 file を取り直して照合する）。同じ directory の親の `license-accepted` と `licenses/` は残すので、同意は聞き直されない。build の手順の文書（`docs/howto/build-from-source.md` の Diagnostics）に書いた。
- 担当の worktree では、自分の `build/` の物も消さずに Q1 に頼む（AGENTS.md §4）。この worktree は新しく取ったので該当しない。
- 案（設計の判断、実施しない）: cache の検査が「数ちょうど」なので、file を足すたびに既存の cache が全部止まる。足りない file だけを取り足して照合する形（表に無い余分な file だけを拒む）にすると、利用者が手で消さずに済む。mk の変更なのでユーザーの判断。

## 未実施

- Pi 500 の実機での起動（こちらに機械が無い）。外部の利用者に今の image で dtb を足さずに起動するかを見てもらうのが確かめになる。
- bcm43455 の firmware を含む完全な image（license の同意がこの worktree に無い）。FAT の中身は bcm43455 の有無で変わらない。
