<!-- awesome-plan project=zedbsd record=ws126-p002 -->

# ws126-p002: build 用 Python と target の interpreter（T1）

Parent: [WS126](../ws.md)
Status: cleared 候補（2026-10-09 P1 q916: 依存の libc の不足を直し、target の interpreter と T1 の module を cross build・stage した。guest の試しは未実施で、p005 の image の登録と合わせて T1 へ回す案。Q1 の判定待ち）
Disposition: normal
Queue / attempts: q916（2026-10-09 Q1 承認、ユーザー「ベータ2のすべての作業をP1でスケジューリングして行います」）item 2
Goal: 同じ版の build 用 Python を host に作り、target の `python3` と T1 の module を cross build して stage する。
Prerequisites: p001 cleared（方式の確定、planning の理由）。
Investigation bound: 4 時間。

## 範囲

- `userland/packages/lang/python3/Makefile`（`ZEDBSD_EXT_python3_*`、`ZEDBSD_EXTERNAL_SOURCE`）、`patches/`。
- build 用の Python: 同じ distfile を `build/packages/python3/host` に展開して host の cc で作る（host の `/usr` に入れない）。
- target: `ZEDBSD_EXTERNAL_CROSS_ENV` と cache で configure（`--host`、`--build`、`--with-build-python`、`--prefix=/usr`、`--without-ensurepip`（D2 は p005）、zlib は `libs/zlib` の stage）。`make` と `DESTDIR` の install で stage。`check-dynamic-elf.py` で `python3`・`libpython`・拡張 module の ELF を確かめる。
- 依存する package の stage は OpenSSH と同じく path で名指す（変数の読み込み順に依らない）。
- libc の不足は回避せず記録し、main 経由で WS034 か Bug へ。

## 受け入れ

- stage に `/usr/bin/python3`、`/usr/lib/python3.<x>/`（T1 の module を含む）。ELF の検査 PASS。guest で `python3 -c 'print(1)'` を試し、結果を記録（受け入れの本体は p005）。

## 検証

build の warning の確認、ELF の検査、試しの guest の実行。

## 所有 path

`userland/packages/lang/python3/`、`plan/ws126/`、自分の worktree の `build/`。

## 依存・未決の判断

p001。

## 2026-10-09 P1 の試み（uncleared）

- `userland/packages/lang/python3/Makefile` を作った。中身は 3 つ: 取得と検証（`ZEDBSD_EXTERNAL_SOURCE`）、build 用 Python（`python3-host`、`build/packages/python3/host`）、target の configure・make・`DESTDIR` の install と ELF の検査（`python3`）。
- target の configure の要点:
  - zedBSD の cross の環境と cache（design.md の 3 つ）。
  - pkg-config は zlib の stage だけを見る（`PKG_CONFIG_LIBDIR`・`PKG_CONFIG_SYSROOT_DIR`）。
  - `--enable-shared --without-ensurepip --without-mimalloc`。mimalloc の Unix の層は `<sys/syscall.h>` と生の system call を使い、zedBSD に無い。GIL のある既定の build は pymalloc で足りる。
- 結果:
  - `make … python3-host` は rc 0。
  - `make … python3` は rc 2。`Python/pytime.c` の `#error "unsupported time_t size"` で止まる。configure の `checking size of time_t` で `<sys/types.h>` に `time_t` が無く、`ac_cv_sizeof_time_t=0` になったため。
- libc の不足（回避していない、Q1 へ）: zedBSD の `<sys/types.h>` に、POSIX が求める次の型が無い（`zedbsd-clang` で `#include <sys/types.h>` だけを入れて 1 つずつ確かめた）。
  - `clock_t` `clockid_t` `time_t` `timer_t`
  - `fsblkcnt_t` `fsfilcnt_t` `key_t`
  - `pthread_t` `pthread_attr_t` `pthread_mutex_t`（ほかの pthread の型も同じ見込み）
- 再開の条件: libc の `<sys/types.h>` の直しが main に入ったら、`make … python3` を流し直す（cache で `ac_cv_sizeof_time_t=8` を渡す回避はしない）。

## 2026-10-09 P1 q916 の再開（cleared 候補）

### 依存の解消（libc、q916 item 1 と同じ Queue で Q1 の承認つき）

- q916 item 1（commit b6c91e342、main に merge 済み）: libc の `<sys/types.h>` に POSIX の型を足した。time_t・clockid_t・timer_t・fsblkcnt_t・fsfilcnt_t は `include/uapi/types.h` へ移し、clock_t・key_t・struct sched_param・pthread_* の 13 型は `include/libc/sys/types.h` へ移した（どれも 1 箇所の定義、layout 不変）。Bug ticket は作らない（Q1 2026-10-09「q916 item 1 として記録すれば足りる」）。これで configure の `checking size of time_t... 8`。
- その後に `make -k` で洗い出した libc の不足を、Q1 の承認（2026-10-09）を得て直した:
  - (a) `<unistd.h>` と `sysconf()` に `_SC_GETGR_R_SIZE_MAX`（`ACCOUNT_RESULT_MAX`、getgrnam() の内部 buffer の定数を新しい `userland/base/libc/account-internal.h` に移して account.c と sysconf の両方が参照）と `_SC_TTY_NAME_MAX`（`PATH_MAX`、ttyname() の内部 buffer と同じ定数）。grp・posix（os.ttyname）module が使う。
  - (b) `include/libc/sys/resource.h` に `RLIM_NLIMITS`（`RLIMIT_NLIMITS` の別名、POSIX 外。libc の header に可視性の guard の慣習が無いので guard なし）。resource module が使う。
  - (d) `include/libc/sys/socket.h` に `SOMAXCONN 128`。ユーザーの指示（2026-10-09、Q1 経由）「<sys/socket.h> ですが、libcに入れてくれますか？」により libc に置き、uapi と kernel は変えない（最初に uapi と kernel の unix-socket.c に入れた案は戻した）。値は kernel の unix stream の上限 `UNIX_LISTEN_BACKLOG_MAX` と同じで、その旨を comment に書いた（数字は 2 箇所）。socket module が使う。
