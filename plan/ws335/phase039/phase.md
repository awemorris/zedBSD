<!-- awesome-plan project=zedbsd record=ws335-p039 -->

# ws335-p039: zedBSD の libcrypto-compat.so（build の規則と package・image の登録、TLS は無し）

Status: cleared（2026-11-02 10:21 Q1）。前: 実装済み・試験 PASS（2026-11-02 10:20 Sonnet 5.5 q1925-i01。cleared は Q1）。前: in-progress（2026-11-02 09:40）、planned（2026-11-01 p001 が作成。起動は Q1。依存: p023、p024）
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
- `platform/common/userland-dynamic.mk` に `$(DYNAMIC_DIR)/libcrypto-compat.so` の規則（libz-compat と同じ形: `-soname libcrypto-compat.so --hash-style=both -Bsymbolic-functions -z defs -z now -z relro -z separate-code`、それに `-z nodelete`（cryc-os.h: thread の値の destroy が dlclose の後も map のまま。zedBSD の ld.so が DF_1_NODELETE を守るかを確かめ、守らなければ結果の節に書いて Q1 に）、`--version-script`、`-l:libc.so`、`check-dynamic-elf.py --role shared-library --needed libc.so --soname libcrypto-compat.so`）。pthread と getrandom が libc.so にある事を `-z defs` が確かめる（無ければ止めて Q1 に）。
- 公開の header の sysroot への install: `include/libc/compat/crypto/` が sysroot の `usr/include/compat/crypto/` に入るか（libz-compat の `compat/zlib/` と同じ道か）を確かめ、入らなければ規則を足す（toolchain の `sysroot.mk` は変えない。変える必要があれば止めて Q1 に）。
- 試験: `plan/ws335/tests/check-exports.sh`（新）: .so の `llvm-nm -D --defined-only` の名前の集合が、公開の header の関数の名前（`LIBCRYPTO_COMPAT`）と `cryc_` の名前（`LIBCRYPTO_COMPAT_PRIVATE`）の和に等しい事、他の名前（static でない補助）が無い事、各名前の version node が正しい事（`llvm-readelf --dyn-syms` の `@@`）。最後の行は `check-exports: PASS` か `FAIL`。
- 試験: `plan/ws335/tests/crypto-smoke.c`（新、公開の API だけを使う zedBSD の小さな program: `EVP_Digest` の SHA-256 を FIPS 180-4 の "abc" の例で、X25519 を RFC 7748 6.1 の例で、Ed25519 の署名と確かめを RFC 8032 7.1 の TEST 1 で、`RAND_bytes` の 32 byte、`ERR_get_error` が 0 の事。最後の行 `crypto-smoke: PASS`）と、それを zedBSD の clang で `-l:libcrypto-compat.so` に link する規則（試験の image 用、`plan/ws335/tests/config.mk` で package を y にして作る）。
- 結果の節と ws.md の行。

## 3. 試験と受け入れ

- `make BUILD=build/p039 build/p039/dynamic/libcrypto-compat.so`（x86_64 と aarch64 の zedBSD の config で）が warning 0、`check-dynamic-elf.py` が通る。
- `sh plan/ws335/tests/check-exports.sh build/p039/dynamic/libcrypto-compat.so` が PASS。
- QEMU（Q1 の割当の名前・port。`plan/tools/boot-test.sh` で起動の確かめ、guest の操作は SSH）: `plan/ws335/tests/config.mk` の image で `/lib/libcrypto-compat.so` と `crypto-smoke` が入り、SSH で `crypto-smoke` を走らせて最後の行が `crypto-smoke: PASS`。console の log で判定しない。
- `sh plan/ws335/tests/run-unit.sh all`・`BASE=b7dfd3952 sh plan/ws335/tests/check-skeleton.sh`・`git diff --check` が PASS。

## 4. 注意

- libssl-compat.so は作らない（p033）。`exports.map` に `SSL_`・`TLS_`・`sslc_` を入れない。
- header・module の .c・試験の道具（check-skeleton.sh ほか凍結の一覧）を変えない。header の不備は止めて返す。toolchain・HAL・共有の build を変えない。rm をしない。push しない。
- Windows の build の規則は作らない（Windows は使う側の build が `sources.mk` を読む、[design.md](../design.md) §4.3）。

## 5. 確かめの command

```
make BUILD=build/p039 build/p039/dynamic/libcrypto-compat.so
sh plan/ws335/tests/check-exports.sh build/p039/dynamic/libcrypto-compat.so
sh plan/ws335/tests/run-unit.sh all
BASE=b7dfd3952 sh plan/ws335/tests/check-skeleton.sh
git diff --check
```

## 結果

2026-11-02（host の `date` は 10:20 JST）Sonnet 5.5、Queue q1925-i01。worktree s-ws335-p039（main 7c02d400d から）。

作った物:

