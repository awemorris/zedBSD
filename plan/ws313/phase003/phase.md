<!-- awesome-plan project=zedbsd record=ws313-p003 -->

# ws313-p003: 媒体と夜の試験の手順

Status: in-progress（実装済み・実機未試験）
Queue: q1675-i01・q1675-i02（P7）
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
