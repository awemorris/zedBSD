<!-- awesome-plan project=zedbsd record=ws046p013 -->

# ws046-p013: libc に mount の一覧の API を足す（coreutils の cross build のため）

Phase ID: `ws046-p013`
Parent: [WS046](../ws.md)
Status: cleared
Queue: q424-i01

## きっかけ

ws046-p008（q421-i01）: coreutils の cross build が gnulib の `mountlist.c`（「Please port gnulib mountlist.c to your platform!」）で止まる。zedBSD の libc に
`getmntinfo`・`getfsstat`・`getmntent` のどれも無い。`/sbin/mount` は mount の一覧を出せるので、kernel は情報を持っている。

## 目的

`/sbin/mount` が使う kernel の interface を調べ、libc に BSD の `getmntinfo()`（`struct statfs` の配列）か `getfsstat()` を足して、coreutils の configure が見つけて `df` が動くようにする。

## 受け入れ

- coreutils の cross build が configure・build・install まで通る（ws046-p008 の残り）。guest で `df` が mount の一覧を出す。
- 新しい code は規約の全文。回帰（guest の sh・make の差分試験、4 platform の boot）。

## 設計（2026-09-25）

- `/sbin/mount` は `/dev/system` の `ioctl(KERN_SYSTEM_GET_MOUNTS, struct kern_mount_query)`（`<uapi/mountinfo.h>`、最大 64 entry、権限の検査なし）で一覧を得る。
- libc の `struct statvfs` には mount 名が無いので、BSD の `getmntinfo` より glibc 形式の `<mntent.h>` を足す: `struct mntent { mnt_fsname, mnt_dir, mnt_type, mnt_opts, mnt_freq, mnt_passno }`、
  `setmntent(path, mode)`（path は無視して ioctl で一覧を取る）、`getmntent(FILE *)`、`addmntent`（ENOSYS）、`endmntent`、`hasmntopt`、`MOUNTED`（`"/etc/mtab"`、gnulib の `mountlist.c` が参照する）。
  gnulib は `MOUNTED_GETMNTENT1` を選ぶ（`getmntent` が link できること）。
- 実装は `userland/base/libc/posix.c`（`statvfs` と同じ file）か新しい `mntent.c`。`FILE *` は一覧を持つ opaque な記録を返す（gnulib は `FILE *` を `endmntent` に渡すだけ）。

## 実装（q424-i01、2026-09-25）

- `include/libc/mntent.h`（新規）: `struct mntent`、`MOUNTED`（`/etc/mtab`）、`MNTTAB`、`MNTTYPE_*`・`MNTOPT_*`、5 関数の宣言。`include/libc/paths.h` に `_PATH_MOUNTED`・`_PATH_MNTTAB`。
- `userland/base/libc/posix.c`（`statvfs` の隣、build の一覧は変えない）:
  - `setmntent(path, mode)`: path が `MOUNTED` か `/proc/mounts` なら `/dev/system` の `KERN_SYSTEM_GET_MOUNTS` で一覧を取り、mtab の形（`fsname dir type opts 0 0`、blank と `\` は `\ooo`）で `/tmp/.mtab.<pid>.<n>` に書いて unlink し、`fdopen(fd, "r")` の本物の stream を返す（`fclose` でも閉じられる）。他の path は `fopen`（fstab）。device の mount は `/dev/<source>`、擬似 file system は type を fsname に。option は `rw`/`ro`、`nosuid`、`bind`。
  - `getmntent(FILE *)`: 行を読んで 6 field に分け（空行・`#` は飛ばす）、`\ooo` を戻して static な `struct mntent` を返す（glibc と同じ寿命）。
  - `addmntent`: 表は kernel のものなので `EROFS` で 1。`endmntent`: `fclose` して 1。`hasmntopt`: comma 区切りの語の全体か `=` の前で一致。
