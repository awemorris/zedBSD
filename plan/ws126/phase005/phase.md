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

## T1-495 の FAIL の解析と直し（2026-10-09 深夜、P1）

T1-495: image（0）・boot-test（1）は PASS、python3-guest の P2・P3・P5・P6 が `ImportError: invalid shared-object path`（math・json・zlib・hashlib など lib-dynload の拡張 module。os・sys・re など組み込みの物は ok）。
原因: zedBSD の ld.so（`src/rtld/rtld.c` の `__rtld_dlopen`）は bare name と `/lib/<name>` だけを受け、それ以外の slash を含む path を `dlopen_bare_name` が NULL にして「invalid shared-object path」で断っていた（BUG-083 の直しで「任意の絶対 path は今のまま断る」と残った物）。CPython は拡張 module を `/usr/lib/python3.14/lib-dynload/<module>.cpython-314-….so` の絶対 path で dlopen するので、全部断られる。image の配置ではなく loader の制限。
直し（`src/rtld/rtld.c`）: POSIX の dlopen のとおり、`/lib/` と bare name 以外の slash を含む path は、その file をそのまま開く（`dlopen_names_file`）。既に読んだ object の判定は path が同じ物だけ（別の directory の同じ名前の file を取り違えない。同じ file の別の path は load の時の inode（`find_identity`）で同じ object）。`load_object` を名前の検索（`load_object`）と開いた file の map（`load_object_file`）に分け、path で開く `load_object_path` を足した。bare name・`/lib/<name>`・DT_NEEDED の検索は今までと同じ。
確かめ: `make BUILD=build/p1-rtld build/p1-rtld/dynamic/ld.so`（-Werror、warning 0）。rtld は guest でしか動かないので host の試験は無し。回帰の道具 `plan/tools/rtld/`（Master の Tools）に段 `path` を足した（検索の届かない `dynload/libpathmod.so` を絶対 path・`./` の相対 path・再度の絶対 path で開いて同じ関数の address、無い path は NULL）。`rtld-many.c` は target の clang で -Werror の compile のみ。`build-many.sh` の host の `rm -rf "$out"` は `fresh_out` に替えた（2026-10-06 の規則）。
未実施（T1）: `rtld-many.sh`（新しい段 path を含む 17 段）と、T1-495 の python3-guest の再試験。
注: `plan/ws074/tests/rtld-dlopen.*`（ws074-p017、cleared）は「/usr/lib/libcrypto.so の絶対 path は断る」を確かめる試験で、今の挙動と合わない。Master の Tools にも未完了の Phase にも無いので、直さずに削除を Q1 に依頼する。
