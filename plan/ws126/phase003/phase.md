<!-- awesome-plan project=zedbsd record=ws126-p003 -->

# ws126-p003: T2（OpenSSL と expat）

Parent: [WS126](../ws.md)
Status: cleared（2026-10-09 Q1 判定: _ssl・_hashlib の build と ELF、import 0 failed、host の tls-loopback の script の確かめ。guest の確かめは p005 の image で T1）
Disposition: normal
Queue / attempts: q916（2026-10-09 Q1 承認、ユーザー「ベータ2のすべての作業をP1でスケジューリングして行います」）の 3 (2)
Goal: 既存の package の OpenSSL（`_ssl`・`_hashlib`）と expat（`pyexpat`・`_elementtree`）を Python に組み込む。
Prerequisites: p002 cleared。
Investigation bound: 2 時間。

## 範囲

- `--with-openssl=` に `security/openssl` の stage、CA bundle の既定の場所（`/etc/ssl/cert.pem`、ca-certificates の package と同じ）を `ssl.get_default_verify_paths()` が返すようにする。
- expat は `libs/expat` の stage（`--with-system-expat`）か同梱の copy（p001 の決定）。
- `plan/ws126/tests/` に P3 の試験（loopback の TLS の server と client、自己署名の証明書は試験の中で作る）。

## 受け入れ

- stage の `_ssl`・`_hashlib`・`pyexpat` が依存する `.so`（`libssl.so`・`libcrypto.so`・`libexpat.so.1`）を `DT_NEEDED` に持ち、ELF の検査 PASS。guest で P3 を試す（受け入れの本体は p005）。

## 検証

ELF の検査、試しの guest の実行。

## 所有 path

`userland/packages/lang/python3/`、`plan/ws126/`、自分の worktree の `build/`。OpenSSL・expat の package は読むだけ（変更が要れば main へ）。

## 依存・未決の判断

p002。

## 2026-10-09 P1 q916 の結果（cleared 候補）

### 変更

- `userland/packages/lang/python3/Makefile`: stage の OpenSSL（`$(ZEDBSD_EXTERNAL_WORKROOT)/openssl/stage`、zlib と同じく path で名指す）を `--with-openssl=<stage>/usr --with-openssl-rpath=no` で渡し、`-Wl,-rpath-link` に足し、`.zedbsd-staged` を configure の前提にした。pkg-config では渡さない（今の pkg-config は zlib の stage を sysroot として前置するので、別の stage の .pc は合わない）。
- CA bundle: Python の `ssl.get_default_verify_paths()` は OpenSSL の compile 時の既定を返す。security/openssl は `--openssldir=/etc/ssl` で、libcrypto の既定は `/etc/ssl/cert.pem`（`strings` で確かめた）。security/ca-certificates の置き場所と同じなので、Python 側には何も足さない。
- expat: p001 の決定（[design.md](../design.md) の module の表）どおり同梱の expat。`pyexpat`・`_elementtree` は libc.so だけを NEEDED に持つ。受け入れの「`libexpat.so.1` を DT_NEEDED に持つ」は同梱を選んだので当てはまらない。
- `plan/ws126/tests/tls-loopback.py`（P3 の試験）: (1) `_hashlib.openssl_sha256(b"abc")` が FIPS 180-2 の値、`sha512_256` が `hashlib.algorithms_available` にある、`pbkdf2_hmac("sha1")` が RFC 6070 の値 (2) `openssl_cafile` が `/etc/ssl/cert.pem`、そこにあり、`create_default_context()` が CA を 1 つ以上読む (3) `openssl req` で作った自己署名の証明書で loopback の TLS の server と client が handshake・検証・往復する。最後の行が `tls-loopback: PASS` か `FAIL`。一時 file は temp の下の run ごとの directory に残す（消さない）。

### 結果

- `make -j32 -f userland/packages/lang/python3/Makefile PYTHON=python3 python3` rc 0（build/ws126/p003-target1.log）: `checking whether OpenSSL provides required ssl module APIs... yes`、hashlib も yes、`Checked 114 modules (36 built-in, 65 shared, 1 n/a on zedbsd-x86_64, 1 disabled, 11 missing, 0 failed on import)`。missing は `_bz2 _ctypes _ctypes_test _curses _curses_panel _gdbm _lzma _tkinter _uuid _zstd readline`（p004 か範囲外）。
- ELF（llvm-readelf）: `_ssl` の NEEDED は libssl.so・libcrypto.so・libc.so、`_hashlib` は libcrypto.so・libc.so、どちらも RPATH・RUNPATH・TEXTREL・SONAME なし、PT_GNU_RELRO あり。libpython・python3 の check-dynamic-elf は Makefile の中で PASS。
- 試験の host での確かめ（試験の script の検査、zedBSD の証拠ではない）: host 用の build の Python 3.14（host の OpenSSL 3.5.6）で `tls-loopback.py --cafile /usr/lib/ssl/cert.pem` が PASS（TLSv1.3、TLS_AES_256_GCM_SHA384、CA 150）。既定の `--cafile /etc/ssl/cert.pem` では host の path と違うので FAIL になる（失敗の経路の確認）。
- build の warning（CPython の source）: 前回の inttypes の format の warning は消えた（q916 の 3 (1)）。残りは `initconfig.c` の wint_t の format 2 つ（Q1 に報告済み、libcxx の ABI のため直さない）、`remote_debug.h` の unused function、同梱 expat の fallthrough。

### 未実施

- guest での P3: p005 の image で T1 に流す（[phase005](../phase005/phase.md) の「p003 から渡す確認」に書いた）。