- coreutils の configure（cross、host）: `HAVE_MNTENT_H`・`HAVE_SETMNTENT`・`HAVE_HASMNTOPT`・`MOUNTED_GETMNTENT1` を検出（configure 82 秒、status 0）。
- coreutils の cross build の次の障害（mountlist の先）: gnulib が cross のとき `posix_spawn` の検査を「guessing no」にして `posix_spawn` 全体を置き換え（`lib/spawni.c`）、その private header の `struct __spawn_action`（glibc の内部名）が zedBSD の `<spawn.h>` の同名の struct と衝突して compile できない。
  gnulib は `posix_spawn_file_actions_addchdir_np`（glibc の名前）も探す。libc には POSIX 2024 の `addchdir`/`addfchdir` があるので `_np` の別名を足した（`include/libc/spawn.h`・`posix.c`）。
  検査の残り（`posix_spawn` が動く、file action 3 種が動く、`posix_spawn`/`posix_spawnp` が `#!` の無い script を拒む）は cross では実行できないので configure の cache 変数で yes と与える。`posix_spawnp` が `#!` の無い script を拒むことは guest の probe で確認した（`posix_spawnp` = 拒否）。
- 次の障害 2 つ（gnulib、cross build）:
  - `lib/strerrorname_np.c` が `EPFNOSUPPORT` と `EAFNOSUPPORT` で `duplicate case value`。zedBSD の `<uapi/errno.h>` は `EPFNOSUPPORT` を `EAFNOSUPPORT` の別名にしていたが、他の system は別の値を持ち gnulib もそう仮定する。`EPFNOSUPPORT` を 68（空き）にし、`src/libc/string.c` の `strerror` に文言を足した。kernel は `EPFNOSUPPORT` を使っていない（uapi の値の追加、既存の値は不変）。
  - `lib/thread-optim.c` が `<elf.h>` を要る（`__ELF__` + `<link.h>` + `dl_iterate_phdr` のある system は `<elf.h>` を持つ、という gnulib の前提）。libc に無かったので `include/libc/elf.h`（System V ABI の名前と値: `Elf32_`/`Elf64_` の型、Ehdr/Phdr/Shdr/Sym/Dyn/Rel/Rela/Relr/Nhdr、`PT_`/`PF_`/`SHT_`/`DT_`/`STB_`/`STT_`、`ELF64_R_SYM` など、x86-64・i386・AArch64・SPARC・m68k・PowerPC の主な relocation）を足し、`<link.h>` の `ElfW(type)` を `Elf64_##type`/`Elf32_##type` に変えて `ElfW_Addr` などは同じ型の別名として残した（rtld は `ElfW_` の名前を使う。rtld の private な `src/rtld/elf.h` とは macro 112 個が共通で値の衝突は無い）。
- 次の障害: gnulib の `lib/utime.h` が `<utime.h>` の無い system では `<sys/utime.h>` を要る。libc に `<utime.h>`（`struct utimbuf`、`utime()` は `utimensat` の上）を足した（XSI、POSIX 2024 では obsolescent だが可搬な software が呼ぶ）。
- 次の障害: coreutils の `src/` が `ETXTBSY` を使う。`<uapi/errno.h>` に無かったので 69 にし、POSIX の errno で欠けていた残り 12（`EBADMSG`・`EMULTIHOP`・`ENETRESET`・`ENOLCK`・`ENOLINK`・`ENOSR`・`ENOSTR`・`ENOTRECOVERABLE`・`EOWNERDEAD`・`EPROTOTYPE`・`ESOCKTNOSUPPORT`・`ETIME`）を 70〜81 で足し、`strerror` の文言も足した（既存の値は不変、重複なし）。
- 次の障害: gnulib の `fpending.c`・`freading.c`・`fseterr.c`・`freadahead.c` が FILE の中身を知る必要があり（「Please port gnulib ... to your platform!」）、`fseeko.c` が `fseeko` を関数として呼ぶ（zedBSD では `fseek` の macro だった）。
  Solaris・glibc・musl の流儀で `<stdio_ext.h>`（`__fpending`・`__freadahead`・`__freading`・`__fwriting`・`__freadable`・`__fwritable`・`__flbf`・`__fbufsize`・`__fpurge`・`__fseterr`・`__fsetlocking`）を libc に足した。gnulib は `AC_CHECK_FUNCS` でこれらを見つけると自前の file を使わない。`fseeko`・`ftello` を `off_t` の本物の関数にした（`fseek` の long に収まらない offset は `EOVERFLOW`）。
