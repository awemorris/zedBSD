# WS126: CPython の cross build の方式（ws126-p001 の結論、2026-10-09 P1）

## 版

3.14.8（[provenance.md](provenance.md)）。3.15.0 の final が出たら、別の Phase で上げてよい。

## build 用の Python

- configure は、cross の時に同じ版（3.14）の build 用の Python を求める。host の python3 は 3.13 なので不可（`"python3" has incompatible version 3.13 (expected: 3.14)`）。
- 同じ tarball から host 用を作る: `configure --prefix=<BUILD>/packages/lang/python3/host && make && make install`。2026-10-09 の試しで成功（host の 114 module のうち 5 つが missing、失敗 0）。
- package の Makefile の段 1 にする。

## zedBSD の target の patch

`userland/packages/lang/python3/patches/0001-recognise-the-zedbsd-target.patch`（2026-10-09 に作った。tarball に `--dry-run` で当たる）。

- `config.sub`: `zedbsd*` を OS の名前に足す。
- 生成済みの `configure`（configure.ac は作り直さない）:
  - cross の host の表に `*-*-zedbsd*` → `ac_sys_system=zedBSD`（MACHDEP `zedbsd`）と `_host_ident=$host_cpu`。
  - 共有 library の 4 か所で、zedBSD を Linux と同じに扱う: `libpython$(LDVERSION).so` と SONAME、`LDSHARED='$(CC) -shared'`、`CCSHARED=-fPIC`、`LINKFORSHARED=-Xlinker -export-dynamic`。

## configure の cross の cache（`zedbsd.cache` に無い物を引数で渡す）

| 変数 | 値 | 理由 |
| --- | --- | --- |
| `ac_cv_buggy_getaddrinfo` | no | cross では試せず、既定で「壊れている」とみなして止まる。zedBSD の libc の getaddrinfo は IPv6 を含めて使う |
| `ac_cv_file__dev_ptmx` | yes | zedBSD は `/dev/ptmx` を持つ（kernel の tty.c） |
| `ac_cv_file__dev_ptc` | no | BSD の古い形は無い |

## pkg-config

configure は `pkg-config` で libffi・zlib・lzma・zstd・openssl・uuid を探す。host の pkg-config を使うと、host の library を見つけ（試しで `_ctypes`・`_lzma`・`_zstd` が誤って yes）、ffi.h が無くて失敗する。build は sysroot と stage の .pc だけを見る pkg-config（`PKG_CONFIG_LIBDIR` を zedBSD の stage に限る、external.mk の `zedbsd-pkg-config` と同じ考え）で行う。

## module の表（2026-10-09 の試し、PKG_CONFIG_LIBDIR を空にした configure の結果）

- yes（68）: core と T1 の全部。`_io` `time` `array` `_asyncio` `select` `_socket` `_posixsubprocess` `_multiprocessing` `_posixshmem` `fcntl` `mmap` `termios` `resource` `grp` `pwd` `syslog` `math` `cmath` `_datetime` `_decimal`（同梱の libmpdec） `_json` `_pickle` `_struct` `unicodedata` `_md5` `_sha1` `_sha2` `_sha3` `_blake2` `_hmac`（同梱の HACL*） `pyexpat` `_elementtree`（同梱の expat） `_dbm`（libc の ndbm） `_zoneinfo` `binascii` CJK の codec、ほか試験の module。
- missing:
  - T1 で要る物: `zlib`（`libs/zlib` の stage と .pc を渡す）
  - T2: `_ssl` `_hashlib`（`security/openssl`）
  - T3（D1）: `_ctypes`（libffi）、`_lzma`・`_bz2`・`_zstd`
  - `_uuid`（libuuid が無い。純 Python の uuid は動く）
  - 範囲外: `_curses` `_curses_panel` `_gdbm` `readline` `_tkinter`
- disabled: `_sqlite3`（SQLite が無い、D1）。n/a: `_scproxy`（macOS）。
- libc の観察: `mremap` 無し（mmap の resize は copy の経路）、`kqueue` 無し（select・poll）、`setns`・`unshare` 無し、`major`・`minor`・`makedev` 無し（`os.major` などが無い）。

## 方式の決め

- `--enable-shared`: libpython3.14.so と拡張 module の共有 object（`lib-dynload`、dlopen）。dlopen は zedBSD の libc にある（configure の `dlopen... yes`）。動かなければ `--disable-shared` と `Modules/Setup.local` で静的にする（p002 の予備）。
- install から外す物: `test/`、`idlelib`、`tkinter`、`turtledemo`、`ensurepip`（D2 まで）。
- `.pyc`: build 用の Python の `compileall` で build の時に作る（target で初回に書かずに済む）。
- P5 の `python3 -m test` の集合: `test_json test_re test_datetime test_os test_subprocess test_socket test_threading test_zlib test_hashlib test_unicodedata test_pathlib test_asyncio`（WS の表のまま。test/ を入れない image では、試験の時だけ test/ を写す）。
