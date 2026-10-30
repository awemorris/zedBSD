<!-- awesome-plan project=zedbsd record=ws009-p028 -->

# ws009-p028: clean な tree からの build と amd64 QEMU の boot の再現、互換の主張の照合（案 D）

Status: in-progress（2026-10-30 P7。終わりの条件を満たした: clean tree の 4 variant の build と 5 つの boot が画面の login: まで、DOC-T60 済み。cleared は Q1 が決める）
Parent: [WS009](../ws.md)
Approval: 2026-10-30 ユーザー「WS009、WS296・WS297は標準廉価実装手順を適用してphaseを作り替え、今から実行してOKです。ただ、1 WSずつ実施します。」。2026-10-30 Q1: 案 D は P1 が実行、QEMU は P1 専用の名前・port・directory で流してよい。
実行の担当: Opus（P1）
依存: p026（build-from-source.md の Noct と variant の記述を直した後の文書で再現する）。DOC-T60 の部分は依存なし。

## 1. 目的

WS の完了の条件「build and boot instructions are reproduced in a clean environment」「compatibility claims agree with WS001」を、
今の main で確かめ直す。最後の再現は p002（2026-08-25）・q190 で、その後 amd64 の既定の profile が `native` になり、Noct が 2.0.3 になった。

## 2. 範囲と方法

- **clean な tree**: 確かめる commit を `git archive <SHA> | tar -x -C <worktree>/build/tmp/p028-src/` で展開した、build の状態を持たない tree。
  共有の toolchain（`build/llvm`・`build/NoctLang`・`build/sources`・`build/distfiles` など、文書が「取得する」と言う物）は、その tree の `build/` から
  main の checkout の物への読み取り専用の symlink で使う（LLVM・Noct を build・install しない。AGENTS.md §7）。symlink した物と、tree の中で
  新たに作られた物を結果に分けて書く。
- **手順は文書の通り**: `docs/howto/build-from-source.md` の command を順に（`make help`・`make list-targets`・既定の amd64 の image の build）。
  文書の command で足りない物・違う物があれば、その時点で記録し、文書の誤りとして直す（p028 の範囲の中、build-from-source.md だけ）。
- **boot**: `plan/tools/boot-test.sh`（画面を撮って login prompt を読む。console・serial の log では判定しない）。
  `native`（既定、`BOOT_MODE=uefi-nvme`）で login prompt。時間が許せば `hybrid`（OVMF と `bios-ide` の両方）・`uefi`（OVMF）・`bios`（`bios-ide`）も
  同じ tree で `ZEDBSD_VARIANT=` を変えて image だけを作り直して確かめる（DOC-T72）。i386 PC/AT・PC-98 は範囲の外（過去の WS010 の証拠のまま、結果にそう書く）。
- QEMU の名前・directory: `OUTPUT=build/tmp/p028-boot-<variant>`、`BOOT_TEST_WORK=build/tmp/p028-work-<variant>`。各 VM に `BOOT_TIMEOUT`、各 command に `timeout`。
- 同じ条件で変更なしの再試行は 3 回まで。
- **DOC-T60**: `docs/reference/compatibility-profile.md` の Feature selection の表の各行を `include/libc/features.h` の定義と照らす。
  `uapi_ptr_t` の選び方を `include/uapi/types.h` と照らす。[WS001](../../ws001/ws.md) の今の台帳（§12 など）と、文書が主張する
  「conformance work remains active」「完全な準拠を主張しない」が食い違わないか（文書が WS001 の未完の物を完了と言っていないか）を読む。

## 3. 作る物

- 1 本の script `plan/ws009/tests/p028-reproduce.sh`（展開 → symlink → make → boot-test → 要約）。出力は `build/tmp/p028-*` の新しい directory。
  削除をしない（古い出力は残し、新しい directory に書く）。
- 必要なら `docs/howto/build-from-source.md`・`docs/reference/compatibility-profile.md` の直し。
- この phase.md の「結果」の節、[ws.md](../ws.md) の Phase の表のこの行。PNG の path を結果に書く（Q1 がユーザーに見せる）。

## 4. 変えてはいけない物