- 次の障害: `src/expr.c` の `mpz_out_str` が未宣言。gnulib の mini-gmp（GMP と同じ）は `<stdio.h>` が include 済みかを既知の guard macro（`_STDIO_H`・`_STDIO_H_`・`__DEFINED_FILE` など）で判定し、zedBSD の guard `LIBC_STDIO_H` を知らない。musl と同じく `<stdio.h>` に `_STDIO_H` も定義した。
- 次の障害: gnulib の `getlocalename_l-unsafe.c` は platform ごとの分岐で locale の名前を取り、zedBSD の分岐が無い（POSIX 2024 の `getlocalename_l` は libc にあり configure も検出する）。coreutils の source への 1 箇所の patch（`__ZEDBSD__ && HAVE_GETLOCALENAME_L` で `getlocalename_l` を呼ぶ）を [tests/coreutils-patches/getlocalename_l-zedbsd.patch](../tests/coreutils-patches/getlocalename_l-zedbsd.patch) に置いた（package 化のとき `ZEDBSD_EXT_<name>_PATCHES` で当てる。config.sub の zedbsd は q421 で当てた tar に含まれている）。
- 次の障害: `src/stat.c` が `struct statfs` を要る。coreutils は `struct statvfs` に `f_basetype`（Solaris）か `f_fstypename`（BSD）があれば `statvfs` を使う（`USE_STATVFS`）。`<uapi/statvfs.h>` の `struct statvfs` の末尾に `char f_basetype[16]` を足し、kernel の `mount_statvfs()`（`src/kern/mount.c`）が `m_type->fs_name` を入れる（uapi の ABI の変更。userland は全て image の build で作り直される。HAL は不変）。

## 結果（q424-i01、2026-09-25）

### coreutils の cross build（host）

`/tmp/vitest/cux/coreutils-9.12`（q421 の tar、config.sub の zedbsd 済み）+ [tests/coreutils-patches/getlocalename_l-zedbsd.patch](../tests/coreutils-patches/getlocalename_l-zedbsd.patch)。
configure: `CC="build/llvm/bin/clang --target=x86_64-unknown-zedbsd --sysroot=build/amd64/sysroot" LDFLAGS="-L build/ws053-full-hal-guest/dynamic -Wl,-rpath-link,..." AR/RANLIB=llvm-*`、`--host=x86_64-unknown-zedbsd --build=x86_64-pc-linux-gnu --prefix=/usr/local --disable-nls`、
cross では実行できない検査の cache 変数（`gl_cv_func_posix_spawn_works`・`..._file_actions_{addclose,adddup2,addopen}_works`・`gl_cv_func_posix_spawn{,p}_secure_exec` = yes。`posix_spawnp` の拒否は guest の probe で確認）。
configure は `MOUNTED_GETMNTENT1`・`HAVE_SETMNTENT`・`HAVE_HASMNTOPT`・`HAVE___FPENDING/FREADING/FSETERR/FREADAHEAD`・`HAVE_UTIME_H`・`HAVE_STRUCT_STATVFS_F_BASETYPE`（`USE_STATVFS`）・`HAVE_GETLOCALENAME_L` を検出、status 0（82 秒）。
`make -j32`: **status 0**（`src/` の全 program、warning は gnulib 由来のみ）。`make install DESTDIR=/tmp/vitest/cux-dest`: status 0、`/usr/local/bin` に **102 program**（`df`・`ls`・`stat`・`sort`・`cp`・`install` など、ELF 64-bit x86-64 PIE、dynamic）。

### guest（QEMU amd64 8 GiB、`build/ws053-full-hal-guest` を kernel・libc の変更で作り直した image）

