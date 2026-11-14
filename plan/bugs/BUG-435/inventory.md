<!-- awesome-plan project=zedbsd record=BUG-435-inventory -->

# BUG-435 の一覧: 古い build の変数を使う試験の道具と config

[BUG-435](../BUG-435.md) の第 1 段（2026-11-14 P2、q2079-i01、基点 main cc83f0a7fc）。

## 数え方

- 対象: `grep -rlE 'ZEDBSD_CONFIG|ZEDBSD_USER_PROGRAMS|ZEDBSD_TEST_EXTRA_FILES|disk-image|ZEDBSD_[A-Z_]+' plan --include=*.mk --include=*.sh --include=*.py`（`plan/history` を除く）の 340 file。
- 「古い名」は、今の Makefile・`mk/`（`mk/old-tools` を除く）・userland の Makefile が読まない名と target: `ZEDBSD_CONFIG=`（config の指定）・`ZEDBSD_USER_PROGRAMS`・`ZEDBSD_TEST_EXTRA_FILES`・`ZEDBSD_EXTRA_FILES`・`ZEDBSD_EXTRA_INPUTS`・`disk-image`・`ZEDBSD_PLATFORM`・`ZEDBSD_ARCHITECTURE`・`ZEDBSD_BOARD`・`ZEDBSD_MENU_VERSION`・`ZEDBSD_GRAPHICAL_BOOT`・`ZEDBSD_GRAPHICAL_LOGIN`・`ZEDBSD_BOOT_KERNEL_MESSAGES`・`ZEDBSD_BOOT_EXTRA_LINES`・`ZEDBSD_ROOTFS_*`・`ZEDBSD_KEILAND_WALLPAPERS`・`ZEDBSD_TEST_RC_CONF`・`ZEDBSD_TEST_CPPFLAGS`・`ZEDBSD_INSTALLER_TEST_*`・`ZEDBSD_PACKAGE_*` ほか。`ZEDBSD_VARIANT`（x86_64・i386 の `image.mk` が読む）・`ZEDBSD_JOBS`（`plan/tools/guest/jobs.sh`）・`ZEDBSD_TEST_SCREEN_CAPTURE`（wayland の Makefile）は今も有効なので古い名に数えない。
- 分け方（file ごとに機械で判定し、境目は目で見た）:
  - **A1**: `plan/tools/` の下（共有の道具）か、`plan/tools.md` に path が載る物。
  - **A2**: 自分の WS・Bug の外の記録（他の `plan/wsNNN/`・`plan/bugs/`・`plan/tools/`・`plan/tests.md`・`plan/uat`）が path で呼ぶ物、または Status が incomplete・planned・planning の WS の試験。
  - **B**: retired の directory、今の build に無い機種（m68k・mips・ppc・sun4u・x68k・zybo・armv7・riscv32・riscv64-virt・odroid・intelmac。今の PLATFORM は x86_64・rpi5・rpi4・vf2・i386・pc9821）、完了・取り下げの WS の物で他から呼ばれない物。
  - **N**: 古い名を使っていない（広い grep に掛かっただけ）。
- 数: **A1 83、A2 121、B 81、N 55**（古い名を使うのは 285）。この段で直したのは、表の中の 72 file（下の表の「この段」）と、表の外の 9 file（相対の include だけの config 5、`config-amd64-uat.mk`、fflush の手当ての 3）の計 81。

## 古い名と今の名

| 古い | 今 | 注 |
| --- | --- | --- |
| `make ZEDBSD_CONFIG=X` | `make CONFIGMK=X` | |
| target `disk-image` | `disk` | |
| `ZEDBSD_PLATFORM`・`ZEDBSD_ARCHITECTURE`・`ZEDBSD_BOARD` と `include config/ci/config-<機種>.mk` | `include config/config.<機種>.mk` と `include mk/platform-<機種>/defaults.mk` | amd64 → x86_64、pcat → i386、visionfive2 → vf2、pc98 → pc9821。`config/ci/` は全部が古い形（下の「範囲の外」） |
| `ZEDBSD_USER_PROGRAMS :=`・`+=` | `override CONFIG_USERLAND_PROGRAMS :=`・`+=` | 下の「config の書き方」 |
| `ZEDBSD_GRAPHICAL_BOOT` | `override CONFIG_BOOT_SPLASH` | |
| `ZEDBSD_GRAPHICAL_LOGIN` | `override CONFIG_GRAPHICAL_LOGIN` | |
| `ZEDBSD_BOOT_KERNEL_MESSAGES := y`・`n` | `override CONFIG_QUIET_BOOT := n`・`y` | 意味が逆 |
| `ZEDBSD_ROOTFS_DEVELOPMENT` | `override CONFIG_ROOTFS_DEVELOPMENT` | |
| `CONFIG_X := v`（試験の config） | `override CONFIG_X := v` | platform の defaults.mk が config の後に `:=` で入れるため |
| `ZEDBSD_TEST_EXTRA_FILES`・`ZEDBSD_EXTRA_FILES` | `ROOTFS_EXTRA_FILES` | `ZEDBSD_EXTRA_INPUTS` は要らない（`ROOTFS_EXTRA_INPUTS` が自動） |
| `ZEDBSD_TEST_CPPFLAGS :=` | `CONFIG_CPPFLAGS +=` | |
| `ZEDBSD_KEILAND_WALLPAPERS` | `KEILAND_WALLPAPERS` | |
| package の名 `libgl`・`keiland-ime`・`ime-dict-ja`・`ime-dict-skk`・`keiland-preview`・`keiland-printd`・`libwayland-client` | `libGL`・`ime`・`dict`・`skk-dict`・`preview`・`printd`・`libwayland` | 選択は package の directory の名で、知らない名は黙って無視される。`Xzed` は package が無いので外した |
| `ZEDBSD_BOOT_EXTRA_LINES`・`ZEDBSD_TEST_RC_CONF`・`ZEDBSD_INSTALLER_TEST_*`・`ZEDBSD_ROOT_LOCKED`・`ZEDBSD_PACKAGE_*` | **無い** | 下の「今の build に無い入口」 |

## config の書き方（この段で揃えた形）

1. **include は tree の絶対 path で**。package の入れ子の make は package の directory で走り、`STANDALONE_CONFIG`（config の絶対 path）を読む。config の中の `include plan/…` は相対なので、入れ子の make では「No such file or directory」で止まる（この段の最初の build で `userland/base/cat` が止まった）。各 config の先頭で `TEST_TREE := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)`（深さは file の位置で）を作り、`include $(TEST_TREE)/…` と書く（WS353 の config と同じ形）。**古い名を使わない config でも、相対の include を持つ物は同じ理由で壊れている**（`plan/` の下に 131 file。この段で直したのは下の表の物と `plan/tools/posix/config-amd64-serial.mk`・`plan/tools/rtld/config-amd64-rtld.mk`・`plan/tools/powervr/p008/config-visionfive2-compute.mk`・`plan/tools/aat/config-amd64-notes-pen.mk`・`plan/tools/libm/config-amd64-libm.mk`）。
2. **基の config は platform の defaults.mk も読む**。top の Makefile は config の後に `mk/platform-<機種>/defaults.mk` を読み、そこで `CONFIG_*` を `:=`、`CONFIG_USERLAND_PROGRAMS` を `+=` で入れる。試験の config が全部の program を基に足す・外す（`+=`・`filter-out`）には、先に defaults.mk を読んでから `override` で書く。`override` の後の defaults.mk の 2 度目の `+=` は効かない。
3. 小さな image（boot の確かめなど）は defaults.mk を読まず `override CONFIG_USERLAND_PROGRAMS := …` で全部を書く（`mk/userland.mk` が sh・init・networkd・ifconfig・route・dhcpc などを必ず足す）。
4. `override CONFIG_USERLAND_PROGRAMS += $(filter-out $(CONFIG_USERLAND_PROGRAMS),…)` は「Recursive variable」で止まる。重なりは `$(sort)` で消えるので、ただ `+=` で足す。

## この段の確かめ

- 直した config の全部（65 file）を `make -pn CONFIGMK=<file> BUILD=build/bug435-parse help` で parse した。全部 rc 0、PLATFORM が決まり（prompt にならない）、`CONFIG_USERLAND_PROGRAMS` が期待の大きさ（boot の image は 20 語、全部の userland は 180〜264 語）。
- `sh plan/tools/process/forkrace-qemu.sh amd64 build/bug435-fr build/bug435-fr-out2`（`plan/tools/boot-images/config-amd64-boot.mk` を include する `config-amd64-forkrace.mk` を `make -j4 BUILD_JOBS=4 CONFIGMK=… BUILD=… disk` で作り、`plan/tools/boot-test.sh`（uefi-usb）で起動し、login して forkrace を走らせる）: `forkrace-qemu: PASS amd64`。build の log の `warning:` は 0。boot-test は画面で login・Password・root@・`FORKRACE PASS` を見た（`build/bug435-fr-out2/boot/login-after.png`、P2 が目でも見た）。
- `sh plan/tools/installer/host/ufs-read-test.sh`（`make -s CONFIGMK=config/config.x86_64.mk build/x86_64/zedimage-host` に直した）: `ufs-read-test: PASS`。
- 未実施: 直した他の config の build（parse だけ）、rpi4・i386・vf2 の image、QEMU の rpi4（console-end・forkrace の rpi4）、実機の道具（i915-hw・gpu-compute/hw・hw5330・rtl8127・nvrtx など）。

## 今の build に無い入口（Q1 へ。Makefile・mk/ の判断が要る）

1. **`ZEDBSD_BOOT_EXTRA_LINES`**（zedbsd.cfg に試験の行を足す）: 今の `mk/platform-x86_64/image.mk` は splash・graphical login・quiet だけを書く。使う物: `plan/tools/nvrtx/config-amd64-nvrtx.mk`（RUN ごとの nvrtx.* の行、20 か所）、`plan/tools/i915-hw/hdmi-h2-hw.sh`、`plan/ws031/tests/hw-machine.sh`、`plan/ws075/demo/`（display=edp）ほか。案の方向: `BOOT_EXTRA_LINES` を `$(BUILD)/zedbsd.cfg` の規則で足し、stamp に入れる（x86_64・i386）。
2. **`ZEDBSD_TEST_RC_CONF`**（試験の rc.conf）: `ROOTFS_EXTRA_FILES` の `--file /etc/rc.conf=…` で足りる見込み（rootfs.mk は一覧を最後に入れる）。`plan/tools/gpu-compute/hw/g3-hw.sh`・`gles-hw.sh` を直す時に替える。
3. **installer の image**（`ZEDBSD_INSTALLER_TEST_FILES`・`_UNITS`・`_OMIT`・`_ITEMS`）: 今の Makefile・`mk/` に installer の image の target そのものが無い（`grep -rn installer mk Makefile` が 0）。`plan/tools/installer/config-amd64-p009.mk`・`p010.mk` と、それを使う試験は動かない。
4. **`ZEDBSD_ROOT_LOCKED`**（root の password を錠にしない）: 今は無い。`plan/tools/boot-time/config-amd64-boottime.mk` が使う。
5. **`config/current-uat.mk`** が無い（UAT の config）。`plan/tools/aat/config-amd64-uat.mk` は x86_64 の platform の config と defaults で代えた（AAT の image の全部がこれを基にする）。UAT の config を戻すか、これでよいかはユーザーの判断。

## 範囲の外で見つけた事（直していない）

- **`config/ci/` の全部（19 file）が古い形**（`ZEDBSD_PLATFORM := amd64` など）。今の Makefile では PLATFORM が空になり、対話の prompt で止まる。`config/ci/` を include する `plan/` の config は、A2・B に多く残る。`config/ci/config-amd64-server.mk` はユーザーの物。`config/release/config-amd64-beta2.mk` も古い形。
- **`mk/` の古い名の残り**（Makefile・`mk/` は変えない規則なので案だけ）:
  - `mk/platform-x86_64/image.mk:37`・`mk/platform-i386/image.mk:31` が `VARIANT := $(if $(ZEDBSD_VARIANT),$(ZEDBSD_VARIANT),uefi)`（i386 は bios）で、config の `VARIANT`（`config/config.x86_64.mk` の `VARIANT := uefi`）を読まず上書きする。`mk/platform-i386/bootloader.mk:3` も `ZEDBSD_VARIANT`。
  - `mk/libc.mk:84`・`87` の `crt0-$(ZEDBSD_ARCHITECTURE).S`・`crt1-…`: `ZEDBSD_ARCHITECTURE` はどこでも定義されない（`CRT0_ASM`・`CRT1_ASM` が使われていなければ害は無い。未確かめ）。
  - `mk/platform-x86_64/image.mk:68` の `ZEDBSD_CONFIG := $(BUILD)/zedbsd.cfg`（boot の cfg の path）: 古い試験が command line で `ZEDBSD_CONFIG=<config.mk>` を渡すと、この変数を上書きして config の .mk を zedbsd.cfg として image に入れる。古い道具を流すと黙って壊れた image ができる。
  - firmware の package の Makefile（`userland/firmware/*/Makefile`、13 file）と `userland/desktop/wallpapers/Makefile` が `ZEDBSD_USER_PROGRAMS` で選択を見る（`CONFIG_USERLAND_PROGRAMS` を見ない）。firmware の license の問い（`ZEDBSD_FIRMWARE_ASK_AT_PARSE`）が今の選択で働くかは未確かめ。
