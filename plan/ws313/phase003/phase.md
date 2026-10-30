<!-- awesome-plan project=zedbsd record=ws313-p003 -->

# ws313-p003: 媒体と夜の試験の手順

Status: in-progress（実装済み・実機未試験）
Queue: q1675-i01・q1675-i02（P7）、q1709-i01・q1714-i01・q1716-i01（P1）
WS: [WS313](../ws.md)、設計 [design.md](../design.md) §2・§6・§7

## 実装（2026-10-29）

- `platform/arm64/odroid-c5.mk`: Hardkernel の DTS（`platform/arm64/odroid-c5/dts-sources.txt` の 31 file、pinned の raw URL、size と SHA-256）を
  `platform/arm64/tools/fetch-odroid-c5-dts.py` で `$(BUILD)/dts-sources/` に取り、cpp と dtc で `odroid-c5.dtb`（zedBSD の追加は `odroid-c5/odroid-c5.dts`: /chosen の simple-framebuffer）。
  `boot.scr`（`odroid-c5/boot.cmd` を `tools/make-uboot-script.py` で legacy image に、login= は config から）。`hdd-image.img`（`tools/make-odroid-c5-hdd-image.py`）。
- 試験: [tests/README.md](../tests/README.md)、`tests/c5-night-test.sh`、`tests/c5-serial.py`。

## 確かめ

- boot.scr: `mkimage -l` で header を読め、payload が `mkimage -T script` の物と一致。
- DTB: build でき、HAL の parser の host の試験が PASS（p002）。
- 2026-10-29（q1675-i02）: `make ZEDBSD_CONFIG=config/ci/config-odroid-c5.mk odroid-c5-fdt-host-test` が PASS（DTS を worktree の `build/odroid-c5/dts-sources/` に取り直し、
  `uart fe07a000 irq 201, 4 CPUs, 7 reserved, fb 7f800000`）。
- 2026-10-29: `make … sysroot-arm64` rc=0（worktree の `build/arm64/sysroot`）、`make ZEDBSD_CONFIG=config/ci/config-odroid-c5.mk BUILD=build/odroid-c5-image world` rc=0
  （zedBSD の source の warning 0。warning は openssl の deprecated と perl の locale だけ）、`build/odroid-c5-image/hdd-image.img` rc=0（bootloader 無し、456 MiB、
  FAT に boot.scr・vmunix・odroid-c5.dtb・data.img・swapfile、`ufs-root.img: canonical zedBSD UFS OK`）。kernel の LOAD は 0x80000〜0x8d6000 で、予約の 0x04e00000 より下。
  この image は周辺の driver が stub の時の source の物（夜の image は main で作り直す）。
- bootloader: ユーザーの判断は「取得して image に入れる」だが、公開の取得先が無く（dn.odroid.com は 403、github・Launchpad に無い）、今は `--bootloader-from`。URL はユーザーに確かめ中。
- bootloader の notice（2026-10-29、q1675-i03）: `platform/arm64/odroid-c5-bootloader.notice`。`--bootloader-from` の時、image の tool が FAT に `/BOOTLOADER.txt`（notice＋donor の名前＋写した区画（MBR の 440 byte と sector 1〜）の SHA-256）を置く（`platform/arm64/tools/make-odroid-c5-hdd-image.py --notice`、既定はこの notice）。
  調べ（GitHub の API の GET だけ）: Hardkernel の build の tree は github.com/hardkernel/u-boot の branch `odroidc5-v2023.01`（c99f3409d5、2026-02-04「Firmware update with Amlogic BSP 202508」）。Amlogic の BL2・BL30・BL31・BL32 と U-Boot は git.odroid.com の submodule（git.odroid.com は読んでいない）。Amlogic の blob の license の文は見つからず、再配布の条件は不明 → notice に「作った人の使用のため、条件が分かるまで渡さない」と書いた（推し）。U-Boot は GPL-2.0-or-later で、対応する source は submodule `bl33/v2023`。
  確かめ: 偽の donor（440 byte の code と 4 KiB の乱数）で tool を流し rc=0、FAT の `BOOTLOADER.txt` の末尾の SHA-256 が別に計算した値と一致。`--no-bootloader` では置かない（rc=0、FAT は 5 file）。