- source・Makefile・tools/・toolchain・HAL。共有の `build/` に書かない（symlink は読むだけ）。

## 5. 終わりの条件

`native` の image が clean な tree の文書の手順だけで作れ、boot-test.sh が login prompt を読んだ（PNG がある）。他の variant は確かめた物と
未実施の物を分けて書く。DOC-T60 の照合の表（行・header の値・一致）。commit は
`git commit -m WIP -- plan/ws009/tests/p028-reproduce.sh plan/ws009/phase028/phase.md plan/ws009/ws.md`（と直した docs）。

## 結果

### DOC-T60（2026-10-30 P1、host の `date` 11:1x）

`docs/reference/compatibility-profile.md` を `include/libc/features.h`・`include/uapi/types.h` と [WS001](../../ws001/ws.md) の台帳と照らした。

| 文書の主張 | header・台帳 | 一致 |
| --- | --- | --- |
| `_POSIX_VERSION` 202405L | features.h 42 行 `202405L` | 一致 |
| `_POSIX2_VERSION` 200809L | features.h 43 行 `200809L`、WS001 §4「The advertised values remain `_POSIX2_VERSION=200809L`」 | 一致 |
| `_XOPEN_VERSION` 700 | features.h 44 行 `700`、WS001 §4 同じ | 一致 |
| `__KERN_POSIX_2024_VISIBLE` の条件 | features.h 17〜24 行（`_POSIX_C_SOURCE >= 202405L` か `_XOPEN_SOURCE >= 800`） | 一致 |
| THREADS・TIMERS・MONOTONIC_CLOCK 200809L、DEVICE_CONTROL 202405L | features.h 47・60・61・54 行 | 一致 |
| THREAD_SAFE_FUNCTIONS・PRIO_INHERIT・PRIO_PROTECT・ASYNCHRONOUS_IO・PRIORITIZED_IO・TYPED_MEMORY_OBJECTS は -1 | features.h 55〜59・62 行 | 一致 |
| `__KERN_LEGACY_VISIBLE` は既定と古い selector で見える | features.h 29〜37 行 | 一致 |
| 「完全な SUS Issue 8 の主張ではない」「conformance work remains active」 | WS001 は incomplete・保留、P1 の既知の非互換 75 ほか | 一致（文書は完了を主張しない） |
| `uapi_ptr_t` は **`ZEDBSD_USER_ABI_LP64`** の時 64 bit | types.h は `KERN_USER_ABI_LP64`（build が言わなければ `__LP64__` から定義）。`ZEDBSD_USER_ABI_LP64` は source の中に無い（plan の試験と Noct の patch の compile の旗だけ） | **不一致 → 直した**（`KERN_USER_ABI_LP64` と `__LP64__` からの既定を書いた） |
| `libGL.so` は OpenGL だけ、GLX は `libGLX.so` | userland/desktop/libGL/exports.map に `glX` 0 件、userland/desktop/xserver/libGLX がある | 一致 |
| `ttyname_r` の例 | userland/base/libc/posix.c に `ttyname_r` | 一致（振る舞いは実行で確かめていない） |

観察（直していない）: features.h の comment に「XXX: Rename __KERN_* to __LIBC_*」。`ZEDBSD_USER_ABI_LP64` は Noct の patch（userland/base/noct/patches/0002-use-project-llvm-sysroot.patch）が渡す旗で、types.h は見ない（toolchain の範囲、触らない）。

### build と boot の再現（2026-10-30 11:08〜11:12 P1、ラップアップで停止）

方法は Q1 の判断 (a)（2026-10-30 Q1「(a) で進めてよい。toolchain の 2 手は再現しない（共有の物を使ったと結果に書く。完全な再現は将来の CI か release の UAT の扱い）。config は --defaults、firmware の license の確認は自動で同意してよい…boot は boot-test.sh（画面で login prompt）で判定し…」）。
script `plan/ws009/tests/p028-reproduce.sh`（初版 04ff4da4e、boot の直しはこの commit）。