- WS353 の config（`plan/ws353/tests/config-amd64-answer.mk` ほか）は `libwayland-client` を選ぶが、その package は無い（`libwayland`）。黙って無視される。

## 残り（次の段）

- A1 のうち直していない物: `plan/tools/gpu-compute/hw/`（`g3-hw.sh`・`gles-hw.sh`・`run-hw.sh`、`/home/awe/zedBSD-rpi4` の noct と 5330 を前提）、`plan/tools/i915-hw/`（`ZEDBSD_CONFIG` を環境で下の script に渡す連鎖、`plan/ws031/tests/config-zdesktop-hw.mk` が基）、`plan/tools/menuconfig-target-host-test.py`（menuconfig は `mk/old-tools`）、`plan/tools/packages/run-cross-host-test.sh`（`ZEDBSD_PLATFORM_DIR`）、`plan/tools/guest/test-image.sh`（`disk-image` を `disk` と読む互換だけ。`ROOTFS_EXTRA_FILES` が入ったので、root に後から写す今の仕組みを `ROOTFS_EXTRA_FILES` に替えられる）、tools.md に載る `plan/ws031/tests/hw-machine.sh`・`plan/ws075/demo/build-demo-image.sh`・`plan/ws125/tests/subtree-host-test.sh`・`plan/ws206/tests/bcm43455-firmware-package-test.sh`。
- A2 の 121: 各 WS・Bug の担当が、その WS を再開する時に上の形で直す（この段では `plan/ws035/tests/config-amd64-userland.mk`（guest の SSH の image の基）・`plan/ws221/tests/config-amd64-tcp.mk`（tools.md に登録）・`plan/ws075/demo/config-demo-hdmi.mk`（gpu-compute の demo の基）だけ直した）。
- 相対の include を持つ config（古い名が無い物を含め 131）の `TEST_TREE` 化。
- tools.md の書き換え（Q1 の file）: 道具の行の `make ZEDBSD_CONFIG=… disk-image` の例を `make CONFIGMK=… disk` に（boot-images・rpi4・console-end・process・ports の行ほか）。

## tools.md の scanf の行の案（Q1 へ）

今の行は `make -f src/libc/libc.mk scanf-host-test` を指すが、`src/libc/libc.mk` は無く、今の Makefile・`mk/` に `scanf-host-test` の target も無い。host の cc で直に作って流せる（2026-11-14 P2 が流して `scanf host test: PASS`、ASan・UBSan でも PASS）。案:

> | tests/scanf-host-test.c（2026-10-31、BUG-344） | libc の scanf 系（stream に読み残しを戻す、`%[a-z]` の範囲、`%zu`・`%jd`・`%td` の幅、空の入力で EOF） | `cc -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Dsscanf=zed_sscanf -Dfscanf=zed_fscanf -Dscanf=zed_scanf -Dvsscanf=zed_vsscanf -Dvfscanf=zed_vfscanf -Dvscanf=zed_vscanf -o build/tmp/scanf-host-test tests/scanf-host-test.c src/libc/scan.c -lm && build/tmp/scanf-host-test`（最後の行 `scanf host test: PASS`。`-fsanitize=address,undefined` でも PASS） |

注: gcc は `scan.c:236` に `-Wnonnull-compare` の warning を 1 つ出す（host の `<stdio.h>` の `vfscanf` の nonnull の属性が名を替えた関数に付くため。zedBSD の build では出ない）。

## BUG-432 の後の fflush(NULL) の手当て（この段で外した）

`plan/ws333/tests/hw5330/static-tool.sh`・`thermal-static.sh`、`plan/bugs/BUG-424/netsleep/build.sh` の、main を `tool_main` に名を替えて `fflush(NULL)` してから返す main を足す手当てを外した（static の `crt0.o` は BUG-432 の直しで main の後に `exit` を呼び、`exit` が stdio を flush する）。確かめは下の「経過」。

## 経過

- 2026-11-14 P2: 一覧を作り分けた。最初の forkrace の build は、config の相対の include が package の入れ子の make で読めず止まった（`userland/base/cat`）。include を `TEST_TREE` の絶対 path にして 2 度目で PASS。
- 見つけた事（直していない）: 最小の image（boot の確かめ）で、init が `/sbin/volumed`・`bluetoothd`・`audiod`・`ntpdate` を exec して ENOENT になり、何度も起こし直す（画面に出続ける）。init の service の一覧が、選ばれていない program の service も入れている。BUG-430 の観測 6（networkd）と同じ種類で、`mk/userland.mk` の必ず入れる一覧か、init の service の data の選び方の問題。
- fflush の手当てを外した 3 本: `build/bug435-fr/sysroot` に向けて流し（script の `sr` を置き換えて stdin から）、`-Werror` で compile・link が通った（thermal・netsleep・sysctl）。`llvm-objdump` で `_start` が main の後に `exit` を呼ぶ事を確かめた。guest・実機（5330）での出力の確かめは未実施（BUG-432 は QEMU で確かめ済み）。

## 一覧

### A1: 共有の道具（plan/tools/）と tools.md に登録された回帰試験（83）