- CSF の firmware: package `mali-csf-firmware`（`userland/firmware/mali-csf/`）を作り keiland の config に入れた。license の同意はユーザーが build の時に答える。
- 実機: 未実施。

## q1709-i01（2026-10-30、P1）: ユーザーに渡す 2 つの image と bootloader の出所

- ユーザーの指示（原文、2026-10-30）: 「WS313をP1で実行します。ODROIDのテスト用イメージをください。」「AmlogicのブートローダはGitHubから探してください。」
  ユーザーの判断（2026-10-30 のクリック「公式 image を渡す (推し)」、Q1 の中継）と、その後の GitHub の探索の指示。Q1 の条件: 取得は GitHub の GET か `git clone --depth 1` まで、git.odroid.com・dn.odroid.com は触らない、候補ごとに出所を記録、S905X5M 向けと確かめられない物は使わない、image に入れる前に Q1 の OK。
- dn.odroid.com は Cloudflare の bot の確認（HTTP 403、`cf-mitigated: challenge`）。素の GET で止め、回避していない。
- Hardkernel の GitHub（github.com/hardkernel/u-boot の branch odroidc5-v2023.01）は submodule だけで binary は git.odroid.com。GitHub の hardkernel/yocto-* の mirror は空。
- GitHub の候補（raw.githubusercontent.com の commit 固定の GET、worktree の `build/tmp/q1709/bl/`）。S905X5M の Amlogic の SoC の名は **S7D**（DTS `s7d_s905x5m_odroidc5.dts`、BL30 の文字列 `S7D-s905x5m-…`）。

| 候補 | repo・commit・file | 大きさ・SHA-256 | 誰・何向け | license の表示 |
| --- | --- | --- | --- | --- |
| **A（採用、Q1 の OK 2026-10-30）** | github.com/mdrjr/meta-odroid-aml 548e2f037e01c626e3fc964fa298464df148fd51、`recipes-bsp/u-boot-odroid-c5/files/u-boot.bin.signed`（足した commit b319921106、2025-05-14「Support ODROID-C5」） | 3379200、8c2e66919a05921d79e130cc00a977a07cf8df6b34153e66c18c3241c0809e3c（git blob 1bbecf065a は API の値と一致） | Mauro Ribeiro（GitHub の company は Hardkernel、mauro.ribeiro@hardkernel.com）の Yocto の layer。ODROID-C5（`conf/machine/odroid-c5.conf`）。署名・暗号化済み、BL30 は `S7D-s905x5m-25022719`。置き方は `files/wic/odroid-c5.wks` の `rawcopy … --offset 1S`（msdos の表、sector 1 から） | recipe は `GPL-2.0-only`（U-Boot）。repo の LICENSE なし、Amlogic の blob の条件の表示なし → 再配布の条件は不明 |
| B（不採用） | github.com/codewalkerster/android_device_hardkernel_odroidc5 91c4fa663686e918520b6487c443ffd9d8858d45、`prebuilt/bootloader/u-boot.bin.sd` | 1720032、a0827774a371569f2f88206807854eb63e9ca168134aea2b530672d906679a2a | Hardkernel（Chris）の Android 14 の device tree。Android の U-Boot | repo の LICENSE は Amlogic の Reference の条件（Android TV の release 以外への移転・複製を禁止） |
| C（不採用） | github.com/LeonardWard/retropangui c6eebbcf7211eb379ed584e451bf89700a900866、`board/odroidc5/u-boot.bin.sd.bin` | 8388096、a199ef4ec55070dbdc161cbaa3fd0cf2861b9d779a07aa7baa4326ee60c4c2df | 第三者（retro game の OS）。U-Boot に自前の patch、BL30 は `S7D-s905x5m-26080103` | なし |

