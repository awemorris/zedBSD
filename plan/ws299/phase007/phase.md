<!-- awesome-plan project=zedbsd record=ws299-p007 -->

# ws299-p007 受け入れの測りと回帰（QEMU の段と 5330 の段）

- 親: [WS299](../ws.md)。Queue: q2108 / q2108-i01（担当 P4、2026-11-15、QEMU の段だけ。5330 は P1 が使っているので使わない）。前提: p002〜p006・p010 の code（全部 main）。
- Status: in-progress（2026-11-15 P4 q2108: QEMU の段は済み、suite 8 本・host 試験・x86_64・rpi5・vf2 の kernel の build warning 0。5330 の段は未着手）
- Disposition: normal

## 範囲

- QEMU の段（q2108）: `plan/ws299/tests/` の QEMU の試験の image の作り方を今の build（top の Makefile と `mk/`、`CONFIGMK`、`plan/tools/guest/test-image.sh`）に直し、suite を流して p004（2026-11-13 の乱読みの後の run の直し 35b527f23c を含む）と p005 の受け入れ 2・3、p010 の受け入れ 2 を取る。重要な 3 機種（x86_64・rpi5・vf2）の kernel の build の warning 0。
- 5330 の段（後の Queue）: `measure-5330.sh` の受け入れの測り（p004 の受け入れ 1、p005 の受け入れ 1 を同じ形で、p010 の受け入れ 3）、`5330-run.sh` の素の起動、p010 の direct の読み 4 MiB・1 MiB の残りの差。

## 乖離の確かめ（2026-11-15、P4、main ba1482f25d）

- 2026-11-13 の直し 35b527f23c、測りの基点 fe6ea0f967、p005 の 3a4e64353・4e7b2a827、p010 の c9468203a は main の祖先（`git merge-base --is-ancestor`）。35b527f23c の後に buf.c・pci-nvme.c・pci-vmd.c・ufs.c・disk.c・devfs.c・libc の string.c・buf-readahead.h・buf-direct.h を変えた commit は無い（`git log 35b527f23c..HEAD -- …` が空）。
- branch `agent/p6-ws299`（2026-10-29、main に無い commit 7 つ）: `write_cached_lines`（buf.c）と mkfs の `format_tail` は main にある。main に無いのは `plan/ws299/tests/host/mkfs-native-tail-test.sh`（BUG-355 の側の試験）と当時の記録の差分。WS299 の受け入れには効かない。
- 試験の道具の古い所（直した物は下の「直した道具」）: `config-qemu-cache128.mk`・`config-qemu-cache32.mk`・`config-5330-cache128.mk` は `ZEDBSD_CONFIG=… disk-image` の形で、include が相対の path（入れ子の make で読めない）、`CONFIG_BUF_CACHE_KIB` が `override` でなく、土台の `config-amd64-base.mk` の `override CONFIG_BUF_CACHE_KIB := 0` に負けて効かない形だった。`qemu-cache32-write.sh` の kernel の差し替えは FAT の位置を固定の数（133120 sector）で持っていた（今の image でも同じ位置だったが、表から探す形にした）。`qemu-grow-reboot.sh` と `host/ufs-grow-test.sh` は無くなった `make build/zedimage-host` と `tools/build/check-ufs-image.py` を、`host/memcpy-test.sh` は無くなった `build/amd64/sysroot`（と main の checkout の build/ を読む fallback）と `HAL_ARCH_AMD64` を使っていた。
- build の機種: p004・p005 の受け入れ 4 の build の一覧（config-5330・pcat・visionfive2・rpi4・m68k-virt）は今の build に無い名を含む（m68k の platform は今の `mk/` に無い）。今の 6 機種（x86_64・rpi4・rpi5・vf2・i386・pc9821）に読み替え、確かめは重要な x86_64・rpi5（arm64）・vf2（riscv64）でする（AGENTS.md §9、2026-11-13 ユーザーの決定）。
- p005 の受け入れ 1 の基準は 2026-11-13 の Q1 の判断（同じ形で比べる: rawone 対 b-w1m）に直した（[phase005](../phase005/phase.md)）。

## 直した道具（2026-11-15、P4、commit 11ec8cb348）