| file | 古い名 | 根拠（誰が呼ぶか） | この段 |
| --- | --- | --- | --- |
| `plan/tools/aat/config-amd64-aat-bugs.mk` | ZEDBSD_USER_PROGRAMS | plan/tools の共有の道具 | 直した |
| `plan/tools/aat/config-amd64-aat-dnd.mk` | ZEDBSD_USER_PROGRAMS | plan/tools の共有の道具 | 直した |
| `plan/tools/aat/config-amd64-aat-menu.mk` | ZEDBSD_USER_PROGRAMS | plan/tools の共有の道具 | 直した |
| `plan/tools/aat/config-amd64-aat-touch.mk` | ZEDBSD_USER_PROGRAMS | plan/tools の共有の道具、呼ぶ: plan/ws190 | 直した |
| `plan/tools/aat/config-amd64-aat.mk` | ZEDBSD_USER_PROGRAMS | plan/tools の共有の道具、呼ぶ: plan/bugs/BUG-264.md plan/bugs/BUG-370 plan/bugs/BUG-376 plan/ws143 plan/ws158 plan/ws175 plan/ws217 plan/ws223 | 直した |
| `plan/tools/aat/config-rpi5-aat.mk` | ZEDBSD_CONFIG ZEDBSD_USER_PROGRAMS | plan/tools の共有の道具、tools.md に登録、呼ぶ: plan/bugs/BUG-412.md | 直した |
| `plan/tools/accounts/config-amd64-users.mk` | ZEDBSD_USER_PROGRAMS | plan/tools の共有の道具 | 直した |
| `plan/tools/boot-images/config-amd64-boot.mk` | ZEDBSD_CONFIG ZEDBSD_GRAPHICAL_BOOT ZEDBSD_GRAPHICAL_LOGIN ZEDBSD_USER_PROGRAMS disk-image | plan/tools の共有の道具、呼ぶ: plan/tools/process plan/ws331 plan/ws333 | 直した |
| `plan/tools/boot-images/config-pcat-boot.mk` | ZEDBSD_CONFIG ZEDBSD_GRAPHICAL_BOOT ZEDBSD_GRAPHICAL_LOGIN ZEDBSD_USER_PROGRAMS disk-image | plan/tools の共有の道具、呼ぶ: plan/tools/process | 直した |
| `plan/tools/boot-images/config-rpi4-boot.mk` | ZEDBSD_CONFIG ZEDBSD_GRAPHICAL_BOOT ZEDBSD_GRAPHICAL_LOGIN ZEDBSD_USER_PROGRAMS disk-image | plan/tools の共有の道具、呼ぶ: plan/tools/console-end plan/ws331 | 直した |
| `plan/tools/boot-keys/config-amd64-keys.mk` | ZEDBSD_BOOT_KERNEL_MESSAGES ZEDBSD_GRAPHICAL_BOOT | plan/tools の共有の道具 | 直した |
| `plan/tools/boot-time/config-amd64-boottime.mk` | ZEDBSD_BOOT_KERNEL_MESSAGES ZEDBSD_USER_PROGRAMS | plan/tools の共有の道具、呼ぶ: plan/bugs/BUG-394.md | 直した |
| `plan/tools/console-end/config-rpi4-end.mk` | ZEDBSD_CONFIG ZEDBSD_TEST_CPPFLAGS disk-image | plan/tools の共有の道具、tools.md に登録、呼ぶ: plan/ws331 | 直した |
| `plan/tools/console-end/config-rpi4-serial.mk` | ZEDBSD_CONFIG disk-image | plan/tools の共有の道具、呼ぶ: plan/bugs/BUG-418.md plan/tools/process | 直した |
| `plan/tools/console-end/config-rpi4-text.mk` | ZEDBSD_CONFIG ZEDBSD_TEST_CPPFLAGS disk-image | plan/tools の共有の道具 | 直した |
| `plan/tools/console-end/config-visionfive2-end.mk` | ZEDBSD_CONFIG ZEDBSD_TEST_CPPFLAGS | plan/tools の共有の道具 | 直した |
| `plan/tools/console-end/end-rpi4-qemu.sh` | ZEDBSD_CONFIG disk-image | plan/tools の共有の道具、tools.md に登録 | 直した |
| `plan/tools/files/config-amd64-files-ime.mk` | ZEDBSD_USER_PROGRAMS | plan/tools の共有の道具、呼ぶ: plan/bugs/BUG-146.md | 直した |
| `plan/tools/files/config-amd64-files.mk` | ZEDBSD_USER_PROGRAMS | plan/tools の共有の道具、呼ぶ: plan/bugs/BUG-146.md plan/tools/ime plan/ws035 plan/ws071 plan/ws074 plan/ws079 plan/ws089 plan/ws090 plan/ws095 plan/ws127 plan/ws128 plan/ws130 plan/ws134 plan/ws143 | 直した |
| `plan/tools/gnu-utils/config-amd64-base.mk` | ZEDBSD_ARCHITECTURE ZEDBSD_BOARD ZEDBSD_MENU_VERSION ZEDBSD_PLATFORM ZEDBSD_USER_PROGRAMS | plan/tools の共有の道具、tools.md に登録、呼ぶ: plan/bugs/BUG-347.md plan/tools/libm plan/tools/posix plan/ws048 plan/ws049 plan/ws073 | 直した |
| `plan/tools/gpu-boundary/build-forge-image.sh` | ZEDBSD_KEILAND_WALLPAPERS | plan/tools の共有の道具、呼ぶ: plan/tools/keiland-linux plan/ws291 | 直した |
| `plan/tools/gpu-boundary/config-amd64-forge.mk` | ZEDBSD_USER_PROGRAMS | plan/tools の共有の道具、呼ぶ: plan/ws309 | 直した |
| `plan/tools/gpu-compute/demo/config.mk` | ZEDBSD_USER_PROGRAMS | plan/tools の共有の道具、呼ぶ: plan/ws068 plan/ws101 | 直した |
| `plan/tools/gpu-compute/gles/config-amd64-compute.mk` | ZEDBSD_USER_PROGRAMS | plan/tools の共有の道具、呼ぶ: plan/ws101 | 直した |
| `plan/tools/gpu-compute/hw/g3-hw.sh` | ZEDBSD_CONFIG ZEDBSD_TEST_EXTRA_FILES ZEDBSD_TEST_RC_CONF disk-image | plan/tools の共有の道具、tools.md に登録、呼ぶ: plan/tools/hw5330 plan/ws101 plan/ws233 |  |
| `plan/tools/gpu-compute/hw/g3/config.mk` | ZEDBSD_USER_PROGRAMS | plan/tools の共有の道具、呼ぶ: plan/ws068 plan/ws101 | 直した |
| `plan/tools/gpu-compute/hw/gles-hw.sh` | ZEDBSD_CONFIG ZEDBSD_TEST_EXTRA_FILES ZEDBSD_TEST_RC_CONF disk-image | plan/tools の共有の道具、tools.md に登録、呼ぶ: plan/tools/hw5330 plan/ws068 plan/ws101 |  |
| `plan/tools/gpu-compute/hw/gles/config.mk` | ZEDBSD_USER_PROGRAMS | plan/tools の共有の道具、呼ぶ: plan/ws068 plan/ws101 | 直した |
| `plan/tools/gpu-compute/hw/run-hw.sh` | ZEDBSD_CONFIG | plan/tools の共有の道具、tools.md に登録、呼ぶ: plan/tools/hw5330 plan/tools/hwsv7 plan/ws031 plan/ws101 |  |
| `plan/tools/guest/config-amd64-full.mk` | ZEDBSD_GRAPHICAL_BOOT ZEDBSD_USER_PROGRAMS | plan/tools の共有の道具 | 直した |
| `plan/tools/guest/config-amd64-ssh.mk` | ZEDBSD_CONFIG ZEDBSD_USER_PROGRAMS disk-image | plan/tools の共有の道具、呼ぶ: plan/bugs/BUG-307.md plan/bugs/BUG-363.md plan/bugs/BUG-430-makefile.md plan/tools/boot-keys plan/tools/installer plan/tools/ipv6-frag plan/tools/powervr plan/tools/rtld plan/tools/webp plan/ws033 plan/ws052 plan/ws063 plan/ws073 plan/ws220 plan/ws221 plan/ws255 plan/ws335 plan/ws336 | 直した |
| `plan/tools/guest/test-image.sh` | disk-image | plan/tools の共有の道具、tools.md に登録、呼ぶ: plan/bugs/BUG-158.md plan/bugs/BUG-171.md plan/bugs/BUG-203.md plan/bugs/BUG-205.md plan/bugs/BUG-298.md plan/bugs/BUG-304.md plan/bugs/BUG-369.md plan/bugs/BUG-394.md plan/bugs/BUG-409.md plan/bugs/BUG-410.md plan/bugs/BUG-416 plan/bugs/BUG-418.md plan/bugs/BUG-430-makefile.md plan/bugs/BUG-430.md plan/tools/aat plan/tools/accounts plan/tools/boot-keys plan/tools/boot-time plan/tools/files plan/tools/gpu-boundary plan/tools/gpu-compute plan/tools/ime plan/tools/installer plan/tools/ipv6-frag plan/tools/nvrtx plan/tools/rtld plan/tools/rtx-video plan/tools/tcp-close plan/tools/titlebar plan/tools/webp plan/ws031 plan/ws033 plan/ws035 plan/ws037 plan/ws039 plan/ws049 plan/ws052 plan/ws068 plan/ws073 plan/ws074 plan/ws075 plan/ws079 plan/ws089 plan/ws095 plan/ws099 plan/ws100 plan/ws115 plan/ws126 plan/ws128 plan/ws130 plan/ws136 plan/ws142 plan/ws143 plan/ws158 plan/ws172 plan/ws202 plan/ws220 plan/ws221 plan/ws233 plan/ws255 plan/ws288 plan/ws290 plan/ws305 plan/ws309 plan/ws314 plan/ws334 plan/ws335 plan/ws336 plan/ws345 plan/ws353 |  |
| `plan/tools/hw5330/config-5330.mk` | ZEDBSD_CONFIG ZEDBSD_USER_PROGRAMS disk-image | plan/tools の共有の道具、呼ぶ: plan/bugs/BUG-382.md plan/tools/rtl815x plan/ws221 plan/ws282 plan/ws291 plan/ws298 plan/ws299 plan/ws319 plan/ws333 | 直した |
| `plan/tools/i915-hw/hdmi-h1-hw.sh` | ZEDBSD_CONFIG | plan/tools の共有の道具、tools.md に登録、呼ぶ: plan/tools/hw5330 plan/ws079 plan/ws245 |  |
| `plan/tools/i915-hw/hdmi-h2-hw.sh` | ZEDBSD_BOOT_EXTRA_LINES ZEDBSD_CONFIG | plan/tools の共有の道具、呼ぶ: plan/tools/hw5330 |  |
| `plan/tools/i915-hw/test-hw.sh` | ZEDBSD_CONFIG | plan/tools の共有の道具、tools.md に登録、呼ぶ: plan/tools/gpu-compute plan/tools/hw5330 plan/tools/hwsv7 plan/ws031 plan/ws051 plan/ws075 plan/ws101 plan/ws202 |  |
| `plan/tools/ime/config-amd64-languages.mk` | ZEDBSD_USER_PROGRAMS | plan/tools の共有の道具、tools.md に登録、呼ぶ: plan/ws158 | 直した |
| `plan/tools/installer/config-amd64-p007.mk` | ZEDBSD_USER_PROGRAMS | plan/tools の共有の道具 | 直した |
| `plan/tools/installer/config-amd64-p009.mk` | ZEDBSD_INSTALLER_TEST_FILES ZEDBSD_INSTALLER_TEST_UNITS | plan/tools の共有の道具、tools.md に登録 | 直した |
| `plan/tools/installer/config-amd64-p010.mk` | ZEDBSD_INSTALLER_TEST_FILES ZEDBSD_INSTALLER_TEST_ITEMS ZEDBSD_INSTALLER_TEST_OMIT ZEDBSD_INSTALLER_TEST_UNITS | plan/tools の共有の道具 | 直した |
| `plan/tools/installer/host/ufs-read-test.sh` | ZEDBSD_CONFIG | plan/tools の共有の道具 | 直した |
| `plan/tools/ipv6-frag/config-amd64-frag.mk` | ZEDBSD_USER_PROGRAMS | plan/tools の共有の道具 | 直した |
| `plan/tools/kstress/qemu-credrace.sh` | ZEDBSD_EXTRA_FILES | plan/tools の共有の道具 | 直した |
| `plan/tools/kstress/qemu-iostress.sh` | ZEDBSD_EXTRA_FILES | plan/tools の共有の道具 | 直した |
| `plan/tools/kstress/qemu-nvme-errors.sh` | ZEDBSD_EXTRA_FILES | plan/tools の共有の道具 | 直した |
| `plan/tools/kstress/qemu-nvme-timeout.sh` | ZEDBSD_EXTRA_FILES | plan/tools の共有の道具 | 直した |
| `plan/tools/kstress/qemu-sigexec.sh` | ZEDBSD_EXTRA_FILES | plan/tools の共有の道具 | 直した |
| `plan/tools/latency/config-amd64-bench.mk` | ZEDBSD_EXTRA_FILES ZEDBSD_EXTRA_INPUTS ZEDBSD_USER_PROGRAMS | plan/tools の共有の道具 | 直した |
| `plan/tools/latency/config-pc98-bench.mk` | ZEDBSD_USER_PROGRAMS | plan/tools の共有の道具 | 直した |
| `plan/tools/libm/browser-js.sh` | ZEDBSD_CONFIG ZEDBSD_TEST_EXTRA_FILES disk-image | plan/tools の共有の道具、呼ぶ: plan/bugs/BUG-078.md plan/ws233 | 直した |
| `plan/tools/libm/config-amd64-browser-libm.mk` | ZEDBSD_USER_PROGRAMS | plan/tools の共有の道具 | 直した |
| `plan/tools/libm/guest-test.sh` | ZEDBSD_CONFIG ZEDBSD_TEST_EXTRA_FILES disk-image | plan/tools の共有の道具、tools.md に登録、呼ぶ: plan/bugs/BUG-078.md plan/ws073 plan/ws233 | 直した |
| `plan/tools/menuconfig-target-host-test.py` | ZEDBSD_ARCHITECTURE ZEDBSD_BOARD ZEDBSD_BOOT_KERNEL_MESSAGES ZEDBSD_CONFIG ZEDBSD_GRAPHICAL_BOOT ZEDBSD_GRAPHICAL_LOGIN ZEDBSD_MENU_VERSION ZEDBSD_PLATFORM ZEDBSD_USER_PROGRAMS disk-image | plan/tools の共有の道具、tools.md に登録、呼ぶ: plan/bugs/BUG-129.md plan/bugs/BUG-377.md plan/ws073 plan/ws074 plan/ws178 plan/ws215 plan/ws222 plan/ws233 plan/ws313 plan/ws321 |  |
| `plan/tools/nvrtx/config-amd64-nvrtx.mk` | ZEDBSD_BOOT_EXTRA_LINES ZEDBSD_BOOT_KERNEL_MESSAGES ZEDBSD_GRAPHICAL_BOOT ZEDBSD_GRAPHICAL_LOGIN ZEDBSD_USER_PROGRAMS | plan/tools の共有の道具、呼ぶ: plan/ws037 plan/ws241 plan/ws242 | 直した |
| `plan/tools/packages/run-cross-host-test.sh` | ZEDBSD_PLATFORM_DIR | plan/tools の共有の道具 |  |
| `plan/tools/ports/build-vmunix-all.sh` | ZEDBSD_CONFIG | plan/tools の共有の道具、tools.md に登録、呼ぶ: plan/ws331 plan/ws333 | 直した |
| `plan/tools/powervr/p008/config-visionfive2-coherency.mk` | ZEDBSD_USER_PROGRAMS | plan/tools の共有の道具 | 直した |
| `plan/tools/powervr/p009/config-amd64-vkcompute.mk` | ZEDBSD_USER_PROGRAMS | plan/tools の共有の道具 | 直した |
| `plan/tools/powervr/p009/config-visionfive2-vkcompute.mk` | ZEDBSD_CONFIG ZEDBSD_USER_PROGRAMS disk-image | plan/tools の共有の道具 | 直した |
| `plan/tools/powervr/p010/config-amd64-vkstage.mk` | ZEDBSD_CONFIG ZEDBSD_USER_PROGRAMS disk-image | plan/tools の共有の道具 | 直した |
| `plan/tools/powervr/p010/config-visionfive2-selftest.mk` | ZEDBSD_CONFIG ZEDBSD_USER_PROGRAMS disk-image | plan/tools の共有の道具、呼ぶ: plan/ws269 | 直した |
| `plan/tools/process/config-amd64-forkrace.mk` | ZEDBSD_CONFIG ZEDBSD_USER_PROGRAMS disk-image | plan/tools の共有の道具 | 直した |
| `plan/tools/process/config-pcat-forkrace.mk` | ZEDBSD_CONFIG ZEDBSD_USER_PROGRAMS disk-image | plan/tools の共有の道具 | 直した |
| `plan/tools/process/config-rpi4-forkrace.mk` | ZEDBSD_CONFIG ZEDBSD_USER_PROGRAMS disk-image | plan/tools の共有の道具 | 直した |
| `plan/tools/process/forkrace-qemu.sh` | ZEDBSD_CONFIG disk-image | plan/tools の共有の道具、tools.md に登録、呼ぶ: plan/bugs/BUG-379.md plan/ws291 | 直した |
| `plan/tools/rpi4/config-rpi4-sd.mk` | ZEDBSD_USER_PROGRAMS | plan/tools の共有の道具、tools.md に登録、呼ぶ: plan/tools/boot-images plan/ws289 | 直した |
| `plan/tools/rtl8127/config-amd64-rtl8127-perf.mk` | ZEDBSD_CONFIG ZEDBSD_USER_PROGRAMS | plan/tools の共有の道具 | 直した |
| `plan/tools/rtl8127/config-amd64-rtl8127-profile.mk` | ZEDBSD_CONFIG | plan/tools の共有の道具 | 直した |
| `plan/tools/rtl8127/config-amd64-rtl8127.mk` | ZEDBSD_CONFIG | plan/tools の共有の道具、呼ぶ: plan/ws221 | 直した |
| `plan/tools/rtl815x/config-5330-rtl.mk` | ZEDBSD_CONFIG ZEDBSD_USER_PROGRAMS disk-image | plan/tools の共有の道具、呼ぶ: plan/ws315 | 直した |
| `plan/tools/rtx-video/config-amd64-rtx-inject.mk` | ZEDBSD_USER_PROGRAMS | plan/tools の共有の道具 | 直した |
| `plan/tools/rtx-video/config-amd64-venus-video.mk` | ZEDBSD_USER_PROGRAMS | plan/tools の共有の道具、呼ぶ: plan/ws291 | 直した |
| `plan/tools/settings/config-amd64-settings.mk` | ZEDBSD_USER_PROGRAMS | plan/tools の共有の道具、呼ぶ: plan/bugs/BUG-171.md plan/ws089 | 直した |
| `plan/tools/titlebar/config-amd64-menu.mk` | ZEDBSD_USER_PROGRAMS | plan/tools の共有の道具、呼ぶ: plan/tools/aat plan/tools/files plan/tools/rtx-video plan/ws035 plan/ws070 plan/ws071 plan/ws079 plan/ws090 plan/ws314 | 直した |
| `plan/tools/vi/config-amd64-console.mk` | ZEDBSD_GRAPHICAL_BOOT ZEDBSD_GRAPHICAL_LOGIN | plan/tools の共有の道具 | 直した |
| `plan/tools/vi/config-amd64-vi.mk` | ZEDBSD_USER_PROGRAMS | plan/tools の共有の道具、呼ぶ: plan/bugs/BUG-307.md | 直した |
| `plan/tools/webp/platform/config-amd64-webp.mk` | ZEDBSD_USER_PROGRAMS | plan/tools の共有の道具 | 直した |
| `plan/ws031/tests/hw-machine.sh` | ZEDBSD_BOOT_EXTRA_LINES | tools.md に登録、呼ぶ: plan/tools/gpu-compute plan/tools/hw5330 plan/tools/hwsv7 plan/tools/i915-hw plan/ws233 |  |
| `plan/ws035/tests/build-login-image.sh` | disk-image | tools.md に登録、呼ぶ: plan/bugs/BUG-392.md plan/tools/sessiond plan/ws099 plan/ws172 plan/ws233 | 直した |
| `plan/ws075/demo/build-demo-image.sh` | ZEDBSD_BOOT_EXTRA_LINES ZEDBSD_KEILAND_WALLPAPERS ZEDBSD_TEST_CPPFLAGS | tools.md に登録、呼ぶ: plan/bugs/BUG-058.md plan/bugs/BUG-094.md plan/tools/gpu-compute plan/tools/hw5330 plan/tools/i915-hw plan/ws035 plan/ws079 plan/ws084 plan/ws085 plan/ws089 plan/ws090 plan/ws094 plan/ws099 plan/ws100 plan/ws101 plan/ws102 plan/ws128 plan/ws133 plan/ws233 |  |
| `plan/ws125/tests/subtree-host-test.sh` | ZEDBSD_PACKAGE_FILES ZEDBSD_PACKAGE_INPUTS ZEDBSD_PACKAGE_LINKS ZEDBSD_ROOTFS_CONFIG_STAMP ZEDBSD_ROOTFS_DEVELOPMENT ZEDBSD_ROOTFS_DIRECTORIES ZEDBSD_ROOTFS_INSTALL_DEVELOPMENT ZEDBSD_ROOTFS_STICKY_DIRECTORIES ZEDBSD_ROOTFS_TREE_RULE | tools.md に登録 |  |
| `plan/ws206/tests/bcm43455-firmware-package-test.sh` | ZEDBSD_ARCHITECTURE ZEDBSD_BOARD ZEDBSD_CONFIG ZEDBSD_PLATFORM ZEDBSD_USERLAND_DATA_INPUTS ZEDBSD_USER_PROGRAMS | tools.md に登録、呼ぶ: plan/ws233 |  |
| `plan/ws221/tests/config-amd64-tcp.mk` | ZEDBSD_USER_PROGRAMS | tools.md に登録、呼ぶ: plan/bugs/BUG-281.md plan/bugs/BUG-308.md plan/bugs/BUG-367.md plan/bugs/BUG-369.md plan/tools/tcp-close plan/ws336 | 直した |