- A の donor（今の `ODROID_C5_BOOTLOADER_FROM` に渡す形）: 512 byte の 0 ＋ A = `build/tmp/q1709/bl/meta-odroid-aml-548e2f037e-u-boot.bin.signed.sector1.img`（3379712 byte、SHA-256 8b5d750dec9105fcbfc62c3aa211a2742f537621e4c2fd5479615649ce46da43）。MBR の code は 0、area は sector 1 から。
- **使うのは試験だけ**（Q1 の条件、notice の「条件が分かるまで渡さない」）。image・donor を公開・push・release しない。
- build（2026-10-30、main 5aca73738 ＋ 21297ef31）: `make sysroot-arm64` rc=0、`make … BUILD=build/odroid-c5 world` rc=0、keiland の `world` rc=0（`MALI_CSF_FIRMWARE_CACHE_ROOT` は worktree の build/fw-sources、license は自動で同意: 2026-10-30 ユーザー「試験に関しては私はすべてのライセンスに同意しておりますので、自動でいいです」）。zedBSD の source の warning 0（warning は openssl・openssh・perl だけ）。
- host の試験: `c5-build-check.sh q1709` の 5 つの vmunix の warning 0、meson-mmc・Mali の 10 の試験 PASS。DTS の取得は raw.githubusercontent.com の HTTP 429 で失敗 → 他の worktree の DTS の cache を複写し（fetcher が manifest の SHA-256 で検証）、`odroid-c5-fdt-host-test` PASS・meson-mmc glue PASS。
  `mali-compiler-host-test` は 2026-10-30 に main に入った `plan/tools/nvrtx/sm75/precise.comp`（NoContraction）を共通の front end が拒否して 34/35 → Mali の feature の bit（`DRV_GPU_SPIRV_FEATURE_NO_CONTRACTION`、Mali の backend は積と和を融合しない）で受けて 35/35 PASS（21297ef31・4d4193dfa）。
- image（2026-10-30 14:16〜14:17、branch agent/p1-ws313 81118cb62、`flock /tmp/zedbsd-image-build.lock make … ODROID_C5_BOOTLOADER_FROM=<A の donor> disk-image` rc=0）:
  - text: `/home/awe/zedBSD-worktrees/p1-ws313/build/odroid-c5/hdd-image.img`、461373440 byte、sha256 65f5f69b7086b7aaffc3887ae6c152aa520d673621b9f2a9b3e483718a7cde55
  - desktop: `/home/awe/zedBSD-worktrees/p1-ws313/build/odroid-c5-keiland/hdd-image.img`、464519168 byte、sha256 fa02863f3a3ce75c8723c80f521786d065b7e1d5f00a7b31b9fff1b4966a45ff（rootfs に `mali_csffw.bin`）
  - 確かめ（host）: 両方とも sector 1 からの 3379200 byte が A と cmp で一致、MBR の code は 0・署名 55aa、区画の SHA-256 e6f020856acff8aedd041058971155e8bdb8bae6e3f8764e9818a20afc7b313d、FAT に boot.scr・vmunix・odroid-c5.dtb・data.img・swapfile・BOOTLOADER.txt、UFS の root は `canonical zedBSD UFS OK`。
  - 書き込み: `sudo dd if=<image> of=/dev/sdX bs=4M conv=fsync status=progress`。
- DTS の取得の流し直し（14:17、Q1 の指示で 1 回だけ）: raw.githubusercontent.com がまだ HTTP 429。image と host の試験は複写した cache（SHA-256 で検証）で作った。
- 実機: 未実施（U-Boot が A でこの image の boot.scr を読むかも未確認）。