- `userland/base/libcrypto-compat/sources.mk`（`LIBCRYPTO_COMPAT_SOURCES` は OS に依らない 27 file を名指し、`LIBCRYPTO_COMPAT_POSIX_SOURCES`・`LIBCRYPTO_COMPAT_WIN32_SOURCES`）。
- `userland/base/libcrypto-compat/Makefile`（package `libcrypto-compat`、shared-library、base、既定 **n**、image の data は `/lib/libcrypto-compat.so`）。`list-user-programs` に `libcrypto-compat|libcrypto-compat|*|n|base|base/libcrypto-compat|` が出る。
- `userland/base/libcrypto-compat/exports.map`（公開の 58 名を 1 つずつ、`cryc_*`、`local: *`）。
- `platform/common/userland-dynamic.mk` に `$(DYNAMIC_DIR)/libcrypto-compat.so` の規則（libz-compat と同じ形 + `-z nodelete`）。amd64・arm64・riscv64 が共有する。
- 試験: `plan/ws335/tests/check-exports.sh`、`crypto-smoke.c`、`build-smoke.sh`（zedBSD の clang で amd64 の dynamic program を link）、`config.mk`（serial の小さな guest、`libcrypto-compat` を選ぶ）、`run-image.sh`（library・smoke・image・QEMU の serial での実行を 1 本に）。

実行した command と最後の行:

| command | 最後の行 |
| --- | --- |
| `make ZEDBSD_CONFIG=config/ci/config-amd64.mk BUILD=build/p039 build/p039/dynamic/libcrypto-compat.so` | rc=0、`warning:` 0 件、末尾は `check-dynamic-elf.py --role shared-library --needed libc.so --soname libcrypto-compat.so`（出力なし = 通過） |
| `make ZEDBSD_CONFIG=config/ci/config-rpi4.mk BUILD=build/p039a build/p039a/dynamic/libcrypto-compat.so`（aarch64） | rc=0、`warning:` 0 件、check-dynamic-elf 通過 |
| `make ZEDBSD_CONFIG=config/ci/config-riscv64-virt.mk BUILD=build/p039r build/p039r/dynamic/libcrypto-compat.so` | rc=0、`warning:` 0 件、check-dynamic-elf 通過（riscv64 は phase の範囲外の追加。worktree の sysroot の複写に `compat/crypto`・`compat/ssl` の header を足した） |
| `sh plan/ws335/tests/check-exports.sh build/p039/dynamic/libcrypto-compat.so` | `check-exports: public 58, cryc_ 186, defined 244` / `check-exports: PASS` |
| わざと壊す: 版つきの map で link、`EVP_sha256` を export から外した map で link | それぞれ `FAILED the library has symbol version definitions` / `FAILED public names the library does not define: EVP_sha256`、`check-exports: FAIL` |
| `sh plan/ws335/tests/build-smoke.sh build/p039 build/tmp/ws335-smoke` | `build-smoke: PASS`（warning 0、check-dynamic-elf `--role application --needed libcrypto-compat.so --needed libc.so` 通過） |
| `sh plan/ws335/tests/run-image.sh build/p039 build/tmp/ws335-run-image`（QEMU、amd64、`plan/tools/guest/amd64-serial.sh` の serial 対話、NVMe の snapshot disk、画面や console の log では判定していない） | guest で `ls -l /lib/libcrypto-compat.so /root/crypto-smoke` が両方出て、`/root/crypto-smoke` が `crypto-smoke: 47 checks` / `crypto-smoke: PASS` / `exit=0`、最後の行 `run-image: PASS`（同じ手順を手で 1 回、script で 1 回、計 2 回 PASS） |
| `WS335_VECTORS=/home/awe/zedBSD-claude1/build/sources/tls-vectors sh plan/ws335/tests/run-unit.sh all` | 全 test と全 ct が PASS（51 行）、最後の行 `run-unit: PASS` |
| `BASE=7c02d400d sh plan/ws335/tests/check-skeleton.sh` | `check-skeleton: PASS`（header・試験の道具はこの Phase で変えていない） |
| `BASE=b7dfd3952 sh plan/ws335/tests/check-skeleton.sh`（phase.md の記載） | `check-skeleton: FAIL`（`userland/base/libcrypto-compat/cryc-x509.h` ほか 2 file が b7dfd3952 から変わっている。p019 の承認済みの 1 語の変更と Q1 の run-unit.sh の変更。この Phase の変更ではない。BASE は 7c02d400d で通る） |
| `git diff --check` | 出力なし |

crypto-smoke（`plan/ws335/tests/crypto-smoke.c`、公開の header だけ）の内容: `EVP_Digest` と context の呼び出しで SHA-256 "abc"（FIPS 180-4）、X25519 の RFC 7748 6.1（公開鍵・Alice と Bob の両方向の共有の秘密）、Ed25519 の RFC 8032 7.1 TEST 1（公開鍵・署名の byte 一致・検証・1 bit 反転と別の message で 0）、`RAND_bytes` 32 byte を 2 回（全零でなく互いに違う）、`CRYPTO_memcmp`・`OPENSSL_cleanse`、最後に `ERR_get_error` が 0。host（ASan・UBSan、全 .c をそのまま build）でも 47 checks PASS。