### A2: 他の WS・Bug の記録や script が呼ぶ、または開いている WS の試験（121）

| file | 古い名 | 根拠（誰が呼ぶか） | この段 |
| --- | --- | --- | --- |
| `plan/bugs/BUG-370/config-amd64-aat.mk` | ZEDBSD_USER_PROGRAMS | 他の記録が呼ぶ: plan/bugs/BUG-376 |  |
| `plan/bugs/BUG-376/config-amd64-ps2.mk` | ZEDBSD_USER_PROGRAMS | 他の記録が呼ぶ: plan/bugs/BUG-370.md plan/bugs/BUG-376.md |  |
| `plan/bugs/BUG-416/config-amd64.mk` | ZEDBSD_GRAPHICAL_BOOT ZEDBSD_GRAPHICAL_LOGIN ZEDBSD_USER_PROGRAMS | 他の記録が呼ぶ: plan/bugs/BUG-412.md plan/bugs/BUG-416.md |  |
| `plan/uat/config-uat.mk` | ZEDBSD_USER_PROGRAMS | 他の記録が呼ぶ: plan/bugs/BUG-158.md plan/bugs/BUG-165.md plan/bugs/BUG-166.md plan/bugs/BUG-167.md plan/bugs/BUG-168.md plan/bugs/BUG-169.md plan/bugs/BUG-170.md plan/bugs/BUG-171.md plan/bugs/BUG-172.md plan/bugs/BUG-173.md plan/bugs/BUG-174.md plan/bugs/BUG-175.md plan/bugs/BUG-176.md plan/bugs/BUG-202.md plan/ws049 plan/ws073 plan/ws075 |  |
| `plan/ws001/tests/config-amd64-lean-guest.mk` | ZEDBSD_USER_PROGRAMS | 他の記録が呼ぶ: plan/ws005（ws001 は incomplete） |  |
| `plan/ws001/tests/credential-vfs-qemu-config.mk` | ZEDBSD_ARCHITECTURE ZEDBSD_BOARD ZEDBSD_MENU_VERSION ZEDBSD_PLATFORM | ws001 は incomplete、その WS の試験 |  |
| `plan/ws001/tests/credential-vfs-qemu.mk` | ZEDBSD_ARCH_UFS_IMAGE_RULE ZEDBSD_CONFIG ZEDBSD_PLATFORM_DIR | ws001 は incomplete、その WS の試験 |  |
| `plan/ws001/tests/guest-run.sh` | ZEDBSD_CONFIG ZEDBSD_TEST_EXTRA_FILES disk-image | 他の記録が呼ぶ: plan/tools/utils plan/ws233（ws001 は incomplete） |  |
| `plan/ws005/tests/run-peercred-native-qemu.sh` | ZEDBSD_ARCHITECTURE ZEDBSD_BOARD ZEDBSD_PLATFORM | 他の記録が呼ぶ: plan/ws034（ws005 は completed） |  |
| `plan/ws014/tests/config-venus-amd64.mk` | ZEDBSD_ARCHITECTURE ZEDBSD_BOARD ZEDBSD_MENU_VERSION ZEDBSD_PLATFORM ZEDBSD_USER_PROGRAMS | 他の記録が呼ぶ: plan/ws035（ws014 は closed） |  |
| `plan/ws014/tests/run-gpu-build-selection-test.py` | ZEDBSD_ARCHITECTURE ZEDBSD_BOARD ZEDBSD_CONFIG ZEDBSD_PLATFORM | 他の記録が呼ぶ: plan/ws035（ws014 は closed） |  |
| `plan/ws014/tests/run-venus-remote.py` | ZEDBSD_CONFIG disk-image | 他の記録が呼ぶ: plan/ws029 plan/ws034 plan/ws274（ws014 は closed） |  |
| `plan/ws031/tests/config-zdesktop-hw.mk` | ZEDBSD_USER_PROGRAMS | 他の記録が呼ぶ: plan/tools/gpu-compute plan/tools/hw5330 plan/tools/i915-hw plan/ws035 plan/ws068 plan/ws202（ws031 は completed） |  |
| `plan/ws031/tests/vkloop-hw.sh` | ZEDBSD_BOOT_EXTRA_LINES ZEDBSD_CONFIG ZEDBSD_TEST_CPPFLAGS ZEDBSD_TEST_RC_CONF disk-image | 他の記録が呼ぶ: plan/bugs/BUG-056.md plan/bugs/BUG-057.md plan/tools/gpu-compute plan/tools/hw5330 plan/tools/hwsv7 plan/tools/i915-hw plan/tools/titlebar plan/ws014 plan/ws035 plan/ws068 plan/ws075 plan/ws078 plan/ws101（ws031 は completed） |  |
| `plan/ws035/demo/build-demo-image.sh` | ZEDBSD_TEST_RC_CONF | 他の記録が呼ぶ: plan/tools/gpu-compute plan/tools/hw5330 plan/ws075 plan/ws079 plan/ws085 plan/ws089 plan/ws090 plan/ws094 plan/ws099 plan/ws100 plan/ws101 plan/ws233（ws035 は completed） |  |
| `plan/ws035/tests/config-amd64-audiod.mk` | ZEDBSD_EXTRA_FILES ZEDBSD_EXTRA_INPUTS ZEDBSD_USER_PROGRAMS | 他の記録が呼ぶ: plan/ws100（ws035 は completed） |  |
| `plan/ws035/tests/config-amd64-graphical.mk` | ZEDBSD_GRAPHICAL_BOOT | 他の記録が呼ぶ: plan/ws099 plan/ws172 plan/ws290（ws035 は completed） |  |
| `plan/ws035/tests/config-amd64-guest.mk` | ZEDBSD_USER_PROGRAMS | 他の記録が呼ぶ: plan/ws034 plan/ws073 plan/ws314（ws035 は completed） |  |
| `plan/ws035/tests/config-amd64-hda-image.mk` | ZEDBSD_EXTRA_FILES ZEDBSD_EXTRA_INPUTS | 他の記録が呼ぶ: plan/ws034（ws035 は completed） |  |
| `plan/ws035/tests/config-amd64-userland.mk` | ZEDBSD_ARCHITECTURE ZEDBSD_BOARD ZEDBSD_GRAPHICAL_BOOT ZEDBSD_MENU_VERSION ZEDBSD_PLATFORM ZEDBSD_USER_PROGRAMS | 他の記録が呼ぶ: plan/bugs/BUG-134.md plan/bugs/BUG-430-makefile.md plan/tools/guest plan/tools/latency plan/tools/titlebar plan/ws031 plan/ws034 plan/ws100 plan/ws202 plan/ws336（ws035 は completed） | 直した |
| `plan/ws035/tests/config-amd64-zdesktop.mk` | ZEDBSD_CONFIG ZEDBSD_USER_PROGRAMS disk-image | 他の記録が呼ぶ: plan/bugs/BUG-144.md plan/bugs/BUG-208.md plan/bugs/BUG-242.md plan/bugs/BUG-248.md plan/bugs/BUG-252.md plan/bugs/BUG-255.md plan/ws001 plan/ws052 plan/ws068 plan/ws073 plan/ws092 plan/ws100 plan/ws102 plan/ws131 plan/ws142 plan/ws172 plan/ws195 plan/ws333（ws035 は completed） |  |
| `plan/ws039/tests/config.mk` | ZEDBSD_BOOT_EXTRA_LINES ZEDBSD_BOOT_KERNEL_MESSAGES ZEDBSD_CONFIG ZEDBSD_GRAPHICAL_BOOT ZEDBSD_GRAPHICAL_LOGIN ZEDBSD_USER_PROGRAMS | 他の記録が呼ぶ: plan/ws255 plan/ws291（ws039 は completed） |  |
| `plan/ws052/tests/config-amd64-sleep.mk` | ZEDBSD_USER_PROGRAMS | 他の記録が呼ぶ: plan/ws197（ws052 は incomplete） |  |
| `plan/ws068/tests/config-amd64-glsl.mk` | ZEDBSD_USER_PROGRAMS | 他の記録が呼ぶ: plan/tools/gpu-compute（ws068 は incomplete） |  |
| `plan/ws073/tests/bug084.sh` | ZEDBSD_CONFIG ZEDBSD_PACKAGE_INPUTS | 他の記録が呼ぶ: plan/bugs/BUG-084.md（ws073 は completed） |  |
| `plan/ws073/tests/bug087.sh` | ZEDBSD_CONFIG ZEDBSD_PACKAGE_INPUTS | 他の記録が呼ぶ: plan/bugs/BUG-087.md（ws073 は completed） |  |
| `plan/ws074/tests/config-amd64-browser.mk` | ZEDBSD_USER_PROGRAMS | 他の記録が呼ぶ: plan/ws314 plan/ws334 plan/ws345（ws074 は incomplete） |  |
| `plan/ws075/demo/config-demo-hdmi.mk` | ZEDBSD_BOOT_EXTRA_LINES ZEDBSD_KEILAND_WALLPAPERS ZEDBSD_USER_PROGRAMS | 他の記録が呼ぶ: plan/tools/gpu-compute plan/tools/hw5330 plan/uat plan/ws035 plan/ws079 plan/ws084 plan/ws089 plan/ws099 plan/ws100 plan/ws101 plan/ws129（ws075 は completed） | 直した |
| `plan/ws079/tests/config-amd64-pen.mk` | ZEDBSD_USER_PROGRAMS | 他の記録が呼ぶ: plan/bugs/BUG-111.md plan/tools/desktop-guest plan/tools/imageview plan/ws089 plan/ws099 plan/ws142（ws079 は completed） |  |
| `plan/ws089/tests/build-settings-image.sh` | ZEDBSD_KEILAND_WALLPAPERS | 他の記録が呼ぶ: plan/bugs/BUG-413.md plan/tools/keiland-linux plan/tools/settings plan/ws005 plan/ws128（ws089 は completed） |  |
| `plan/ws089/tests/config-amd64-settings-ime.mk` | ZEDBSD_USER_PROGRAMS | 他の記録が呼ぶ: plan/tools/settings（ws089 は completed） |  |
| `plan/ws089/tests/config-amd64-settings-touch.mk` | ZEDBSD_USER_PROGRAMS | 他の記録が呼ぶ: plan/ws128 plan/ws134（ws089 は completed） |  |
| `plan/ws089/tests/config-amd64-settings.mk` | ZEDBSD_KEILAND_WALLPAPERS ZEDBSD_USER_PROGRAMS | 他の記録が呼ぶ: plan/bugs/BUG-146.md plan/bugs/BUG-171.md plan/tools/settings plan/ws014 plan/ws100 plan/ws127 plan/ws128（ws089 は completed） |  |
| `plan/ws099/tests/build-criteria-image.sh` | ZEDBSD_KEILAND_WALLPAPERS | 他の記録が呼ぶ: plan/tools/keiland-linux plan/ws073 plan/ws078 plan/ws085 plan/ws102 plan/ws309（ws099 は completed） |  |
| `plan/ws099/tests/config-amd64-criteria.mk` | ZEDBSD_KEILAND_WALLPAPERS ZEDBSD_USER_PROGRAMS | 他の記録が呼ぶ: plan/tools/gpu-boundary plan/ws078 plan/ws100 plan/ws115（ws099 は completed） |  |
| `plan/ws100/tests/config-amd64-audiod.mk` | ZEDBSD_EXTRA_FILES ZEDBSD_EXTRA_INPUTS ZEDBSD_USER_PROGRAMS | 他の記録が呼ぶ: plan/ws191（ws100 は completed） |  |
| `plan/ws100/tests/config-amd64-volume.mk` | ZEDBSD_EXTRA_FILES ZEDBSD_EXTRA_INPUTS ZEDBSD_USER_PROGRAMS | 他の記録が呼ぶ: plan/ws078 plan/ws128（ws100 は completed） |  |
| `plan/ws115/tests/config-amd64-glib.mk` | ZEDBSD_USER_PROGRAMS | ws115 は incomplete、その WS の試験 |  |
| `plan/ws115/tests/config-amd64-gtk.mk` | ZEDBSD_CONFIG ZEDBSD_USER_PROGRAMS | ws115 は incomplete、その WS の試験 |  |
| `plan/ws115/tests/config-amd64-image.mk` | ZEDBSD_USER_PROGRAMS | ws115 は incomplete、その WS の試験 |  |
| `plan/ws115/tests/config-amd64-pango.mk` | ZEDBSD_USER_PROGRAMS | ws115 は incomplete、その WS の試験 |  |
| `plan/ws115/tests/config-amd64-text.mk` | ZEDBSD_USER_PROGRAMS | ws115 は incomplete、その WS の試験 |  |
| `plan/ws115/tests/glib-probe.sh` | ZEDBSD_TEST_EXTRA_FILES | ws115 は incomplete、その WS の試験 |  |
| `plan/ws115/tests/image-probe.sh` | ZEDBSD_TEST_EXTRA_FILES | ws115 は incomplete、その WS の試験 |  |
| `plan/ws115/tests/meson-package-probe.mk` | ZEDBSD_ARCHITECTURE ZEDBSD_PROBE_MAKEFILE ZEDBSD_STANDALONE_CONFIG | ws115 は incomplete、その WS の試験 |  |
| `plan/ws115/tests/pango-probe.sh` | ZEDBSD_TEST_EXTRA_FILES | ws115 は incomplete、その WS の試験 |  |
| `plan/ws115/tests/text-probe.sh` | ZEDBSD_TEST_EXTRA_FILES | ws115 は incomplete、その WS の試験 |  |
| `plan/ws115/tests/xdg-probe.sh` | ZEDBSD_TEST_EXTRA_FILES | ws115 は incomplete、その WS の試験 |  |
| `plan/ws126/tests/config-amd64-python3.mk` | ZEDBSD_EXTRA_FILES ZEDBSD_EXTRA_INPUTS ZEDBSD_USER_PROGRAMS | ws126 は incomplete、その WS の試験 |  |
| `plan/ws128/tests/config-amd64-imageview.mk` | ZEDBSD_USER_PROGRAMS | 他の記録が呼ぶ: plan/ws127（ws128 は completed） |  |
| `plan/ws143/tests/config-amd64-bt.mk` | ZEDBSD_USER_PROGRAMS | 他の記録が呼ぶ: plan/ws197（ws143 は completed） |  |
| `plan/ws143/tests/intelbt-firmware-package-test.sh` | ZEDBSD_ARCHITECTURE ZEDBSD_BOARD ZEDBSD_CONFIG ZEDBSD_PLATFORM ZEDBSD_USERLAND_DATA_INPUTS ZEDBSD_USER_PROGRAMS | 他の記録が呼ぶ: plan/ws233（ws143 は completed） |  |
| `plan/ws172/tests/build-fido2-image.sh` | disk-image | ws172 は incomplete、その WS の試験 |  |
| `plan/ws172/tests/build-passkey-image.sh` | disk-image | ws172 は incomplete、その WS の試験 |  |
| `plan/ws172/tests/config-amd64-fido2.mk` | ZEDBSD_USER_PROGRAMS | ws172 は incomplete、その WS の試験 |  |
| `plan/ws172/tests/config-amd64-passkey.mk` | ZEDBSD_USER_PROGRAMS | ws172 は incomplete、その WS の試験 |  |
| `plan/ws197/tests/config-amd64-sco.mk` | ZEDBSD_USER_PROGRAMS | ws197 は incomplete、その WS の試験 |  |
| `plan/ws202/tests/config-media.mk` | ZEDBSD_USER_PROGRAMS | 他の記録が呼ぶ: plan/bugs/BUG-300.md（ws202 は completed） |  |
| `plan/ws204/tests/config-amd64-audio.mk` | ZEDBSD_USER_PROGRAMS | ws204 は incomplete、その WS の試験 |  |
| `plan/ws215/tests/brcmbt-firmware-package-test.sh` | ZEDBSD_ARCHITECTURE ZEDBSD_BOARD ZEDBSD_CONFIG ZEDBSD_PLATFORM ZEDBSD_USERLAND_DATA_INPUTS ZEDBSD_USER_PROGRAMS | 他の記録が呼ぶ: plan/ws233（ws215 は completed） |  |
| `plan/ws217/tests/config-amd64.mk` | ZEDBSD_USER_PROGRAMS | 他の記録が呼ぶ: plan/bugs/BUG-306.md plan/bugs/BUG-313.md plan/bugs/BUG-412.md plan/ws309（ws217 は incomplete） |  |
| `plan/ws220/tests/config-amd64-tcp.mk` | ZEDBSD_USER_PROGRAMS | 他の記録が呼ぶ: plan/bugs/BUG-298.md plan/bugs/BUG-367.md plan/bugs/BUG-369.md（ws220 は completed） |  |
| `plan/ws221/tests/config-5330-jumbo.mk` | ZEDBSD_CONFIG ZEDBSD_USER_PROGRAMS disk-image | ws221 は incomplete、その WS の試験 |  |
| `plan/ws221/tests/config-amd64-idle.mk` | ZEDBSD_USER_PROGRAMS | ws221 は incomplete、その WS の試験 |  |
| `plan/ws221/tests/config-amd64-jumbo.mk` | ZEDBSD_USER_PROGRAMS | ws221 は incomplete、その WS の試験 |  |
| `plan/ws221/tests/config-amd64-usb.mk` | ZEDBSD_USER_PROGRAMS | ws221 は incomplete、その WS の試験 |  |
| `plan/ws221/tests/config-vf2-tcp.mk` | ZEDBSD_CONFIG ZEDBSD_USER_PROGRAMS | ws221 は incomplete、その WS の試験 |  |
| `plan/ws282/tests/config-rpi4.mk` | ZEDBSD_USER_PROGRAMS | ws282 は incomplete、その WS の試験 |  |
| `plan/ws282/tests/config-rpi5.mk` | ZEDBSD_USER_PROGRAMS | 他の記録が呼ぶ: plan/bugs/BUG-412.md（ws282 は incomplete） |  |
| `plan/ws282/tests/config-visionfive2.mk` | ZEDBSD_USER_PROGRAMS | ws282 は incomplete、その WS の試験 |  |
| `plan/ws282/tests/config.mk` | ZEDBSD_USER_PROGRAMS | ws282 は incomplete、その WS の試験 |  |
| `plan/ws284/tests/config-rpi5.mk` | ZEDBSD_USER_PROGRAMS | ws284 は incomplete、その WS の試験 |  |
| `plan/ws284/tests/config.mk` | ZEDBSD_USER_PROGRAMS | ws284 は incomplete、その WS の試験 |  |
| `plan/ws288/tests/config-amd64-ghci.mk` | ZEDBSD_BOOT_KERNEL_MESSAGES ZEDBSD_GRAPHICAL_BOOT ZEDBSD_USER_PROGRAMS | 他の記録が呼ぶ: plan/ws290 plan/ws347（ws288 は closed） |  |
| `plan/ws288/tests/config-visionfive2-ghci.mk` | ZEDBSD_BOOT_KERNEL_MESSAGES ZEDBSD_CONFIG ZEDBSD_USER_PROGRAMS | 他の記録が呼ぶ: plan/ws290 plan/ws347（ws288 は closed） |  |
| `plan/ws290/tests/config-amd64-ghci.mk` | ZEDBSD_BOOT_KERNEL_MESSAGES ZEDBSD_CONFIG ZEDBSD_GRAPHICAL_BOOT ZEDBSD_USER_PROGRAMS | 他の記録が呼ぶ: plan/ws347（ws290 は incomplete） |  |
| `plan/ws290/tests/config-amd64-text-end-gpu.mk` | ZEDBSD_BOOT_KERNEL_MESSAGES | ws290 は incomplete、その WS の試験 |  |
| `plan/ws290/tests/config-amd64-text-end.mk` | ZEDBSD_BOOT_KERNEL_MESSAGES ZEDBSD_GRAPHICAL_BOOT ZEDBSD_GRAPHICAL_LOGIN ZEDBSD_TEST_CPPFLAGS | ws290 は incomplete、その WS の試験 |  |
| `plan/ws290/tests/config-visionfive2-ghci.mk` | ZEDBSD_BOOT_KERNEL_MESSAGES ZEDBSD_CONFIG ZEDBSD_USER_PROGRAMS | 他の記録が呼ぶ: plan/ws347（ws290 は incomplete） |  |
| `plan/ws291/tests/config-5330.mk` | ZEDBSD_CONFIG ZEDBSD_EXTRA_FILES ZEDBSD_EXTRA_INPUTS ZEDBSD_USER_PROGRAMS disk-image | 他の記録が呼ぶ: plan/ws333（ws291 は incomplete） |  |
| `plan/ws291/tests/config-rpi4-desk.mk` | ZEDBSD_USER_PROGRAMS | ws291 は incomplete、その WS の試験 |  |
| `plan/ws291/tests/config-rpi4-shot.mk` | ZEDBSD_USER_PROGRAMS | ws291 は incomplete、その WS の試験 |  |
| `plan/ws291/tests/config-rpi5-desk.mk` | ZEDBSD_USER_PROGRAMS | ws291 は incomplete、その WS の試験 |  |
| `plan/ws291/tests/config-rpi5-shot.mk` | ZEDBSD_USER_PROGRAMS | ws291 は incomplete、その WS の試験 |  |
| `plan/ws291/tests/config-rpi5.mk` | ZEDBSD_EXTRA_FILES ZEDBSD_EXTRA_INPUTS ZEDBSD_GRAPHICAL_LOGIN ZEDBSD_USER_PROGRAMS | 他の記録が呼ぶ: plan/bugs/BUG-387.md plan/bugs/BUG-418.md plan/ws309（ws291 は incomplete） |  |
| `plan/ws291/tests/config-vf2-shot.mk` | ZEDBSD_USER_PROGRAMS | 他の記録が呼ぶ: plan/ws322（ws291 は incomplete） |  |
| `plan/ws291/tests/config-vf2.mk` | ZEDBSD_USER_PROGRAMS | 他の記録が呼ぶ: plan/bugs/BUG-340.md plan/bugs/BUG-342.md plan/bugs/BUG-375.md plan/ws309（ws291 は incomplete） |  |
| `plan/ws291/tests/config.mk` | ZEDBSD_USER_PROGRAMS | 他の記録が呼ぶ: plan/ws305 plan/ws309（ws291 は incomplete） |  |
| `plan/ws291/tests/nowc-build.sh` | ZEDBSD_CONFIG | ws291 は incomplete、その WS の試験 |  |
| `plan/ws291/tests/rpi5-image.sh` | ZEDBSD_CONFIG disk-image | 他の記録が呼ぶ: plan/ws309（ws291 は incomplete） |  |
| `plan/ws291/tests/rtx-cursor-view.sh` | ZEDBSD_CONFIG | 他の記録が呼ぶ: plan/bugs/BUG-374.md（ws291 は incomplete） |  |
| `plan/ws299/tests/config-5330-cache128.mk` | ZEDBSD_CONFIG | ws299 は incomplete、その WS の試験 |  |
| `plan/ws299/tests/config-qemu-cache128.mk` | ZEDBSD_CONFIG disk-image | ws299 は incomplete、その WS の試験 |  |
| `plan/ws299/tests/config-qemu-cache32.mk` | ZEDBSD_CONFIG disk-image | ws299 は incomplete、その WS の試験 |  |
| `plan/ws299/tests/qemu-direct-reboot.sh` | ZEDBSD_EXTRA_FILES | ws299 は incomplete、その WS の試験 |  |
| `plan/ws299/tests/qemu-directio.sh` | ZEDBSD_EXTRA_FILES | ws299 は incomplete、その WS の試験 |  |
| `plan/ws299/tests/qemu-qdprobe.sh` | ZEDBSD_BOOT_EXTRA_LINES ZEDBSD_EXTRA_FILES | ws299 は incomplete、その WS の試験 |  |
| `plan/ws305/tests/config.mk` | ZEDBSD_BOOT_EXTRA_LINES ZEDBSD_USER_PROGRAMS | ws305 は incomplete、その WS の試験 |  |
| `plan/ws309/tests/config-amd64-p1.mk` | ZEDBSD_USER_PROGRAMS | ws309 は incomplete、その WS の試験 |  |
| `plan/ws309/tests/config-rpi4-p015.mk` | ZEDBSD_CONFIG ZEDBSD_EXTRA_FILES ZEDBSD_EXTRA_INPUTS ZEDBSD_USER_PROGRAMS disk-image | 他の記録が呼ぶ: plan/bugs/BUG-378.md plan/ws291（ws309 は incomplete） |  |
| `plan/ws309/tests/regress-guest.sh` | ZEDBSD_CONFIG | ws309 は incomplete、その WS の試験 |  |
| `plan/ws309/tests/spawn-closefrom/run.sh` | ZEDBSD_CONFIG | ws309 は incomplete、その WS の試験 |  |
| `plan/ws313/tests/c5-build-check.sh` | ZEDBSD_CONFIG | ws313 は incomplete、その WS の試験 |  |
| `plan/ws314/tests/build-zedbsd.sh` | ZEDBSD_CONFIG | 他の記録が呼ぶ: plan/ws327（ws314 は completed） |  |
| `plan/ws314/tests/config-amd64-browser.mk` | ZEDBSD_USER_PROGRAMS | 他の記録が呼ぶ: plan/ws345（ws314 は completed） |  |
| `plan/ws314/tests/doc-examples.sh` | ZEDBSD_CONFIG ZEDBSD_TARGET_SYSROOT | 他の記録が呼ぶ: plan/ws327（ws314 は completed） |  |
| `plan/ws321/tests/config-rpi5-hevc.mk` | ZEDBSD_CONFIG ZEDBSD_USER_PROGRAMS | ws321 は incomplete、その WS の試験 |  |
| `plan/ws322/tests/config-vf2-video-test.mk` | ZEDBSD_CONFIG ZEDBSD_TEST_CPPFLAGS | ws322 は incomplete、その WS の試験 |  |
| `plan/ws322/tests/config-vf2-video.mk` | ZEDBSD_CONFIG ZEDBSD_USER_PROGRAMS | ws322 は incomplete、その WS の試験 |  |
| `plan/ws322/tests/config-visionfive2-vpu-probe.mk` | ZEDBSD_CONFIG | ws322 は incomplete、その WS の試験 |  |
| `plan/ws322/tests/config-visionfive2-vpu.mk` | ZEDBSD_CONFIG | ws322 は incomplete、その WS の試験 |  |
| `plan/ws334/tests/config-amd64-p027-guest.mk` | ZEDBSD_USER_PROGRAMS | ws334 は incomplete、その WS の試験 |  |
| `plan/ws334/tests/sources-same.sh` | ZEDBSD_TOPLEVEL_BUILD | ws334 は incomplete、その WS の試験 |  |
| `plan/ws336/tests/config-amd64-ci-ssh.mk` | ZEDBSD_USER_PROGRAMS | ws336 は incomplete、その WS の試験 |  |
| `plan/ws336/tests/config-amd64-ssh.mk` | ZEDBSD_USER_PROGRAMS | ws336 は incomplete、その WS の試験 |  |
| `plan/ws336/tests/config-amd64-tcp-ssh.mk` | ZEDBSD_USER_PROGRAMS | ws336 は incomplete、その WS の試験 |  |
| `plan/ws336/tests/config-arm64-ssh.mk` | ZEDBSD_USER_PROGRAMS | ws336 は incomplete、その WS の試験 |  |
| `plan/ws336/tests/config-riscv64-ssh.mk` | ZEDBSD_USER_PROGRAMS | ws336 は incomplete、その WS の試験 |  |
| `plan/ws336/tests/config-rpi5-ssh.mk` | ZEDBSD_USER_PROGRAMS | ws336 は incomplete、その WS の試験 |  |
| `plan/ws336/tests/replace-check.sh` | ZEDBSD_CONFIG disk-image | ws336 は incomplete、その WS の試験 |  |
| `plan/ws343/tests/config-amd64-compat.mk` | ZEDBSD_USER_PROGRAMS | ws343 は incomplete、その WS の試験 | 消した（2026-11-15、WS343 の完了の時。今の main で動かない開発の時だけの試験） |