1 回目の run: 出力 `/home/awe/zedBSD-worktrees/p1-ws009r/build/tmp/p028-20261030-110805/`（clean tree は `src/`、tree は commit 04ff4da4e の git archive、共有の llvm・llvm-source・llvm-build・NoctLang・host-noct-state へ読み取り専用の symlink。toolchain の 2 手 `make toolchain-cache`・`make -j16 toolchain` は再現していない）。

| step | 結果 |
| --- | --- |
| `tools/menuconfig.py --output config.mk --defaults` | exit 0（amd64、`ZEDBSD_VARIANT := native`、clang・libcxx の package 無し） |
| `make help`・`make list-targets` | exit 0（出力は logs/help.log・list-targets.log。文書との突き合わせは未実施） |
| `yes \| make firmware-licenses` | exit 0（nvidia-gsp ほかを同意） |
| `make -j16 sysroot-amd64` | exit 0（11:09:48） |
| `make -j16 disk-image`（native） | exit 0（11:11:10）。`hdd-native.img` 2,485,125,120 bytes、`check-amd64-native-image: … OK`。log の `warning:` 6 件は全て host の perl の locale（compiler の warning ではない）。third-party の openssl に clang の「2 warnings generated」（product の code ではない） |
| boot-test.sh（native、uefi-nvme） | **exit 1・判定なし**: `boot-test: no QMP socket`。原因は script の誤り（`BOOT_TEST_WORK` を build/tmp の深い path にしたため QMP の socket の path が 108 bytes を越えた）。guest の問題ではない。script を直した（`BOOT_TEST_WORK` を設定せず tool の既定 TMPDIR に任せる、`P028_BOOT_ONLY=DIR` で build せず boot だけ） |
| hybrid の disk-image | 途中で停止（ラップアップ、11:12）。uefi・bios は未着手 |

未実施: native の boot の判定、hybrid・uefi・bios の build と boot、`make help`・`list-targets` と文書の突き合わせ。

注意（次の担当へ）: menu の既定は `ZEDBSD_GRAPHICAL_LOGIN := y`。boot-test.sh の既定の pattern は text の `login:` なので、画面が greeter なら pattern が合わない可能性がある。その時は PNG（`OUTPUT/login.png`）を Read で見て、文書（build-from-source.md の「The initial success marker … followed by `login:`」）と menu の既定の食い違いとして記録する（graphical の既定で text の `login:` を成功の印と書くのは文書の誤りの候補）。

### build と boot の再現（2026-10-30 22:47〜23:12 P7、q1725-i01、基点 main 2596260f4）

P1 の image は旧 main（04ff4da4e）の物なので使わず、今の HEAD 2596260f4 で clean tree から全部作り直した。

```sh
cd /home/awe/zedBSD-worktrees/p7-ws009
flock -w 3600 /tmp/zedbsd-image-build.lock timeout 14400 plan/ws009/tests/p028-reproduce.sh          # native hybrid uefi bios
P028_BOOT_ONLY=$PWD/build/tmp/p028-20261030-224755 timeout 1500 plan/ws009/tests/p028-reproduce.sh hybrid bios   # script の直しの後、BIOS の boot だけ
```

出力 `/home/awe/zedBSD-worktrees/p7-ws009/build/tmp/p028-20261030-224755/`（`src/` は commit 2596260f4 の git archive。共有の llvm・llvm-source・llvm-build・NoctLang・host-noct-state へ読み取り専用の symlink、`make toolchain-cache`・`make -j16 toolchain` は再現していない（Q1 の判断 (a)）。それ以外の sysroot・firmware・image は tree の中で作った）。

