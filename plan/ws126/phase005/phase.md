<!-- awesome-plan project=zedbsd record=ws126-p005 -->

# ws126-p005: image への導入と guest の受け入れ

Parent: [WS126](../ws.md)
Status: planning
Disposition: normal
Queue / attempts: none
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