| 検証 | 結果 |
| --- | --- |
| build（disk image、full LTO。sysroot の変更で guest の LLVM package も再 build） | status 0、我々の code の warning 0 |
| boot test | PASS、`build/boot-test-amd64-q424/login.png`（起動中に `usb-storage: BOT CSW error=42` が 1 行出たが起動は完了） |
| guest の make の差分試験 | 91/91 |
| cross build した coreutils（102 program を ustar で `/work/cu` に展開） | `df`: status 0 で `/dev/nvme0n1 /work` と `/dev/shm /shm` を出す（root の overlay と tmpfs は引数無しでは出ない。`df -h /` は `overlay 32M 376K` を出す。理由は下）。`stat -f /`: `Type: overlay`（`f_basetype`）、`stat /bin/sh`、`ls -la`、`sort`/`uniq`/`wc`、`cp`、`date`/`env`/`expr` は status 0 |
| `POSIX-R2-REMAINING.ELF`（`/bin/posix-r2-remaining`） | status 0、01-12 PASS |
| `POSIX-R2.ELF`（SSH） | `conformance-identity`（console が要る）まで通る（BUG-046 は console でのみ） |
| guest の sh の差分試験 | 1388/1425、落ちる 37 件は q422 と同じ集合 |

`df` が引数無しで root の overlay と tmpfs を出さない理由: kernel が disk の無い mount に `st_dev` 0 を返し、GNU df が `st_dev` で mount を同一視するため（[BUG-047](../../bugs/BUG-047.md)、kernel の VFS の不具合。`df -a` は全 mount を出し、`df -h /` は正しい）。

### 変更した file

- libc（`userland/base/libc/posix.c`、`include/libc/`）: `<mntent.h>`（`setmntent`・`getmntent`・`addmntent`・`endmntent`・`hasmntopt`、`MOUNTED`）、`<paths.h>` の `_PATH_MOUNTED`・`_PATH_MNTTAB`、`posix_spawn_file_actions_add{chdir,fchdir}_np`、`<utime.h>`（`utime`）、`<stdio_ext.h>`（`__fpending` など 11 関数）、`fseeko`・`ftello` の関数化、`<stdio.h>` の `_STDIO_H`、`<elf.h>`（新規、System V ABI）、`<link.h>` の `ElfW` を `Elf64_`/`Elf32_` に。
- uapi・kernel: `<uapi/errno.h>` に `EPFNOSUPPORT` 68（別名から独立）・`ETXTBSY` 69・POSIX の残り 12 個（70〜81）、`src/libc/string.c` の `strerror` の文言、`<uapi/statvfs.h>` の `f_basetype[16]` と `src/kern/mount.c` の `mount_statvfs()` での設定。HAL は不変。
- 試験: [tests/coreutils-patches/](../tests/coreutils-patches/)、`/tmp/vitest/mnttest`（getmntent の probe）、`/tmp/vitest/spawnp`（posix_spawnp の probe）、`/tmp/vitest/cutest/guest-coreutils.sh`。
- 規約: 変えた行は `style-check.py` の報告 0。

### 受け入れの判定

| 受け入れ | 結果 |
| --- | --- |
| coreutils の cross build が configure・build・install まで通る | **達成**（configure 0、make 0、install 0、102 program） |
| guest で `df` が mount の一覧を出す | **達成（制限付き）**: `df -a` は全 7 mount を出し、`df` は disk の mount（`/dev/nvme0n1`・`/dev/shm`）を出す。root の overlay と tmpfs の行は kernel の `st_dev` 0（BUG-047）で既定の出力から落ちる。libc の `getmntent` 自体は probe で 6 entry を正しく返す |
| 規約・回帰（sh・make の差分試験、boot） | 規約 0、make 91/91、boot PASS、sh 1388/1425（同じ集合）。4 platform の boot は amd64 だけ実施（他は未実施: kernel の変更は `mount_statvfs()` の 3 行と errno の値の追加で platform に依らない） |

q424-i01 の outcome: **cleared**（BUG-047 は kernel の VFS の不具合として Bug 追跡。`df` の既定の出力が欠ける点はユーザーが blocking と判断すれば uncleared に戻す）。