## q1714-i01（2026-10-30、P1）: Amlogic の bootloader と Mali の firmware を zedBSD-firmware と menuconfig の Firmware に

- ユーザーの指示（原文、2026-10-30）: 「P1: ODROIDで使うamlogicブートローダとMaliファームウェアは、zedBSD-firmwareリポジトリに追加してください。make menuconfigにファームウェアとして入れてください。当然他のファームウェアと同じでライセンス合意も必要です。ODROID-C5のBSPを選ぶと、Amlogicのこのチップのファームウェアが自動で依存関係で有効になるようにしてください。」
- zedBSD-firmware（branch agent/p1-c5-firmware、**local の commit 227a76db8e82c91ddd63e4017faa97b7e713b08e、親 fcdfccfa（GitHub の main）、push していない**）:
  - `mali-csf/`: linux-firmware dc85cced の `mali_csffw.bin`（282624、a27847ea…）・`LICENCE.mali_csffw`（9690、ebedc86d…）・`WHENCE`（425450、c282239a…）の無変更の写しと README.md。
  - `amlogic-s7d/`: mdrjr/meta-odroid-aml 548e2f037e の `u-boot.bin.signed`（3379200、8c2e6691…）と recipe `u-boot-odroid-c5.bb`（803、93711ea6…）、U-Boot 用の `GPL-2`（linux-firmware dc85cced、18092、8177f975…）、README.md（先頭に「License not confirmed」）。
  - top の README.md の表と `SHA256SUMS`（`sha256sum -c` OK）。
- license の調べ（GitHub の `git clone --depth 1` だけ。git.odroid.com・dn.odroid.com は触っていない）:
  - meta-odroid-aml 548e2f037e: repo に LICENSE なし。recipe は `LICENSE = "GPL-2.0-only"`（U-Boot）だけ。
  - github.com/hardkernel/u-boot の branch odroidc5-v2023.01（c99f3409d5）: submodule（git.odroid.com）だけで license の file なし。
  - Amlogic の他の SoC の FIP の blob を配る github.com/LibreELEC/amlogic-boot-fip（b6600f2857fdf81d11f18ea7a2e44aa989e7bcc4）の `LICENSE`（sha256 4255537f71b1cd0d487de0fa2b8eddf1eb96ed96d119507336dde8b3782b86e6）: 「All information contained herein is Amlogic confidential. This software is provided to you pursuant to Software License Agreement (SLA) with Amlogic Inc ("Amlogic"). … Redistribution and use in source and binary forms, with or without modification is strictly prohibited without prior written permission from Amlogic.」
  - 結論: S7D の blob の再配布を許す本文は見つからない。Amlogic が他の SoC に付ける条件は書面の許可なしの再配布を禁じる。**zedBSD-firmware の `amlogic-s7d/` の公開（push）はユーザーの判断が要る**（Q1 へ報告済み）。