| step | 結果 |
| --- | --- |
| `tools/menuconfig.py --output config.mk --defaults` | exit 0（`ZEDBSD_PLATFORM := amd64`、`ZEDBSD_VARIANT := native`、`ZEDBSD_GRAPHICAL_BOOT := y`、`ZEDBSD_GRAPHICAL_LOGIN := y`） |
| `make help`・`make list-targets` | exit 0。文書の target（disk-image・vmunix・rootfs・world・bootloader・run・download・toolchain-cache・toolchain・menuconfig）は全部 help か list-targets にある |
| `yes \| make firmware-licenses` | exit 0 |
| `make -j16 sysroot-amd64` | exit 0（22:49:13） |
| `make -j16 disk-image` native・hybrid・uefi・bios | 全部 exit 0（22:50:27・22:53:38・23:01:56・23:06:35）。image 2,485,125,120・585,105,408・584,073,728・516,947,968 bytes。native の log の warning は host の perl の locale 258 行と third-party の OpenSSL の 15 件だけ（zedBSD の code の warning 0）。他の 3 つは 0 |
| boot native（uefi-nvme、OVMF） | PASS、画面に `login:` |
| boot hybrid（uefi-nvme） | PASS |
| boot hybrid（bios-ide、SeaBIOS） | 1 回目は「Booting from Hard Disk...」で止まり FAIL。原因は script: image の写しの横に vmunix が無いので boot-test.sh が qemu-system-i386 を選んだ（tool の説明の通り、64 bit の kernel は i386 の QEMU で止まる）。script に `KERNEL=$src/build/amd64/vmunix` を足して 2 回目 PASS |
| boot uefi（uefi-nvme） | PASS |
| boot bios（bios-ide） | 1 回目は同じ script の誤りで FAIL、2 回目 PASS |

PNG（Q1 がユーザーに見せる）: `build/review/p028-boot-native-uefi-nvme.png`・`p028-boot-hybrid-uefi-nvme.png`・`p028-boot-hybrid-bios-ide.png`・`p028-boot-uefi-uefi-nvme.png`・`p028-boot-bios-bios-ide.png`（worktree の中、元は出力の `boot-*/login.png`）。
どの画面も greeter が「there is no greeter or no display」（native）か「greeter ended」で終わり、console の `login:` が出た。

未実施: 否定の側（`uefi` を SeaBIOS で、`bios` を OVMF で起動しないこと、`native` を SeaBIOS で起動しないこと）、`make run` そのもの（boot-test.sh の uefi-nvme・bios-ide は同じ firmware・bus の組で、command line は同じでない）、i386 PC/AT・PC-98（範囲の外、過去の WS010 の証拠のまま）、toolchain の 2 手。

観察（直していない、Q1 へ）: BIOS の 2 つの起動で kernel が「boot: ignored 1 unknown parameter; first=video」と出す（loader の cfg の `video=640x480` を kernel が知らない）。起動には影響が無い。

### 文書の直し（docs/howto/build-from-source.md）

今の main（WS331・WS332・WS313・WS307 の後）と照らした。docs/ の中の source の path（217 個）は全部実在した（外部の URL の 2 つを除く）。KERNEL_CLOCK_HZ・efiboot の古い記述は docs/ に無い。直した所:

| 所 | 前 | 今の source | 直し |
| --- | --- | --- | --- |
| Status の行 | 2026-10-09 | — | 2026-10-30 |
| menu の CPU / Board | x86_64 か arm64（Raspberry Pi 4） | `MENU_CPUS`: x86_64、arm64（RPi 4・ODROID-C5）、i386（PC-9800・PC/AT）、RISC-V 64（virt・VisionFive 2）、RISC-V 32（virt） | 一覧を書いた |
| main menu の項 | Drivers・Retro が無い | `tui()` の `every_entry` に Drivers・Retro、Desktop は desktop の build される CPU だけ | 2 行足し、Desktop に条件 |
| menu に無い設定 | 「kernel's options and drivers」「the X11 programs」 | drivers は Drivers の menu、Retro の X11 は menu | 「kernel's options」「X11 programs other than Retro's」 |
| i386 | 「no longer offered by the menu」 | menu の CPU に i386 | CPU / Board で選ぶ、に |
| QEMU | 「maintained six-cell runner above」（文書に無い）、amd64 の明示の command が pc・IDE（native の既定と違う） | `platform/amd64/run.mk`: native は q35・OVMF・NVMe | native・uefi の OVMF の command と hybrid・bios の SeaBIOS の command に分けた。Diagnostics の「maintained Variant runner」も OVMF の launch に |
| 成功の印 | init の後に `login:` | menu の既定は graphical login | greeter が display を見つけないと終わり console の `login:` になる、を足した（画面で確かめた振る舞い） |

