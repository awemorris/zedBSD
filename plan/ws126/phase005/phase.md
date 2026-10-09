<!-- awesome-plan project=zedbsd record=ws126-p005 -->

# ws126-p005: image への導入と guest の受け入れ

Parent: [WS126](../ws.md)
Status: test-wait（2026-10-09 P1 q916: 登録・image の tree・試験を実装、host の確かめ済み。guest の P1〜P3・P5〜P8 は T1 の依頼）
Disposition: normal
Queue / attempts: q916（2026-10-09 Q1、順「(a)(b)(c) → ws125-p002 → WS126 p005」）
Goal: python3 を menuconfig で選べる package にし、標準 library の tree を image に入れ、guest で P1〜P8 を確かめる。
Prerequisites: p003 cleared（p004 を行うならそれも）、[ws125-p002](../../ws125/phase002/phase.md) の成果が main に統合済み、D2・D3 の判断（既定案で進めてよい）。
Investigation bound: 3 時間。

## 範囲

- `ZEDBSD_USERLAND_PACKAGE` で `lang/python3` を登録（依存: `libs/zlib`、`security/openssl`、`libs/expat`、選んだ T3）。binary と `libpython` は `--file`、標準 library は tree の指定。除外（`test/` ほか）と `.pyc` の扱いは p001 の決定どおり。D2 なら `ensurepip` の wheel も。
- license を `/usr/share/licenses/python3/`。
- `plan/ws126/tests/`: smoke.py（P2）、P3・P4 の試験、P5 の `python3 -m test` の集合を走らせて結果を残す script。

## 受け入れ

[WS126](../ws.md) の P1〜P8。image の増分（MiB・file 数）と `python3 -m test` の結果の表を記録。

## 検証

`plan/tools/guest/guest.sh`、Keiland の terminal の画面（P6）、`plan/tools/boot-test.sh`（PNG をユーザーに見せる）。

## 所有 path

`userland/packages/lang/python3/`、`plan/ws126/`、自分の worktree の `build/`。

## 依存・未決の判断

p003（p004）、ws125-p002、D2、D3。

## 2026-10-09 Q1: p002 から移した確認

- ws126-p002 の「guest の試し」（`python3 -c 'print(1)'`）は、標準 library 約 2700 file と libz.so.1 の登録を image に入れるこの Phase の受け入れに含める。T1 へはこの Phase の image でまとめて依頼する。

## 2026-10-09 P1: p003 から渡す確認

- P3 は `plan/ws126/tests/tls-loopback.py`（p003 で作った）を guest で流す。image に `security/openssl`（`/usr/bin/openssl`、試験の中で自己署名の証明書を作る）と `security/ca-certificates`（`/etc/ssl/cert.pem`）が要る。最後の行が `tls-loopback: PASS`。
- `_ssl` は `libssl.so`・`libcrypto.so`、`_hashlib` は `libcrypto.so` を NEEDED に持つ（image に OpenSSL の 2 つの `.so` が要る）。`pyexpat`・`_elementtree` は同梱の expat で libc.so だけ（`libs/expat` は依存に入れなくてよい）。

## 2026-10-09 P1 q916 の実装（test-wait）

### 判断（既定案と user の決定）

- D1: user「今は足さない」（Q1 2026-10-09）→ T3 は無し、P4 は対象外。
- D2（pip）: 入れない。2026-10-02 user「pip は後回しでいい」に従い `--without-ensurepip` のまま、image から `ensurepip` も外す。
- D3: menuconfig の既定 n（vim・Emacs と同じ）。

### 変更（`userland/packages/lang/python3/Makefile` の packaging）

- image の tree（`build/packages/python3/image`、規則 `python3-image`）: stage の `/usr/lib/python3.14` から `test`・`idlelib`・`tkinter`・`turtledemo`・`ensurepip`・`config-3.14`（静的の libpython、埋め込み用）・`lib-dynload/_test*`・`lib-dynload/xx*`・`*.opt-1.pyc`・`*.opt-2.pyc` を除いて tar で写す（mode・mtime を保つ。普通の `.pyc` は残すので初回に compile しない）。`python3.14`・`libpython3.14.so.1.0`・拡張 module を `zedbsd-strip --strip-unneeded`。strip の後の 2 つを check-dynamic-elf で検査。
- 登録: `ZEDBSD_USERLAND_PACKAGE`（python3、Python 3 interpreter、全 platform、既定 n、menu `packages/lang`、依存 `libs/zlib security/openssl security/ca-certificates`）。`--file /usr/bin/python3.14`、`--file /usr/lib/libpython3.14.so.1.0`、`--subtree /usr/lib/python3.14=<image の tree>`（ws125-p002）、license `/usr/share/licenses/python3/LICENSE`、link `/usr/bin/python3 → python3.14`。`ZEDBSD_PACKAGE_INPUTS` に image の tree の stamp と license。
- image の増分（host の image の tree）: 34 MB、1185 file（libpython 5.5 MB、`python3.14` 9.8 KB、lib-dynload 4.1 MB・53 個）。依存の zlib・OpenSSL・CA は別の package の分。

### 試験（`plan/ws126/tests/`）

- `config-amd64-python3.mk`: beta2 の release の config＋python3。試験のためだけに、同じ build の package の stage から `test/` と `lib-dynload`（`_test*` を含む、strip 前）を `--subtree` で、`smoke.py`・`tls-loopback.py`・`repl-pty.py` を `/root/ws126` に `--file` で足す。
- `smoke.py`（P2、19 の module）、`repl-pty.py`（P6、pseudo-terminal で `6*7` → `42`、`exit()`、どちらの REPL か）、`tls-loopback.py`（P3、p003）。
- `python3-guest.sh OUTDIR`（host から guest.py で）: P1 版、P2、P3、P5（`python3 -m test -j2 --timeout 600` の 12 の試験、結果の行を記録）、P6、P7（license と du・file 数。du は試験用の test/ を含む）。最後の行 `python3-guest: PASS|FAIL`。

### host の確かめ（zedBSD の証拠ではない）

- `make -f userland/packages/lang/python3/Makefile PYTHON=python3 python3-image` rc 0（stage の作り直しを含む、build/ws126/p005-image1.log）。
- toplevel: `make list-user-programs` に `python3|Python 3 interpreter|*|n|packages/lang|lang/python3|libs/zlib security/openssl security/ca-certificates`。試験の config で選択が python3・openssl・zlib・ca-certificates に解決し、`ZEDBSD_PACKAGE_FILES` に 3 つの `--subtree`、link `/usr/bin/python3=python3.14`。
- host の build 用 Python 3.14 で: `smoke.py` は host に特有の 2 つ（/usr/lib/python3.14 と sys.platform）以外 ok、`repl-pty.py` PASS（pyrepl）、`tls-loopback.py` は p003 で PASS。`sh -n python3-guest.sh` ok。
- `make menuconfig-host-test` は rc 2（`MAC-T001: bluetoothd is offered only on amd64`）。変更前の tree でも同じで、この Phase とは無関係（Q1 に報告）。

### 未実施

- guest の P1〜P3・P5〜P7 と P8（boot-test）: T1。ws125-p002 の全体の image と boot-test もこの image で兼ねる。