- zedBSD（commit 9c09f68f4、親 7fe2d6328）:
  - package `amlogic-s7d-bootloader`（`userland/firmware/amlogic-s7d/`: Makefile・notice・manifest）。zedBSD-firmware の `amlogic-s7d/` を先に、だめなら upstream（mdrjr の raw と kernel.org の GPL-2）から取り、大きさと SHA-256 で検証。license の同意は他の firmware と同じ（notice と GPL-2 の本文）。rootfs には GPL-2・recipe・notice・manifest だけ（bootloader 自体は入れない）。
  - `config/drivers/firmware.list`: key が platform の名の行 `odroid-c5|amlogic-s7d-bootloader`（board を選ぶと選ばれ、別の board で外れる）。`CONFIG_DRIVER_MALI_GPU|mali-csf-firmware` は前から。
  - `tools/menuconfig.py`: ODROID-C5 の board（CPU arm64 の 2 つ目の board）。`driver_firmware` が platform の行を読む。
  - `config/drivers/architecture/arm64.drivers`: odroid-c5 の fixed の 4 行と `CONFIG_DRIVER_MALI_GPU`（既定 y、Pi と同じく board の Platform の driver は既定で有効。今まで catalog に無かった）。
  - `platform/arm64/tools/make-odroid-c5-hdd-image.py --bootloader FILE`（sector 1 から、MBR の code は 0、BOOTLOADER.txt に notice と「Written from」と区画の SHA-256）。`platform/arm64/odroid-c5.mk` は `ODROID_C5_BOOTLOADER_FROM`（明示の上書きとして残す）＞ package ＞ `ODROID_C5_NO_BOOTLOADER=y` の順。
  - `config/ci/config-odroid-c5.mk`・`config-odroid-c5-keiland.mk` の `ZEDBSD_USER_PROGRAMS` に `amlogic-s7d-bootloader`。
  - `tools/release/license-components.json` に `amlogic-s7d-bootloader`（`status: decision`、ユーザーの判断まで release の検査で止まる）。mali-csf の manifest に mirror の行。
  - `plan/tools/menuconfig-target-host-test.py`: main で既に FAIL だった Drivers の分類（Camera が増えた）を追従し、ODROID-C5 の試験（board を選ぶと bootloader と Mali の firmware、Mali の driver を切ると Mali の firmware だけ外れる、Pi に戻すと両方外れる、2 つの CI の config の読み込み）を足した。
- 確かめ（host）:
  - `make menuconfig-host-test`: PASS（firmware licenses PASS、round-trip PASS）。
  - `make ZEDBSD_CONFIG=config/ci/config-odroid-c5.mk AMLOGIC_S7D_BOOTLOADER_CACHE_ROOT=<worktree>/build/fw-sources/amlogic-s7d firmware-licenses-pending`: `amlogic-s7d-bootloader` の行が出て、GPL-2 は repo（fcdfccfa には無い）の 404 の後に kernel.org から取得・検証。
  - image の tool の単体（既存の build/odroid-c5 の vmunix・UFS で、`--bootloader <amlogic-s7d/u-boot.bin.signed>`）: MBR の code は 0・署名 55aa、sector 1〜6600 が blob（8c2e6691…）、その後 8 MiB まで 0、**区画（MBR の 440 byte＋sector 1 から）の SHA-256 は e6f020856acff8aedd041058971155e8bdb8bae6e3f8764e9818a20afc7b313d で q1709 の image と同じ**。3 つの bootloader の指定を 2 つ同時に渡すと拒否。
  - `make -n … BUILD=build/odroid-c5 build/odroid-c5/hdd-image.img`: 命令が `--bootloader <cache>/548e2f…/u-boot.bin.signed --notice userland/firmware/amlogic-s7d/amlogic-s7d-bootloader.notice` になる（rc=0）。
- **未実施**: package 経由の image の build（text の image）。build の前の license の自動の同意（`yes | make … firmware-licenses`）が権限の判定（ユーザーの代わりの同意）で拒まれ、回避せず止めた（Q1 へ報告）。ユーザーが端末で `firmware-licenses` に答えた後に、`BUILD=build/q1714-odroid-c5` で disk-image を作り、区画の SHA-256 を e6f02085… と照らす。
- 残り: zedBSD-firmware の push（ユーザーの判断。`amlogic-s7d/` は license 未確認）と、push の後に `userland/firmware/zedbsd-firmware.mk` の `ZEDBSD_FIRMWARE_REPOSITORY_REVISION` を 227a76db8e82c91ddd63e4017faa97b7e713b08e に（それまでは各 package が upstream から取る）、mali-csf と amlogic-s7d の manifest に `mirror-revision`。menuconfig が保存する config は `CONFIG_ODROID_C5_SERIAL_CONSOLE`（catalog に無い）を書かない（Pi の `CONFIG_RPI4_SERIAL_CONSOLE` と同じ。範囲の外）。

