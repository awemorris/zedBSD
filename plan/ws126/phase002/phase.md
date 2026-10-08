<!-- awesome-plan project=zedbsd record=ws126-p002 -->

# ws126-p002: build 用 Python と target の interpreter（T1）

Parent: [WS126](../ws.md)
Status: uncleared（2026-10-09 P1: 計画に無い依存。libc の `<sys/types.h>` に POSIX の型が無く、configure の `sizeof(time_t)` が 0 になって build が止まる。libc の直し（WS034 か Bug、Q1 が割り当て）を待つ）
Disposition: normal
Queue / attempts: none
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