### B: 開発の時だけ・retired・完了した WS の一回きり・今の build に無い機種（81）

| file | 古い名 | 根拠（誰が呼ぶか） | この段 |
| --- | --- | --- | --- |
| `plan/agents/T1/files/config-t1-552.mk` | ZEDBSD_USER_PROGRAMS | 呼ぶ記録が無い、他から呼ばれない |  |
| `plan/tools/boot-images/config-sun4u-boot.mk` | ZEDBSD_CONFIG ZEDBSD_GRAPHICAL_BOOT ZEDBSD_GRAPHICAL_LOGIN ZEDBSD_USER_PROGRAMS disk-image | その機種が今の build に無い（PLATFORM は x86_64・rpi5・rpi4・vf2・i386・pc9821） |  |
| `plan/tools/ports/config-m68k-virt.mk` | ZEDBSD_ARCHITECTURE ZEDBSD_BOARD ZEDBSD_BOOT_KERNEL_MESSAGES ZEDBSD_GRAPHICAL_BOOT ZEDBSD_GRAPHICAL_LOGIN ZEDBSD_MENU_VERSION ZEDBSD_PLATFORM ZEDBSD_ROOTFS_DEVELOPMENT ZEDBSD_USER_PROGRAMS | その機種が今の build に無い（PLATFORM は x86_64・rpi5・rpi4・vf2・i386・pc9821） |  |
| `plan/tools/ports/config-mips32-malta.mk` | ZEDBSD_ARCHITECTURE ZEDBSD_BOARD ZEDBSD_BOOT_KERNEL_MESSAGES ZEDBSD_GRAPHICAL_BOOT ZEDBSD_GRAPHICAL_LOGIN ZEDBSD_MENU_VERSION ZEDBSD_PLATFORM ZEDBSD_ROOTFS_DEVELOPMENT ZEDBSD_USER_PROGRAMS | その機種が今の build に無い（PLATFORM は x86_64・rpi5・rpi4・vf2・i386・pc9821） |  |
| `plan/tools/ports/config-mips64-malta.mk` | ZEDBSD_ARCHITECTURE ZEDBSD_BOARD ZEDBSD_BOOT_KERNEL_MESSAGES ZEDBSD_GRAPHICAL_BOOT ZEDBSD_GRAPHICAL_LOGIN ZEDBSD_MENU_VERSION ZEDBSD_PLATFORM ZEDBSD_ROOTFS_DEVELOPMENT ZEDBSD_USER_PROGRAMS | その機種が今の build に無い（PLATFORM は x86_64・rpi5・rpi4・vf2・i386・pc9821） |  |
| `plan/tools/ports/config-ppc32-mac99.mk` | ZEDBSD_ARCHITECTURE ZEDBSD_BOARD ZEDBSD_BOOT_KERNEL_MESSAGES ZEDBSD_GRAPHICAL_BOOT ZEDBSD_GRAPHICAL_LOGIN ZEDBSD_MENU_VERSION ZEDBSD_PLATFORM ZEDBSD_ROOTFS_DEVELOPMENT ZEDBSD_USER_PROGRAMS | その機種が今の build に無い（PLATFORM は x86_64・rpi5・rpi4・vf2・i386・pc9821） |  |
| `plan/tools/ports/config-ppc64-pseries.mk` | ZEDBSD_ARCHITECTURE ZEDBSD_BOARD ZEDBSD_BOOT_KERNEL_MESSAGES ZEDBSD_GRAPHICAL_BOOT ZEDBSD_GRAPHICAL_LOGIN ZEDBSD_MENU_VERSION ZEDBSD_PLATFORM ZEDBSD_ROOTFS_DEVELOPMENT ZEDBSD_USER_PROGRAMS | その機種が今の build に無い（PLATFORM は x86_64・rpi5・rpi4・vf2・i386・pc9821） |  |
| `plan/tools/ports/config-riscv32-virt.mk` | ZEDBSD_ARCHITECTURE ZEDBSD_BOARD ZEDBSD_BOOT_KERNEL_MESSAGES ZEDBSD_GRAPHICAL_BOOT ZEDBSD_GRAPHICAL_LOGIN ZEDBSD_MENU_VERSION ZEDBSD_PLATFORM ZEDBSD_ROOTFS_DEVELOPMENT ZEDBSD_USER_PROGRAMS | その機種が今の build に無い（PLATFORM は x86_64・rpi5・rpi4・vf2・i386・pc9821） |  |
| `plan/tools/ports/config-sun4u.mk` | ZEDBSD_ARCHITECTURE ZEDBSD_BOARD ZEDBSD_BOOT_KERNEL_MESSAGES ZEDBSD_GRAPHICAL_BOOT ZEDBSD_GRAPHICAL_LOGIN ZEDBSD_MENU_VERSION ZEDBSD_PLATFORM ZEDBSD_ROOTFS_DEVELOPMENT ZEDBSD_USER_PROGRAMS | その機種が今の build に無い（PLATFORM は x86_64・rpi5・rpi4・vf2・i386・pc9821） |  |
| `plan/tools/ports/config-x68k.mk` | ZEDBSD_ARCHITECTURE ZEDBSD_BOARD ZEDBSD_BOOT_KERNEL_MESSAGES ZEDBSD_GRAPHICAL_BOOT ZEDBSD_GRAPHICAL_LOGIN ZEDBSD_MENU_VERSION ZEDBSD_PLATFORM ZEDBSD_ROOTFS_DEVELOPMENT ZEDBSD_USER_PROGRAMS | その機種が今の build に無い（PLATFORM は x86_64・rpi5・rpi4・vf2・i386・pc9821） |  |
| `plan/tools/process/config-riscv64-forkrace.mk` | ZEDBSD_CONFIG ZEDBSD_USER_PROGRAMS | その機種が今の build に無い（PLATFORM は x86_64・rpi5・rpi4・vf2・i386・pc9821） |  |
| `plan/tools/process/config-zybo-forkrace.mk` | ZEDBSD_CONFIG ZEDBSD_USER_PROGRAMS | その機種が今の build に無い（PLATFORM は x86_64・rpi5・rpi4・vf2・i386・pc9821） |  |
| `plan/tools/zybo/config-zybo-fpga.mk` | ZEDBSD_ARCHITECTURE ZEDBSD_BOARD ZEDBSD_BOOT_KERNEL_MESSAGES ZEDBSD_EXTRA_FILES ZEDBSD_EXTRA_INPUTS ZEDBSD_GRAPHICAL_BOOT ZEDBSD_GRAPHICAL_LOGIN ZEDBSD_MENU_VERSION ZEDBSD_PLATFORM ZEDBSD_ROOTFS_DEVELOPMENT ZEDBSD_USER_PROGRAMS | その機種が今の build に無い（PLATFORM は x86_64・rpi5・rpi4・vf2・i386・pc9821） |  |
| `plan/ws005/tests/config-bug158.mk` | ZEDBSD_ARCHITECTURE ZEDBSD_BOARD ZEDBSD_BOOT_KERNEL_MESSAGES ZEDBSD_GRAPHICAL_BOOT ZEDBSD_MENU_VERSION ZEDBSD_PLATFORM ZEDBSD_ROOTFS_DEVELOPMENT ZEDBSD_USER_PROGRAMS | ws005 は completed、他から呼ばれない |  |
| `plan/ws005/tests/peercred-native-qemu.mk` | ZEDBSD_ARCH_UFS_IMAGE_RULE ZEDBSD_PLATFORM_DIR | ws005 は completed、他から呼ばれない |  |
| `plan/ws005/tests/wifi-credential-native-qemu-config.mk` | ZEDBSD_ARCHITECTURE ZEDBSD_BOARD ZEDBSD_MENU_VERSION ZEDBSD_PLATFORM ZEDBSD_USER_PROGRAMS | ws005 は completed、他から呼ばれない |  |
| `plan/ws005/tests/wifi-credential-native-qemu.mk` | ZEDBSD_ARCH_UFS_IMAGE_RULE ZEDBSD_CONFIG ZEDBSD_PLATFORM_DIR | ws005 は completed、他から呼ばれない |  |
| `plan/ws014/phase008/gpu-core-verification/config-nogpu-amd64.mk` | ZEDBSD_ARCHITECTURE ZEDBSD_BOARD ZEDBSD_MENU_VERSION ZEDBSD_PLATFORM ZEDBSD_USER_PROGRAMS | ws014 は closed、他から呼ばれない |  |
| `plan/ws014/phase009/gpu-core-verification/config-nogpu-amd64.mk` | ZEDBSD_ARCHITECTURE ZEDBSD_BOARD ZEDBSD_MENU_VERSION ZEDBSD_PLATFORM ZEDBSD_USER_PROGRAMS | ws014 は closed、他から呼ばれない |  |
| `plan/ws014/tests/config-vkdemo-amd64.mk` | ZEDBSD_ARCHITECTURE ZEDBSD_BOARD ZEDBSD_MENU_VERSION ZEDBSD_PLATFORM ZEDBSD_USER_PROGRAMS | ws014 は closed、他から呼ばれない |  |
| `plan/ws014/tests/config-wayland-amd64.mk` | ZEDBSD_ARCHITECTURE ZEDBSD_BOARD ZEDBSD_MENU_VERSION ZEDBSD_PLATFORM ZEDBSD_USER_PROGRAMS | ws014 は closed、他から呼ばれない |  |
| `plan/ws031/handover/tools/retired/build_verify.sh` | ZEDBSD_TEST_CPPFLAGS disk-image | retired |  |
| `plan/ws031/handover/tools/retired/sweep_e108.sh` | ZEDBSD_TEST_CPPFLAGS disk-image | retired |  |
| `plan/ws031/handover/tools/retired/sweep_e109.sh` | ZEDBSD_TEST_CPPFLAGS disk-image | retired |  |
| `plan/ws031/handover/tools/retired/sweep_e110.sh` | ZEDBSD_TEST_CPPFLAGS disk-image | retired |  |
| `plan/ws031/handover/tools/retired/sweep_e116.sh` | ZEDBSD_TEST_CPPFLAGS disk-image | retired |  |
| `plan/ws031/handover/tools/retired/sweep_e117.sh` | ZEDBSD_TEST_CPPFLAGS disk-image | retired |  |
| `plan/ws031/handover/tools/retired/sweep_e118.sh` | ZEDBSD_TEST_CPPFLAGS disk-image | retired |  |
| `plan/ws031/handover/tools/retired/sweep_e119.sh` | ZEDBSD_TEST_CPPFLAGS disk-image | retired |  |
| `plan/ws031/handover/tools/retired/sweep_e119f.sh` | ZEDBSD_TEST_CPPFLAGS disk-image | retired |  |
| `plan/ws031/handover/tools/retired/sweep_e120.sh` | ZEDBSD_TEST_CPPFLAGS disk-image | retired |  |
| `plan/ws031/handover/tools/retired/sweep_e121.sh` | ZEDBSD_TEST_CPPFLAGS disk-image | retired |  |
| `plan/ws031/handover/tools/retired/sweep_e122.sh` | ZEDBSD_TEST_CPPFLAGS disk-image | retired |  |
| `plan/ws031/handover/tools/retired/sweep_e123.sh` | ZEDBSD_TEST_CPPFLAGS disk-image | retired |  |
| `plan/ws031/handover/tools/retired/sweep_e123f.sh` | ZEDBSD_TEST_CPPFLAGS disk-image | retired |  |
| `plan/ws031/tests/config-vkprobe-hw.mk` | ZEDBSD_USER_PROGRAMS | ws031 は completed、他から呼ばれない |  |
| `plan/ws034/tests/config-amd64-ca.mk` | ZEDBSD_USER_PROGRAMS | ws034 は canceled、他から呼ばれない |  |
| `plan/ws034/tests/config-amd64-curl.mk` | ZEDBSD_USER_PROGRAMS | ws034 は canceled、他から呼ばれない |  |
| `plan/ws034/tests/config-amd64-dev.mk` | ZEDBSD_ROOTFS_DEVELOPMENT | ws034 は canceled、他から呼ばれない |  |
| `plan/ws034/tests/config-amd64-libs.mk` | ZEDBSD_USER_PROGRAMS | ws034 は canceled、他から呼ばれない |  |
| `plan/ws034/tests/config-amd64-openmax.mk` | ZEDBSD_EXTRA_FILES ZEDBSD_EXTRA_INPUTS | ws034 は canceled、他から呼ばれない |  |
| `plan/ws034/tests/config-amd64-random.mk` | ZEDBSD_EXTRA_FILES ZEDBSD_EXTRA_INPUTS | ws034 は canceled、他から呼ばれない |  |
| `plan/ws034/tests/config-amd64-stream.mk` | ZEDBSD_EXTRA_FILES ZEDBSD_EXTRA_INPUTS | ws034 は canceled、他から呼ばれない |  |
| `plan/ws034/tests/config-amd64-tcp.mk` | ZEDBSD_EXTRA_FILES ZEDBSD_EXTRA_INPUTS | ws034 は canceled、他から呼ばれない |  |
| `plan/ws034/tests/config-amd64-usr.mk` | ZEDBSD_USER_PROGRAMS | ws034 は canceled、他から呼ばれない |  |
| `plan/ws035/tests/config-amd64-demo-venus.mk` | ZEDBSD_USER_PROGRAMS | ws035 は completed、他から呼ばれない |  |
| `plan/ws035/tests/config-amd64-graphical-network.mk` | ZEDBSD_USER_PROGRAMS | ws035 は completed、他から呼ばれない |  |
| `plan/ws035/tests/config-amd64-login.mk` | ZEDBSD_USER_PROGRAMS | ws035 は completed、他から呼ばれない |  |
| `plan/ws035/tests/config-amd64-network.mk` | ZEDBSD_USER_PROGRAMS | ws035 は completed、他から呼ばれない |  |
| `plan/ws035/tests/config-amd64-p048-default.mk` | ZEDBSD_ARCHITECTURE ZEDBSD_BOARD ZEDBSD_MENU_VERSION ZEDBSD_PLATFORM ZEDBSD_ROOTFS_DEVELOPMENT ZEDBSD_USER_PROGRAMS | ws035 は completed、他から呼ばれない |  |
| `plan/ws035/tests/config-pc98-userland.mk` | ZEDBSD_ARCHITECTURE ZEDBSD_BOARD ZEDBSD_MENU_VERSION ZEDBSD_PLATFORM ZEDBSD_USER_PROGRAMS | ws035 は completed、他から呼ばれない |  |
| `plan/ws035/tests/config-pcat-userland.mk` | ZEDBSD_ARCHITECTURE ZEDBSD_BOARD ZEDBSD_MENU_VERSION ZEDBSD_PLATFORM ZEDBSD_USER_PROGRAMS | ws035 は completed、他から呼ばれない |  |
| `plan/ws035/tests/config-sun4u.mk` | ZEDBSD_ARCHITECTURE ZEDBSD_BOARD ZEDBSD_MENU_VERSION ZEDBSD_PLATFORM ZEDBSD_USER_PROGRAMS | その機種が今の build に無い（PLATFORM は x86_64・rpi5・rpi4・vf2・i386・pc9821） |  |
| `plan/ws035/tests/config-x68k.mk` | ZEDBSD_ARCHITECTURE ZEDBSD_BOARD ZEDBSD_MENU_VERSION ZEDBSD_PLATFORM ZEDBSD_USER_PROGRAMS | その機種が今の build に無い（PLATFORM は x86_64・rpi5・rpi4・vf2・i386・pc9821） |  |
| `plan/ws048/tests/rpi4-pcie-boot-config-20261011.mk` | ZEDBSD_ARCHITECTURE ZEDBSD_BOARD ZEDBSD_BOOT_KERNEL_MESSAGES ZEDBSD_GRAPHICAL_BOOT ZEDBSD_GRAPHICAL_LOGIN ZEDBSD_MENU_VERSION ZEDBSD_PLATFORM ZEDBSD_ROOTFS_DEVELOPMENT ZEDBSD_USER_PROGRAMS | ws048 は closed、他から呼ばれない |  |
| `plan/ws048/tests/rpi4-usb-session-config-20261011.mk` | ZEDBSD_ARCHITECTURE ZEDBSD_BOARD ZEDBSD_BOOT_KERNEL_MESSAGES ZEDBSD_GRAPHICAL_BOOT ZEDBSD_GRAPHICAL_LOGIN ZEDBSD_MENU_VERSION ZEDBSD_PLATFORM ZEDBSD_ROOTFS_DEVELOPMENT ZEDBSD_USER_PROGRAMS | ws048 は closed、他から呼ばれない |  |
| `plan/ws073/tests/config-amd64-zdesktop-noclang.mk` | ZEDBSD_USER_PROGRAMS | ws073 は completed、他から呼ばれない |  |
| `plan/ws073/tests/p045-stacks.py` | ZEDBSD_KERNEL_LTO_CFLAGS | ws073 は completed、他から呼ばれない |  |
| `plan/ws073/tests/p051-experiment.sh` | ZEDBSD_KERNEL_LTO_CFLAGS | ws073 は completed、他から呼ばれない |  |
| `plan/ws073/tests/p051-window.sh` | ZEDBSD_KERNEL_LTO_CFLAGS | ws073 は completed、他から呼ばれない |  |
| `plan/ws073/tests/p053-fsync.sh` | ZEDBSD_KERNEL_LTO_CFLAGS | ws073 は completed、他から呼ばれない |  |
| `plan/ws079/tests/config-amd64-notes.mk` | ZEDBSD_USER_PROGRAMS | ws079 は completed、他から呼ばれない |  |
| `plan/ws095/tests/config-amd64-ime.mk` | ZEDBSD_USER_PROGRAMS | ws095 は completed、他から呼ばれない |  |
| `plan/ws100/tests/build-audiod-feedback.sh` | ZEDBSD_CONFIG ZEDBSD_TARGET_SYSROOT | ws100 は completed、他から呼ばれない |  |
| `plan/ws130/tests/config-amd64-ipv6.mk` | ZEDBSD_USER_PROGRAMS | ws130 は completed、他から呼ばれない |  |
| `plan/ws134/tests/config-amd64-monitor.mk` | ZEDBSD_USER_PROGRAMS | ws134 は completed、他から呼ばれない |  |
| `plan/ws143/tests/config-amd64-bt-desktop.mk` | ZEDBSD_USER_PROGRAMS | ws143 は completed、他から呼ばれない |  |
| `plan/ws220/tests/config-amd64-tcp-wrap.mk` | ZEDBSD_TEST_CPPFLAGS | ws220 は completed、他から呼ばれない |  |
| `plan/ws233/tests/run-i915-firmware-package-test.sh` | ZEDBSD_ARCHITECTURE ZEDBSD_BOARD ZEDBSD_CONFIG ZEDBSD_PLATFORM ZEDBSD_USERLAND_DATA_INPUTS ZEDBSD_USER_PROGRAMS | ws233 は completed、他から呼ばれない |  |
| `plan/ws273/tests/config-mips32-malta-force.mk` | ZEDBSD_ARCHITECTURE ZEDBSD_BOARD ZEDBSD_BOOT_KERNEL_MESSAGES ZEDBSD_GRAPHICAL_BOOT ZEDBSD_GRAPHICAL_LOGIN ZEDBSD_MENU_VERSION ZEDBSD_PLATFORM ZEDBSD_ROOTFS_DEVELOPMENT ZEDBSD_USER_PROGRAMS | その機種が今の build に無い（PLATFORM は x86_64・rpi5・rpi4・vf2・i386・pc9821） |  |
| `plan/ws273/tests/config-mips32-rg350.mk` | ZEDBSD_ARCHITECTURE ZEDBSD_BOARD ZEDBSD_BOOT_KERNEL_MESSAGES ZEDBSD_GRAPHICAL_BOOT ZEDBSD_GRAPHICAL_LOGIN ZEDBSD_MENU_VERSION ZEDBSD_PLATFORM ZEDBSD_ROOTFS_DEVELOPMENT ZEDBSD_USER_PROGRAMS | その機種が今の build に無い（PLATFORM は x86_64・rpi5・rpi4・vf2・i386・pc9821） |  |
| `plan/ws273/tests/config-mips64-malta-force.mk` | ZEDBSD_ARCHITECTURE ZEDBSD_BOARD ZEDBSD_BOOT_KERNEL_MESSAGES ZEDBSD_GRAPHICAL_BOOT ZEDBSD_GRAPHICAL_LOGIN ZEDBSD_MENU_VERSION ZEDBSD_PLATFORM ZEDBSD_ROOTFS_DEVELOPMENT ZEDBSD_USER_PROGRAMS | その機種が今の build に無い（PLATFORM は x86_64・rpi5・rpi4・vf2・i386・pc9821） |  |
| `plan/ws275/tests/config-sun4u-x11.mk` | ZEDBSD_ARCHITECTURE ZEDBSD_BOARD ZEDBSD_BOOT_KERNEL_MESSAGES ZEDBSD_GRAPHICAL_BOOT ZEDBSD_GRAPHICAL_LOGIN ZEDBSD_MENU_VERSION ZEDBSD_PLATFORM ZEDBSD_ROOTFS_DEVELOPMENT ZEDBSD_USER_PROGRAMS | その機種が今の build に無い（PLATFORM は x86_64・rpi5・rpi4・vf2・i386・pc9821） |  |
| `plan/ws314/tests/config-amd64-guest.mk` | ZEDBSD_USER_PROGRAMS | ws314 は completed、他から呼ばれない |  |
| `plan/ws314/tests/p001-build.sh` | ZEDBSD_CONFIG ZEDBSD_IMAGE_PROFILE | ws314 は completed、他から呼ばれない |  |
| `plan/ws333/tests/config-amd64-boot.mk` | ZEDBSD_CONFIG ZEDBSD_GRAPHICAL_BOOT ZEDBSD_GRAPHICAL_LOGIN ZEDBSD_USER_PROGRAMS disk-image | ws333 は completed、他から呼ばれない |  |
| `plan/ws333/tests/config-amd64-cpu-x86.mk` | ZEDBSD_USER_PROGRAMS | ws333 は completed、他から呼ばれない |  |
| `plan/ws333/tests/config-amd64-cpuperf.mk` | ZEDBSD_CONFIG ZEDBSD_USER_PROGRAMS disk-image | ws333 は completed、他から呼ばれない |  |
| `plan/ws333/tests/cpu-x86-qemu.sh` | ZEDBSD_CONFIG disk-image | ws333 は completed、他から呼ばれない |  |
| `plan/ws336/tests/config-armv7-ssh.mk` | ZEDBSD_USER_PROGRAMS | その機種が今の build に無い（PLATFORM は x86_64・rpi5・rpi4・vf2・i386・pc9821） |  |
| `plan/ws336/tests/config-odroid-c5-ssh.mk` | ZEDBSD_USER_PROGRAMS | その機種が今の build に無い（PLATFORM は x86_64・rpi5・rpi4・vf2・i386・pc9821） |  |