## q1716-i01（2026-10-30、P1）: bootloader を Hardkernel の公式の image から

- ユーザーの決定（原文、2026-10-30）: 「ODROID-C5のLinuxイメージからブートローダを取得するように変更します。このイメージはODROID-C5で使われるために許可を受けて配布されているわけですよね。だったらこのボードのための利用許可なので、このボードで利用する分にはOKと考えます。」「このファイルは、このボードで利用する目的であれば、再配布可能です。zedBSD-firmwareに入れてください。ライセンスは下記です。 prior written permissionはHardkernelに与えられており、このボードでしか使用できないイメージであれば、再配布が可能なんだそうです。」（Amlogic の license の本文つき）。その後（Q1 の中継）「だめです。36MBのイメージそのものをリポジトリに入れないと、再配布許可のある状態にできません。」
- 元の image: `/home/awe/ubuntu-22.04-gnome-desktop-odroidc5-20260610-bsp-boot.img.xz`（ユーザーが置いた公式の物、37760380 byte、SHA-256 04b9817d4e5e27ac4cc46dc4ad48fbc164e9a6c6f42eb1b0ba8d7a927accc5f2、展開で 545259520 byte）。MBR の code は 0・55aa、区画 1 つ（0x83、sector 16384 から）。sector 1〜6538 が bootloader（3347456 byte、SHA-256 6c5316ed320c78c9c2a67665cdee1089339be25bee625cf1eaffb0a6277c6562、BL30 `S7D-s905x5m-26052812`）、6539〜16383 は 0、sector 1〜16383 の SHA-256 は b6a4b424f0a6dac33be4287d2bad6f2651466d983ca9e4146a29357b8e3c8cba（P1 が `xz -dc | head -c 8388608` で確かめた）。
- zedBSD-firmware: `amlogic-s7d/` を公式の `.img.xz` そのもの（名前も変えず無変更）・`LICENSE.amlogic`（ユーザーが示した本文、1273 byte、5e8f7ad6…）・README（英語、ODROID-C5 で使う目的でだけ再配布、Hardkernel に Amlogic の書面の許可、U-Boot の GPL の source の出所）に。mdrjr の 3 file と切り出した `.bin` は外した。top の README の表と SHA256SUMS を直し `sha256sum -c` OK。P1 の commit（852123aa・f147461e）は Q1 が 1 つの commit **43662bd72419deb927af6d533d90fe72aa158c74**（親 fcdfccfa、tree は f147461e と同じ）にまとめた。push は Q1。
- zedBSD:
  - 1bf02b7fa: package `amlogic-s7d-bootloader` は `.img.xz` と `LICENSE.amlogic` を zedBSD-firmware の `amlogic-s7d/` から取り、size と SHA-256 で検証し、`xz -dc` で展開して sector 1〜6538 を切り出し、その size と SHA-256（6c5316ed…）も検証して cache に置く（cache の名は `odroidc5-20260610`）。image の tool は `--bootloader` で sector 1 から置く今の形のまま。上流の取得先は無い（公式の image は build が自動で取れない）ので、repo が使えない時は `build/sources/firmware/amlogic-s7d/by-hand/` に 2 file を手で置き、同じ検証を通す（license の本文も file:// でそこから読み、同意の質問は同じ）。notice・manifest・platform の `odroid-c5-bootloader.notice`（`ODROID_C5_BOOTLOADER_FROM` の時の notice、donor の例を公式の image に）・`tools/release/license-components.json`（status `decided`、ユーザーの決定を引用）・`userland/firmware/README.md`。
  - 40e11db10（revision だけ）: `userland/firmware/zedbsd-firmware.mk` の `ZEDBSD_FIRMWARE_REPOSITORY_REVISION` と mali-csf・amlogic の manifest の `mirror-revision` を 43662bd7…（push の前は各 package が upstream に fall back、amlogic は手の写しだけ）。
  - 966a7eac0（Q1 の指示）: U-Boot の `GPL-2` を戻した（linux-firmware dc85cced から、18092 byte、8177f975…。repo の `amlogic-s7d/` には無いので 404 の後 kernel.org）。rootfs の `/usr/share/licenses/amlogic-s7d-bootloader/` に `LICENSE.amlogic`・`GPL-2`・notice。notice に U-Boot の source（github.com/hardkernel/u-boot の branch odroidc5-v2023.01、c99f3409d5）。