- `config-qemu-serial.mk`（新規）: QEMU の試験の image。`plan/tools/posix/config-amd64-serial.mk` に `CONFIG_DRIVER_NE2000=n` を足しただけ。土台の config は NE2000 を y にしているが、x86_64 の `mk/platform-x86_64/vmunix.mk` は DP8390 の core（`src/drivers/ethernet/dp8390.c`）を link しないので（i386・pc9821 の vmunix.mk は link する）、`drv_dp8390_*` の未定義で kernel の link が止まる。mk の側の直し（x86_64 でも NE2000 の時に dp8390.c を入れる）は Makefile・mk の変更なので Q1 へ案として出した。
- `config-qemu-cache128.mk`・`config-qemu-cache32.mk`・`config-5330-cache128.mk`: `CONFIGMK=` の形、include は file の場所からの絶対の path、`override CONFIG_BUF_CACHE_KIB`。cache の 2 つは kernel だけを作る（`make CONFIGMK=… BUILD=<dir> <dir>/vmunix`）。guest で `vfs.bufcache.max_bytes` が 134217728・33554432 になる事を確かめた。
- `image-kernel.sh`（新規）: image を写し、表の各区画から `::/vmunix` のある FAT を探して kernel を入れる。`qemu-cache32-write.sh` の固定の位置の代わりと、suite の 128 MiB の image に使う。
- `qemu-suite.sh`: 引数を `SERIAL_IMAGE CACHE128_VMUNIX CACHE32_VMUNIX [TEST...]` に（image 3 つを作らず、kernel 2 つを serial の image の写しに入れる）。試験に `grow-reboot`・`ufs-crash`（2・6・15 秒）を足した。先頭に今の build の手順。各試験の後に `sync`。
- 全ての QEMU の試験: process の名を `-name "${WS299_NAME:-ws299}-<試験>"`（前は `p1-…` の固定か無し）。port は前と同じく `GDB_PORT`・`QMP_PORT` で変えられる。
- `qemu-readahead.sh`: (f) を足した。cache を捨てた後に 1 秒の cache 経由の 4 KiB の乱読み（`/root/randread`、QD1）で 300 MiB の区域に line を散らし、続けて区域を連続で読んで cksum が元と同じ（2026-11-13 の bucket の直し 35b527f23c の道）。
- `qemu-ufs-crash.sh`: (1) 今の image の root に `/sbin/umount` が無い（下の「見つけた事」）ので、guest で `ln -s mount /sbin/umount` してから使う。(2) host の検査を `plan/tools/ufs/check-volume.py`（無くなった `tools/build/check-ufs-image.py` を読む）から、同じ validator `mk/old-tools/build/check-ufs-image.py` を直に呼ぶ形に。
- `qemu-grow-reboot.sh`・`host/ufs-grow-test.sh`: UFS の producer を `mk/old-tools/build/zedimage-host.c` から run の directory に作る（`make build/zedimage-host` は無い）、validator の path を `mk/old-tools/build/`。
- `host/memcpy-test.sh`: sysroot を `build/x86_64/sysroot`（main の checkout の build/ を読む fallback を外した）、`HAL_ARCH_X86_64`。
- 説明の直し: `qemu-directio.sh`・`qemu-direct-reboot.sh`・`qemu-qdprobe.sh` の image の説明（`ZEDBSD_EXTRA_FILES` は今の build に無い）。`qemu-qdprobe.sh` の boot の行 `kern.perf=1`（前は `ZEDBSD_BOOT_EXTRA_LINES`）の今の入れ方は確かめていない（suite の外、未実行）。

## 結果（2026-11-15、P4、main ba1482f25d ＋ 上の道具、QEMU の段）

build（全部 `-j4`、log は worktree の `build/p007/`、git の外）:

| 物 | コマンド | 結果 |
| --- | --- | --- |
| toolchain・sysroot | `make CONFIGMK=config/config.x86_64.mk llvm-toolchain-cached`、`make -j4 CONFIGMK=config/config.x86_64.mk sysroot` | rc 0 |
| 道具 | `sh plan/ws299/tests/build-randread.sh build/p007/tools` | randread・patwrite・directio・cpuloop の zedBSD と Linux の版 |
| serial の image | `ZEDBSD_JOBS=4 plan/tools/guest/test-image.sh --no-harness plan/ws299/tests/config-qemu-serial.mk build/p007/serial --file /root/directio=… --file /root/randread=… --file /root/patwrite=… --mode …=755` | rc 0、`build/p007/serial/hdd-image.img` |
| cache の kernel | `make -j4 CONFIGMK=plan/ws299/tests/config-qemu-cache{128,32}.mk BUILD=build/p007/c{128,32} vmunix` | rc 0、warning 0 |
| x86_64 の kernel | `make -j4 PLATFORM=x86_64 CONFIGMK=config/config.x86_64.mk CONFIG_KERN_THERMAL=y CONFIG_DRIVER_X86_POWER_CONTROL=y build/x86_64/vmunix` | rc 0、warning 0（sha256 先頭 2d98e9bf9c01cbc3） |
| rpi5・vf2 の kernel | `make -j4 CONFIGMK=config/config.{rpi5,vf2}.mk vmunix` | 両方 rc 0、warning 0（`build/rpi5/vmunix`・`build/vf2/vmunix`） |