- package 側の判断（libc は直さない）:
  - (c) `ac_cv_header_netpacket_packet_h=no` を Makefile の cache に足した。zedBSD の `<netpacket/packet.h>` はあるが packet socket の address は `struct sockaddr_l2` で、socket module の AF_PACKET 対応が書かれた Linux の `struct sockaddr_ll`・`PACKET_HOST` と形が違う。socket module は AF_PACKET 無しで build する。
  - patch `0002-declare-ioctl-for-the-socket-module.patch`: socket module は ioctl(FIONBIO) を使うが、`<sys/ioctl.h>` を include するのは名指しの system と `<netpacket/packet.h>` の横だけ。(c) で後者が外れたので zedBSD を前者に足した。
  - patch `0003-clear-key-material-with-explicit-bzero.patch`: HACL* の memzero が zedBSD を知らず `#warning` と volatile の loop に落ちていた。libc の `explicit_bzero()`（`<strings.h>`）を使う枝に足した。

### build の結果

- `make -j32 build/amd64/vmunix sysroot-amd64 sysroot-i386 sysroot-arm64 build/amd64/dynamic/libc.so` rc 0、warning 0、`kernel include check: PASS`、`amd64 vmunix check: PASS`（各変更の後に流した、最後は build/q916-build4.log）。
- header の試験（scratchpad、host の短い試験）: `<sys/types.h>` だけで POSIX の 35 型を宣言、関係 header 15 個を一緒に読む。x86_64・i386・aarch64 × `-std=c89 -pedantic`・`-std=c11`・`-std=gnu17`、`-x c++ -std=c++17`、`-Wall -Wextra -Werror` で PASS。
- `make -j32 -f userland/packages/lang/python3/Makefile PYTHON=python3 python3`（単独で呼ぶ時は `PYTHON=python3` が要る、curl と同じ）rc 0（build/ws126/p002-target8.log）。`Checked 114 modules (36 built-in, 63 shared, 1 n/a on zedbsd-x86_64, 1 disabled, 13 missing, 0 failed on import)`。
  - missing の 13: `_bz2 _ctypes _ctypes_test _curses _curses_panel _gdbm _hashlib _lzma _ssl _tkinter _uuid _zstd readline`（`_ssl`・`_hashlib` は p003、`_ctypes`・`_bz2`・`_lzma`・`_zstd` は p004、残りは範囲外）。disabled は `_sqlite3`（p004）。T1 の層（zlib・`_decimal`（同梱の libmpdec）・`_dbm`・`_socket`・`select`・`_multiprocessing`・`_posixsubprocess`・`termios`・`fcntl`・`mmap`・`resource`・`unicodedata`・`_json`・`math`・`cmath` ほか）は全部 build された。
  - ELF の検査: Makefile の `check-dynamic-elf.py` で `libpython3.14.so.1.0`（shared-library、SONAME 一致、NEEDED libc.so）と `python3.14`（application、NEEDED libpython3.14.so.1.0・libc.so、PT_INTERP /lib/ld.so）が PASS。拡張 module（lib-dynload の 63 個）は checker に合う role が無い（module は試験用、application は PT_INTERP を求める）ので `llvm-readelf` で確かめた: 全部 x86-64 の ET_DYN、PT_GNU_RELRO あり、PT_INTERP・TEXTREL・RPATH・RUNPATH・SONAME なし、NEEDED は libc.so（zlib と binascii は libz.so.1 も）。拡張 module は libpython を NEEDED にせず、python3 が読み込んだ libpython の symbol で解決する（Linux と同じ形）。guest で解決できるかは未確かめ。
  - CPython の build の warning（第三者の source、33 行）: xmltok_impl.c の fallthrough（19、同梱 expat）、`remote_debug.h` の unused function、format の不一致（下の libc の所見）。
- stage: `/usr/bin/python3.14`（と python3 などの link）、`/usr/lib/libpython3.14.so.1.0`（22 MB、debug 情報つき）、`/usr/lib/python3.14/`（271 MB、うち test/ 161 MB・idlelib 8.2 MB・tkinter 1.6 MB・lib-dynload 14 MB）。strip と入れる tree の選びは p005。

### libc の所見（直していない、Q1 へ報告）

- `<inttypes.h>` の `PRId64` などが LP64 でも `"lld"` で、`int64_t` は `long`。`-Wformat` が `format specifies type 'long long' but the argument has type 'int64_t' (aka 'long')` を出す（CPython の _testembed.c・_decimal.c・expat）。`PRIdPTR` なども同じ（`uintptr_t` は `unsigned long`）。動作は同じ大きさなので壊れないが、型は合っていない。
- `<wchar.h>` の `wint_t` は `uint32_t` だが、clang の zedBSD target の `__WINT_TYPE__` は `int`。printf の `%lc` で warning（initconfig.c）。compiler の側か libc の側かの判断が要る（toolchain に触れるので P は直さない）。

### 未実施と残り

- guest で `python3 -c 'print(1)'`: 未実施。image に入れるには 2700 個ほどの標準 library の file と libz.so.1 の登録が要り、それは p005 の範囲（menuconfig の登録、入れる tree の選び）。p005 の image で T1 に流す案。p002 の受け入れの「guest の試し」をそこへ回すかは Q1 の判定。
- 再開の条件（p003）: `security/openssl` の stage を pkg-config と同じ形で見せ、`_ssl`・`_hashlib` を build する。