### N: 対象外（広い grep に掛かったが、古い名を使っていない）（55）

今も有効な名（`ZEDBSD_VARIANT`・`ZEDBSD_JOBS`・`ZEDBSD_TEST_SCREEN_CAPTURE`・試験の script の環境変数など）だけを使う。

<details><summary>一覧</summary>

- `plan/agents/T2/t1-690/runs690.sh`
- `plan/bugs/BUG-267/tests/run-host-replay.sh`
- `plan/bugs/BUG-422/sched-affinity-host.sh`
- `plan/bugs/bug105/run-hid-bolt.sh`
- `plan/tools/docs-check/check-docs-current.py`
- `plan/tools/gpu-compute/demo/build-s13-image.sh`
- `plan/tools/gpu-compute/hw/g3/run-g3.sh`
- `plan/tools/gpu-compute/noct/g3-venus.sh`
- `plan/tools/gpu-perf/amdrdna/dpm-check.sh`
- `plan/tools/gpu-perf/guard.sh`
- `plan/tools/gpu-perf/measure.sh`
- `plan/tools/guest/jobs.sh`
- `plan/tools/guest/serial.py`
- `plan/tools/hid/run-host-i2c-hid.sh`
- `plan/tools/installer/p007-efiboot.sh`
- `plan/tools/rtl8127/rtl-link.sh`
- `plan/tools/vf2-host/native-image/check-native-image.py`
- `plan/ws035/tests/add-uapi-native.py`
- `plan/ws073/tests/bug089.sh`
- `plan/ws073/tests/config-amd64-hybrid.mk`
- `plan/ws074/tests/site-capture.py`
- `plan/ws095/tests/build-ime-image.sh`
- `plan/ws115/tests/build-desktop-image.sh`
- `plan/ws115/tests/meson-probe.sh`
- `plan/ws143/tests/hid-input-host-test.sh`
- `plan/ws143/tests/hid-report-fuzz.sh`
- `plan/ws214/tests/vulkan-overlay-host.sh`
- `plan/ws217/tests/host-hid-pen-props.sh`
- `plan/ws221/tests/ack-order.py`
- `plan/ws221/tests/rdna-tcp.sh`
- `plan/ws255/tests/static-qemu.sh`
- `plan/ws291/tests/5330-mview.sh`
- `plan/ws291/tests/handoff-lib-host-test.sh`
- `plan/ws291/tests/pi4-comp.sh`
- `plan/ws291/tests/pi4-mview.sh`
- `plan/ws291/tests/rdna-live-usb.sh`
- `plan/ws291/tests/rdna-nvme.sh`
- `plan/ws291/tests/rtx-async-check.sh`
- `plan/ws291/tests/rtx-cmdprof.sh`
- `plan/ws291/tests/rtx-p013.sh`
- `plan/ws291/tests/rtx-p024.sh`
- `plan/ws291/tests/rtx-scene-profile.sh`
- `plan/ws291/tests/rtx-subsprof.sh`
- `plan/ws291/tests/rtx-window-ab.sh`
- `plan/ws291/tests/vf2-mview.sh`
- `plan/ws299/tests/compare.py`
- `plan/ws309/tests/pi-p015.sh`
- `plan/ws309/tests/pi4-video.sh`
- `plan/ws309/tests/rtx-p015.sh`
- `plan/ws309/tests/vf2-p015.sh`
- `plan/ws333/tests/hw5330/config-5330-qs40.mk`
- `plan/ws334/tests/openssl-windows.sh`
- `plan/ws336/tests/check-skeleton.sh`
- `plan/ws353/tests/config-amd64-mainwake.mk`
- `plan/ws353/tests/config-amd64-seat.mk`