- 確かめ（host、worktree の `build/tmp/q1716/`）:
  - fixture の goal（`amlogic-s7d-bootloader-fixture-cache`、license を聞かない）で by-hand の 2 file から cache を作る: rc=0、cache の 4 file（img.xz・LICENSE.amlogic・GPL-2・切り出し）の SHA-256 が上の値と一致。既存の cache の再検証 rc=0。repo の URL が 404（fcdfccfa）の時も rc=0（img.xz と LICENSE は手の写し、GPL-2 は kernel.org）。
  - 切り出しの範囲を 1 sector 縮めると size mismatch で rc=2、一時の directory は残らない。by-hand が空なら「no upstream … not placed by hand」で rc=2。
  - `firmware-licenses-pending`: repo の 404 の後、手の写しの `LICENSE.amlogic` を取って pending の行を出す（rc=0、同意は記録していない）。
  - `make -n … build/odroid-c5/hdd-image.img`（cache を fixture の物に）: `--bootloader …/odroidc5-20260610/odroidc5-20260610-bootloader.bin --notice userland/firmware/amlogic-s7d/amlogic-s7d-bootloader.notice`。
  - `make menuconfig-host-test`: rc=0（firmware licenses PASS、round-trip PASS）。
- 試験の image（`ODROID_C5_BOOTLOADER_FROM` の明示の上書き。公式の image の先頭 8 MiB を `build/tmp/q1716/odroidc5-20260610-head8m.img` に展開して渡す。parse の時の license の質問を出さないため、make の command line だけで `ZEDBSD_USER_PROGRAMS` から `amlogic-s7d-bootloader` を外した。config/ci は変えていない。`flock /tmp/zedbsd-image-build.lock make … disk-image`、mali は前の `build/fw-sources/mali-csf`。branch agent/p1-ws313 966a7eac0）:
  - text: `/home/awe/zedBSD-worktrees/p1-ws313/build/odroid-c5/hdd-image.img`、461373440 byte、sha256 0f8f56a14baf6ba5656d1b81b2d06a8cb782dde85e5650593f875cc8298c9fc5（rc=0、15:52）。
  - desktop: `/home/awe/zedBSD-worktrees/p1-ws313/build/odroid-c5-keiland/hdd-image.img`、464519168 byte、sha256 2cf1f64d468989c80a45bb46630f7292030286a463329f2287713a6b7496adfa（rc=0、16:22）。
  - 両方: sector 1〜16383 の SHA-256 が b6a4b424…（公式の image と一致）、sector 1〜6538 は 6c5316ed…、MBR の code は 0・55aa、区画 FAT（16384 から 524288 sector）と UFS（540672 から）、FAT に boot.scr・vmunix・odroid-c5.dtb・data.img・swapfile・BOOTLOADER.txt（新しい platform の notice、Copied from、区画の SHA-256 18621d41…）、`ufs-root.img: canonical zedBSD UFS OK`。warning は openssl・openssh・perl だけ（zedBSD の source は 0）。desktop の rootfs の `mali_csffw.bin` はこの回は見ていない（未確認）。
- 未実施: package 経由の image（ユーザーが端末で `make ZEDBSD_CONFIG=config/ci/config-odroid-c5.mk firmware-licenses` に答えた後。担当は代わりに答えない）。zedBSD-firmware の push の後の repo からの取得（43662bd7 は push 前で、raw の URL はまだ 404 の見込み）。実機。

