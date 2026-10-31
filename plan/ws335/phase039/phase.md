<!-- awesome-plan project=zedbsd record=ws335-p039 -->

# ws335-p039: zedBSD の libcrypto-compat.so（build の規則と package・image の登録、TLS は無し）

Status: planned（2026-11-01 p001 が作成。起動は Q1。依存: p023、p024）
Parent: [WS335](../ws.md)。担当: Sonnet 5.5 high（phase-runner-low）。共有の `platform/common/userland-dynamic.mk` を変えるので、merge の衝突は Q1 が解く。

- 目的: [WS336](../../ws336/ws.md)（自前の ssh・sshd）の zedBSD の側の Phase が、TLS の全部（p025〜p032）と統合（p033）を待たずに `/lib/libcrypto-compat.so` へ link できるようにする（2026-11-01 Q1 の決め、P5 の案 B）。
  libssl-compat.so と TLS の試験は p033 のまま。この Phase は libcrypto-compat の .so だけを作る。
- この時点で x509・path の確かめ（p017〜p020・p022）が済んでいなくても作ってよい: 済んでいない module は骨格（ENOSYS か失敗の値）のまま .so に入る。export の一覧は header で決まっているので、後の Phase は .so の規則を変えない。

## 1. 読む物

- [AGENTS.md](../../../AGENTS.md) §4・§7・§9・§10、[plan/coding-style.md](../../coding-style.md)（Makefile と script の規則）。
- 設計: [design.md](../design.md) の D1・D4・D23・D25・§4.3。
- 前例: `platform/common/userland-dynamic.mk:35-45`（libz-compat の .so の規則）、`userland/base/libz-compat/Makefile`・`sources.mk`・`exports.map`、`tools/build/check-dynamic-elf.py`。
- 公開の header（export の名前の正）: `include/libc/compat/crypto/*.h`。内部の口: `userland/base/libcrypto-compat/cryc*.h`（`cryc_` の名前）。

## 2. 作る物

- `userland/base/libcrypto-compat/sources.mk`（変数だけ。zedBSD・Linux・Windows の build が読む。[design.md](../design.md) §4.3 の形）:
  - `LIBCRYPTO_COMPAT_SOURCES`: OS に依らない .c の全部（`cryc-os-posix.c`・`cryc-os-win32.c` を除く）。
  - `LIBCRYPTO_COMPAT_POSIX_SOURCES := userland/base/libcrypto-compat/cryc-os-posix.c`
  - `LIBCRYPTO_COMPAT_WIN32_SOURCES := userland/base/libcrypto-compat/cryc-os-win32.c`
- `userland/base/libcrypto-compat/Makefile`: `sources.mk` と `../package.mk` を include し、`ZEDBSD_USERLAND_PACKAGE` で package `libcrypto-compat`（type library、shared-library、`base`、image の data は `/lib/libcrypto-compat.so=$(BUILD)/dynamic/libcrypto-compat.so`、source は `$(LIBCRYPTO_COMPAT_SOURCES) $(LIBCRYPTO_COMPAT_POSIX_SOURCES)`）。**既定は n**（config の `ZEDBSD_USER_PROGRAMS` には足さない。足すのは使う側の WS か Q1）。
- `userland/base/libcrypto-compat/exports.map`: node `LIBCRYPTO_COMPAT`（`include/libc/compat/crypto/*.h` が宣言する関数の全部、名前を 1 つずつ。glob を使わない）と、それを継ぐ node `LIBCRYPTO_COMPAT_PRIVATE`（`cryc_*`）、`local: *;`。
- `platform/common/userland-dynamic.mk` に `$(DYNAMIC_DIR)/libcrypto-compat.so` の規則（libz-compat と同じ形: `-soname libcrypto-compat.so --hash-style=both -Bsymbolic-functions -z defs -z now -z relro -z separate-code`、`--version-script`、`-l:libc.so`、`check-dynamic-elf.py --role shared-library --needed libc.so --soname libcrypto-compat.so`）。pthread と getrandom が libc.so にある事を `-z defs` が確かめる（無ければ止めて Q1 に）。
- 公開の header の sysroot への install: `include/libc/compat/crypto/` が sysroot の `usr/include/compat/crypto/` に入るか（libz-compat の `compat/zlib/` と同じ道か）を確かめ、入らなければ規則を足す（toolchain の `sysroot.mk` は変えない。変える必要があれば止めて Q1 に）。
- 試験: `plan/ws335/tests/check-exports.sh`（新）: .so の `llvm-nm -D --defined-only` の名前の集合が、公開の header の関数の名前（`LIBCRYPTO_COMPAT`）と `cryc_` の名前（`LIBCRYPTO_COMPAT_PRIVATE`）の和に等しい事、他の名前（static でない補助）が無い事、各名前の version node が正しい事（`llvm-readelf --dyn-syms` の `@@`）。最後の行は `check-exports: PASS` か `FAIL`。
- 試験: `plan/ws335/tests/crypto-smoke.c`（新、公開の API だけを使う zedBSD の小さな program: `EVP_Digest` の SHA-256 を FIPS 180-4 の "abc" の例で、X25519 を RFC 7748 6.1 の例で、Ed25519 の署名と確かめを RFC 8032 7.1 の TEST 1 で、`RAND_bytes` の 32 byte、`ERR_get_error` が 0 の事。最後の行 `crypto-smoke: PASS`）と、それを zedBSD の clang で `-l:libcrypto-compat.so` に link する規則（試験の image 用、`plan/ws335/tests/config.mk` で package を y にして作る）。
- 結果の節と ws.md の行。

## 3. 試験と受け入れ

- `make BUILD=build/p039 build/p039/dynamic/libcrypto-compat.so`（x86_64 と aarch64 の zedBSD の config で）が warning 0、`check-dynamic-elf.py` が通る。
- `sh plan/ws335/tests/check-exports.sh build/p039/dynamic/libcrypto-compat.so` が PASS。
- QEMU（Q1 の割当の名前・port。`plan/tools/boot-test.sh` で起動の確かめ、guest の操作は SSH）: `plan/ws335/tests/config.mk` の image で `/lib/libcrypto-compat.so` と `crypto-smoke` が入り、SSH で `crypto-smoke` を走らせて最後の行が `crypto-smoke: PASS`。console の log で判定しない。
- `sh plan/ws335/tests/run-unit.sh all`・`BASE=NEWBASE sh plan/ws335/tests/check-skeleton.sh`・`git diff --check` が PASS。

## 4. 注意

- libssl-compat.so は作らない（p033）。`exports.map` に `SSL_`・`TLS_`・`sslc_` を入れない。
- header・module の .c・試験の道具（check-skeleton.sh ほか凍結の一覧）を変えない。header の不備は止めて返す。toolchain・HAL・共有の build を変えない。rm をしない。push しない。
- Windows の build の規則は作らない（Windows は使う側の build が `sources.mk` を読む、[design.md](../design.md) §4.3）。

## 5. 確かめの command

```
make BUILD=build/p039 build/p039/dynamic/libcrypto-compat.so
sh plan/ws335/tests/check-exports.sh build/p039/dynamic/libcrypto-compat.so
sh plan/ws335/tests/run-unit.sh all
BASE=NEWBASE sh plan/ws335/tests/check-skeleton.sh
git diff --check
```

## 結果

（未着手）