</details>


## 消す候補（B、Q1 へ。担当は消していない）

81 file。B の根拠のとおり、今の build で動かず、呼ぶ記録が無いか、retired か、今の build に無い機種の物。消す前に Q1 が、WS の Resume point や Future Work が後で使うと書いていないかを確かめる（機械の判定は path の参照だけを見た）。同じ directory の他の file（C・README・期待値）は数えていないので、directory ごと消すかは Q1 が見る。

| path | 理由 |
| --- | --- |
| `plan/agents/T1/files/config-t1-552.mk` | 呼ぶ記録が無い、他から呼ばれない |
| `plan/tools/boot-images/config-sun4u-boot.mk` | その機種が今の build に無い（PLATFORM は x86_64・rpi5・rpi4・vf2・i386・pc9821） |
| `plan/tools/ports/config-m68k-virt.mk` | その機種が今の build に無い（PLATFORM は x86_64・rpi5・rpi4・vf2・i386・pc9821） |
| `plan/tools/ports/config-mips32-malta.mk` | その機種が今の build に無い（PLATFORM は x86_64・rpi5・rpi4・vf2・i386・pc9821） |
| `plan/tools/ports/config-mips64-malta.mk` | その機種が今の build に無い（PLATFORM は x86_64・rpi5・rpi4・vf2・i386・pc9821） |
| `plan/tools/ports/config-ppc32-mac99.mk` | その機種が今の build に無い（PLATFORM は x86_64・rpi5・rpi4・vf2・i386・pc9821） |
| `plan/tools/ports/config-ppc64-pseries.mk` | その機種が今の build に無い（PLATFORM は x86_64・rpi5・rpi4・vf2・i386・pc9821） |
| `plan/tools/ports/config-riscv32-virt.mk` | その機種が今の build に無い（PLATFORM は x86_64・rpi5・rpi4・vf2・i386・pc9821） |
| `plan/tools/ports/config-sun4u.mk` | その機種が今の build に無い（PLATFORM は x86_64・rpi5・rpi4・vf2・i386・pc9821） |
| `plan/tools/ports/config-x68k.mk` | その機種が今の build に無い（PLATFORM は x86_64・rpi5・rpi4・vf2・i386・pc9821） |
| `plan/tools/process/config-riscv64-forkrace.mk` | その機種が今の build に無い（PLATFORM は x86_64・rpi5・rpi4・vf2・i386・pc9821） |
| `plan/tools/process/config-zybo-forkrace.mk` | その機種が今の build に無い（PLATFORM は x86_64・rpi5・rpi4・vf2・i386・pc9821） |
| `plan/tools/zybo/config-zybo-fpga.mk` | その機種が今の build に無い（PLATFORM は x86_64・rpi5・rpi4・vf2・i386・pc9821） |
| `plan/ws005/tests/config-bug158.mk` | ws005 は completed、他から呼ばれない |
| `plan/ws005/tests/peercred-native-qemu.mk` | ws005 は completed、他から呼ばれない |
| `plan/ws005/tests/wifi-credential-native-qemu-config.mk` | ws005 は completed、他から呼ばれない |
| `plan/ws005/tests/wifi-credential-native-qemu.mk` | ws005 は completed、他から呼ばれない |
| `plan/ws014/phase008/gpu-core-verification/config-nogpu-amd64.mk` | ws014 は closed、他から呼ばれない |
| `plan/ws014/phase009/gpu-core-verification/config-nogpu-amd64.mk` | ws014 は closed、他から呼ばれない |
| `plan/ws014/tests/config-vkdemo-amd64.mk` | ws014 は closed、他から呼ばれない |
| `plan/ws014/tests/config-wayland-amd64.mk` | ws014 は closed、他から呼ばれない |
| `plan/ws031/handover/tools/retired/build_verify.sh` | retired |
| `plan/ws031/handover/tools/retired/sweep_e108.sh` | retired |
| `plan/ws031/handover/tools/retired/sweep_e109.sh` | retired |
| `plan/ws031/handover/tools/retired/sweep_e110.sh` | retired |
| `plan/ws031/handover/tools/retired/sweep_e116.sh` | retired |
| `plan/ws031/handover/tools/retired/sweep_e117.sh` | retired |
| `plan/ws031/handover/tools/retired/sweep_e118.sh` | retired |
| `plan/ws031/handover/tools/retired/sweep_e119.sh` | retired |
| `plan/ws031/handover/tools/retired/sweep_e119f.sh` | retired |
| `plan/ws031/handover/tools/retired/sweep_e120.sh` | retired |
| `plan/ws031/handover/tools/retired/sweep_e121.sh` | retired |
| `plan/ws031/handover/tools/retired/sweep_e122.sh` | retired |
| `plan/ws031/handover/tools/retired/sweep_e123.sh` | retired |
| `plan/ws031/handover/tools/retired/sweep_e123f.sh` | retired |
| `plan/ws031/tests/config-vkprobe-hw.mk` | ws031 は completed、他から呼ばれない |
| `plan/ws034/tests/config-amd64-ca.mk` | ws034 は canceled、他から呼ばれない |
| `plan/ws034/tests/config-amd64-curl.mk` | ws034 は canceled、他から呼ばれない |
| `plan/ws034/tests/config-amd64-dev.mk` | ws034 は canceled、他から呼ばれない |
| `plan/ws034/tests/config-amd64-libs.mk` | ws034 は canceled、他から呼ばれない |
| `plan/ws034/tests/config-amd64-openmax.mk` | ws034 は canceled、他から呼ばれない |
| `plan/ws034/tests/config-amd64-random.mk` | ws034 は canceled、他から呼ばれない |
| `plan/ws034/tests/config-amd64-stream.mk` | ws034 は canceled、他から呼ばれない |
| `plan/ws034/tests/config-amd64-tcp.mk` | ws034 は canceled、他から呼ばれない |
| `plan/ws034/tests/config-amd64-usr.mk` | ws034 は canceled、他から呼ばれない |
| `plan/ws035/tests/config-amd64-demo-venus.mk` | ws035 は completed、他から呼ばれない |
| `plan/ws035/tests/config-amd64-graphical-network.mk` | ws035 は completed、他から呼ばれない |
| `plan/ws035/tests/config-amd64-login.mk` | ws035 は completed、他から呼ばれない |
| `plan/ws035/tests/config-amd64-network.mk` | ws035 は completed、他から呼ばれない |
| `plan/ws035/tests/config-amd64-p048-default.mk` | ws035 は completed、他から呼ばれない |
| `plan/ws035/tests/config-pc98-userland.mk` | ws035 は completed、他から呼ばれない |
| `plan/ws035/tests/config-pcat-userland.mk` | ws035 は completed、他から呼ばれない |
| `plan/ws035/tests/config-sun4u.mk` | その機種が今の build に無い（PLATFORM は x86_64・rpi5・rpi4・vf2・i386・pc9821） |
| `plan/ws035/tests/config-x68k.mk` | その機種が今の build に無い（PLATFORM は x86_64・rpi5・rpi4・vf2・i386・pc9821） |
| `plan/ws048/tests/rpi4-pcie-boot-config-20261011.mk` | ws048 は closed、他から呼ばれない |
| `plan/ws048/tests/rpi4-usb-session-config-20261011.mk` | ws048 は closed、他から呼ばれない |
| `plan/ws073/tests/config-amd64-zdesktop-noclang.mk` | ws073 は completed、他から呼ばれない |
| `plan/ws073/tests/p045-stacks.py` | ws073 は completed、他から呼ばれない |
| `plan/ws073/tests/p051-experiment.sh` | ws073 は completed、他から呼ばれない |
| `plan/ws073/tests/p051-window.sh` | ws073 は completed、他から呼ばれない |
| `plan/ws073/tests/p053-fsync.sh` | ws073 は completed、他から呼ばれない |
| `plan/ws079/tests/config-amd64-notes.mk` | ws079 は completed、他から呼ばれない |
| `plan/ws095/tests/config-amd64-ime.mk` | ws095 は completed、他から呼ばれない |
| `plan/ws100/tests/build-audiod-feedback.sh` | ws100 は completed、他から呼ばれない |
| `plan/ws130/tests/config-amd64-ipv6.mk` | ws130 は completed、他から呼ばれない |
| `plan/ws134/tests/config-amd64-monitor.mk` | ws134 は completed、他から呼ばれない |
| `plan/ws143/tests/config-amd64-bt-desktop.mk` | ws143 は completed、他から呼ばれない |
| `plan/ws220/tests/config-amd64-tcp-wrap.mk` | ws220 は completed、他から呼ばれない |
| `plan/ws233/tests/run-i915-firmware-package-test.sh` | ws233 は completed、他から呼ばれない |
| `plan/ws273/tests/config-mips32-malta-force.mk` | その機種が今の build に無い（PLATFORM は x86_64・rpi5・rpi4・vf2・i386・pc9821） |
| `plan/ws273/tests/config-mips32-rg350.mk` | その機種が今の build に無い（PLATFORM は x86_64・rpi5・rpi4・vf2・i386・pc9821） |
| `plan/ws273/tests/config-mips64-malta-force.mk` | その機種が今の build に無い（PLATFORM は x86_64・rpi5・rpi4・vf2・i386・pc9821） |
| `plan/ws275/tests/config-sun4u-x11.mk` | その機種が今の build に無い（PLATFORM は x86_64・rpi5・rpi4・vf2・i386・pc9821） |
| `plan/ws314/tests/config-amd64-guest.mk` | ws314 は completed、他から呼ばれない |
| `plan/ws314/tests/p001-build.sh` | ws314 は completed、他から呼ばれない |
| `plan/ws333/tests/config-amd64-boot.mk` | ws333 は completed、他から呼ばれない |
| `plan/ws333/tests/config-amd64-cpu-x86.mk` | ws333 は completed、他から呼ばれない |
| `plan/ws333/tests/config-amd64-cpuperf.mk` | ws333 は completed、他から呼ばれない |
| `plan/ws333/tests/cpu-x86-qemu.sh` | ws333 は completed、他から呼ばれない |
| `plan/ws336/tests/config-armv7-ssh.mk` | その機種が今の build に無い（PLATFORM は x86_64・rpi5・rpi4・vf2・i386・pc9821） |
| `plan/ws336/tests/config-odroid-c5-ssh.mk` | その機種が今の build に無い（PLATFORM は x86_64・rpi5・rpi4・vf2・i386・pc9821） |