自分で決めた点・phase.md からの変更:

1. **version node を置かなかった（Q1 が承認、2026-11-02）**。design.md §4.3 と phase.md の「`LIBCRYPTO_COMPAT` と `LIBCRYPTO_COMPAT_PRIVATE` の 2 つの node」で link すると、`tools/build/check-dynamic-elf.py --role shared-library` が `unexpected symbol version definitions` で落ちる（この role は `.gnu.version_d` を禁じ、使う側の program・library の role も `.gnu.version_r` を禁じる。zedBSD の他の library は全部 unversioned の map）。他の compat と同じ unversioned の map（公開の名前 + `cryc_*` を global、`local: *`）にし、check-dynamic-elf.py と使う側の role は変えていない。check-exports.sh は、名前の集合（定義した名前 = 公開の 58 名 ∪ `cryc_*`）、他の名前が無い事、SSL・TLS・`sslc_` の名前が無い事、version 定義と `@` の名前が無い事、定義した名前が全部 GLOBAL である事を確かめる（phase.md の「各名前の version node が `@@`」は無し）。**p035 で design.md の §4.3・D 表と phase.md の記述を直す事**。
2. **`-z nodelete` を ld に渡したが、zedBSD の ld.so は DF_1_NODELETE を守らない**。`llvm-readelf -d` に `FLAGS_1  NOW NODELETE` は入る。しかし `src/rtld/rtld.c` の `__rtld_dlclose` は参照が 0 になると `unload_object_locked` で object を外し、`DT_FLAGS_1`・`NODELETE` を読む所が `src/rtld/` に無い。結果: dlclose の後に thread が終わると、thread ごとの値の destroy 関数（`cryc-os-posix.c` の `os_key` に登録した物）の code が unmap されている可能性が残る。この library を dlopen・dlclose する使い方では起きうる（link して使う program や libssl-compat の `DT_NEEDED` では起きない）。対応は Q1 の判断（rtld に NODELETE を足すか、dlclose を使わない約束にするか）。この Phase では rtld を変えていない。
3. `LIBCRYPTO_COMPAT_SOURCES` は wildcard でなく 27 file を名指し（新しい module は sources.mk に足す）。`exports.map` の公開の名前は、header の宣言を comment・preprocessor を除いて読み取った 58 名（check-exports.sh の awk が同じ読み方で照合する）。
4. 公開 header の sysroot への install は既存の経路（`include/libc/compat/**` が sysroot の `usr/include/compat/` に複写される）で足りる。規則は足していない（main の amd64・arm64 の sysroot には `compat/crypto` がある。riscv64 の古い sysroot の複写には無く、worktree の複写へ手で足した）。
5. `pthread_*`・`getrandom` は libc.so にあり、`-z defs` を通った（未解決の名前は `__libc_errno_location calloc clearerr fclose ferror fopen fread free getrandom malloc memchr memcmp memcpy memset pthread_* read realloc send strlen time write`）。
6. QEMU の試験は、guest の SSH を使わない小さな serial の image（`plan/ws335/tests/config.mk`: `CONFIG_PCAT_SERIAL_MIRROR`・graphical 無し、program は `libcrypto-compat cat echo ls uname sync true false`）。SSH の image は openssh・openssl の package を要し長いため。Q1 から QEMU の名前・port の割当は届いておらず、`plan/tools/guest/amd64-serial.sh` は port を使わない（unix socket、`RUN` の directory は worktree の中）。

未実施: 実機、SSH の guest、arm64・riscv64 の QEMU での実行（build と check-dynamic-elf まで）、dlclose 後の thread 終了（上の 2）、libssl-compat.so（p033）、Windows の規則（範囲外）、`plan/ws335/tests/check-exports.sh` の arm64・riscv64 の .so への実行（amd64 だけ）。

merge の注意（Q1 へ）: `platform/common/userland-dynamic.mk` は libz-compat の rule の次に 14 行を足しただけ（`# libpng-compat` の comment の直前）。他の Phase が同じ file の同じ場所を触ると衝突する。`config.mk`・`run-image.sh` は `plan/ws335/tests/`。この Phase の build の出力は worktree の `build/`（`build/p039`・`p039a`・`p039r`・`tmp/`）。

消してほしい path（Q1）: worktree s-ws335-p039 の `build/p039a`・`build/p039r`・`build/p039/hdd-image.img` と `build/p039/native-swap-1024m.img` と `build/p039/arch-images`（2.4 GB と 1 GB の image）、`build/tmp/p039-serial*`・`build/tmp/ws335-run-image*`・`build/tmp/ws335-exports.run.*`・`build/tmp/ws335-smoke*`・`build/tmp/ws335-unit.run.*`、`build/amd64/sysroot`・`build/arm64/sysroot`・`build/riscv64/sysroot`（main からの複写）、`build/p039-*.log`。worktree ごと片付ける時に一緒に。
