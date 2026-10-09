<!-- awesome-plan project=zedbsd record=ws126-p003 -->

# ws126-p003: T2（OpenSSL と expat）

Parent: [WS126](../ws.md)
Status: planned（2026-10-09 Q1、q916 の 3 として P1 に割当）
Disposition: normal
Queue / attempts: none
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