host 試験（`cc -O2 -Wall -Wextra -I include -I src` で build して実行）: `buf-readahead-test`・`buf-slab-test`・`buf-direct-test`・`dma-range-test`・`nvme-hmb-test`・`nvme-prp-test` 全部 PASS。`host/memcpy-test.sh` PASS（286720 件、rep movsb あり・なし）、`host/ufs-cgmap-test.sh` PASS（3151909 件）、`host/ufs-grow-test.sh` PASS。

QEMU（KVM、`WS299_RUNS=/tmp/p4-ws299-runs WS299_NAME=p4-ws299 GDB_PORT=24412 QMP_PORT=24411 sh plan/ws299/tests/qemu-suite.sh build/p007/serial/hdd-image.img build/p007/c128/vmunix build/p007/c32/vmunix [TEST...]`、判定は guest のコマンドの出力（serial.py の対話）と boot-test の画面）:

| 試験 | 最後の行 |
| --- | --- |
| cache-stress（128 MiB の cache） | `qemu-cache-stress: PASS (7 equal raw checksums, file intact, reload busy 1 of 8)`。evictions 506558、capacity_failures 0、writeback_errors 0 |
| readahead（(f) を足した版） | `qemu-readahead: PASS (7 equal checksums, new data seen, 0 reloads refused)`。(f) の乱読みは 13624 回（区域の 51200 line の約 1/4）、その後の 200 MiB の連続の読みの cksum が元と一致 |
| nvme-shapes | `qemu-nvme-shapes: PASS (5 checksums equal)` |
| directio | `qemu-directio: PASS (2 checks, 3 transfers)` |
| cache32-write（32 MiB の cache、400 MiB） | `qemu-cache32-write: PASS (400 MiB written and synced, checksums equal)` |
| grow-reboot（3 GiB） | `qemu-grow-reboot: PASS reset after 2 s` |
| ufs-crash（2・6・15 秒の切断） | `qemu-ufs-crash: PASS`（3 回とも kept ok・remount ok・check ok） |
| boot | `boot-test: PASS build/p007/boot-test/login.png` |

- 1 回目の suite（`build/p007/suite1.txt`）は ufs-crash だけ FAIL: guest で `umount: not found`（kept の cksum は 3 回とも一致していた）。直した後の 2 回目は host の検査が無くなった path で FAIL。両方を直した 3 回目（`build/p007/ufs-crash3.txt`）が上の PASS。readahead の (f) 入りは 2 回目の suite（`build/p007/suite2.txt`）。他の 6 本は 1 回目の数。
- cache-stress の reload は 8 回のうち 1 回だけ EBUSY（前の記録は 8 of 8）。読み手 4 本が 1 秒の間隔より早く読み終えるため（KVM で速い）と見る。数えるだけで合否には入らない。同時の読みの間の reload を必ず通す形ではなくなっているので、強くするなら読みの量を増やす（未実施）。
- (f) が bucket の道を通った事は、数える口が無いので数では確かめていない（乱読みの line が散った区域を prefetch が読む形を作っただけ）。

## 見つけた事（範囲の外、Q1 へ）

- `/sbin/umount` が image に無い: 旧 `Makefile.bk` の `$(BUILD)/bin/umount: $(BUILD)/bin/mount`（写し）が今の `mk/` に無い。mount の main.c は argv[0] が umount の時に umount として動く。mk の直しの案（rootfs に sbin/umount を mount への link か写しで置く）は Makefile・mk の変更なので Q1 へ。5330 の段の `bench-zedbsd.sh`（p7 の umount）にも効く。
- x86_64 の NE2000 と DP8390（上の「直した道具」）。
- `plan/tools/ufs/check-volume.py` ほか `plan/tools/` の数 file が無くなった `tools/build/check-ufs-image.py` を読む（共有の道具、BUG-435 の側）。
- serial の image の起動で `init: exec /sbin/audiod: No such file or directory`（試験には効かない）。

## 5330 の段に残す物

- p004 受け入れ 1・2（raw の連続の読み 4 GiB、乱読みの後の連続の読みを main の kernel で。bucket の直しの確かめは q2044b の kernel の物）。
- p005 受け入れ 1（同じ形の基準: `rawone` 対 `b-w1m`、p7 の 4 GiB の書き、UFS の file の書き・読み）を main の kernel で取り直し。
- p010 受け入れ 3: direct の読み 4 MiB（92〜93%）・1 MiB（94〜95%）の残りの差（TSC で request の中の区間を測る）。
- `5330-run.sh` の素の起動。5330 の image を作る時は umount の件に注意。

## 再開点（2026-11-15、P4）

- QEMU の段は済み（上の表）。残りは 5330 の段（P1 の 5330 の後）。