照合: `python3 -I plan/tools/docs-check/check-docs-current.py .` PASS（56 items）、link の検査 docs/reference・architecture・howto・release PASS 1051。

### 消す物（Q1 へ）

`/home/awe/zedBSD-worktrees/p7-ws009/build/tmp/p028-20261030-224755/`（clean tree と image 4 つ、約 5.0 GB）、P1 の `/home/awe/zedBSD-worktrees/p1-ws009r/build/tmp/p028-20261030-110805/`・`build/tmp/p028-run.out`。

## 引き継ぎ: WS296 の調べ（2026-10-30 P1、q1705-i01、commit はしていない。WS009 の completed の後に WS296 の設計として書く）

- P6（q1667）が WS296 の p001〜p005 を全部実装済み: branch の先頭 `1809873bd`（基点 main `61630bb0b`、src/drivers の 104 file・+14,854/−8,032、`plan/ws296/tests/check-mgmt.sh`・`check-mgmt.exclude`・`config-amd64-test-options.mk` を含む）。未試験・未 merge。P6 の記録では検査 0・XXX 0・12 の config の vmunix が warning 0、usb.c・usb-rtl8822bu.c は `-S` の関数ごとの比較をした。
- 今の main（00396e3da）で P6 の check-mgmt.sh を流すと **413 行**（platform 25・generic 18・usb 12・acpi 12・wifi 8・pci 8・typec 6・i2c 3・fs 3・mmc・isa・gpio 各 1 の file）、XXX の file は 11（31,308 行）。
- 基点の後に main で変わった P6 の file は 10: fs/fat.c・fs/ufs.c・pci/pci-pcat.c・pci/pci.c・platform/pcat/graphics/splash.c・.h・usb/usb-cdc-ecm.c（main 68 行）・usb/usb-cdc-ncm-net.c（**main 1,105 行**）・usb/usb-cdc-ncm.c（261 行）・usb/usb.c（175 行）。他の 94 file は P6 の版をそのまま持ってこられる。
- 組み直しの案（標準廉価実装手順の形）: 各 Phase は main から始め、自分の file 群について P6 の版を `git merge-file`（基点 61630bb0b、main、1809873bd）で取り込み、検査と「動作を変えない」の機械の確かめをする。
  - 道具: P6 の check-mgmt.sh（path を引数で絞れる形に）、XXX の数え、**asm-compare.sh**（基点と変更後の 2 つの tree を同じ config・`ZEDBSD_KERNEL_LTO=none` で vmunix まで build し、対象の .o を llvm-objdump の関数ごとの disassembly と rodata で比べる。差は名前の変わった static 関数・log の文字列・HAL_ASSERT/HAL_FATAL/KERN_FATAL の `__LINE__`（drivers の gpu/ 以外で 18 file・42 か所）だけのはずで、残りを読む）。下書き: `/home/awe/zedBSD-worktrees/p1-ws009r/build/tmp/ws296-draft/asm-compare.sh`（未試験。基点の tree の build/ には llvm・llvm-source・llvm-build・NoctLang・host-noct-state だけを link する案）、file ごとの P6 の差と main の差の数え `files.tsv`（列: path、P6 の変更行、基点から main の変更行）。
  - Phase の群（Sonnet 5.5 high、各 1 回、file が重ならないので並べられる）: (1) 管理情報だけの約 90 file（platform・acpi・wifi・typec・i2c・gpio・isa・mmc・fs・pci・generic の hid-report 以外・usb の小さな物。main で変わった 6 file は merge-file）、(2) HID: generic/hid-report.c・usb-hid.c・usb-hid-checkpoint.c（約 4,200 行の差）、(3) UAS: usb-uas.c・usb-uas-transport.c・usb-uas-disk.c、(4) usb-cdc-ecm.c・usb-cdc-ncm.c、(5) usb-cdc-ncm-net.c（main の変更が大きいので main から当て直し、P6 は参考）、(6) usb.c、(7) usb-rtl8822bu.c、(8) 最後: 全体の検査 0・XXX 0・P6 の 12 の config の build・規約の全文の見直し（Opus を推す）。旧 p001〜p005 は canceled（P6 の branch に置き換え）。
