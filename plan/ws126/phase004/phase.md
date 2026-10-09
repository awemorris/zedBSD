<!-- awesome-plan project=zedbsd record=ws126-p004 -->

# ws126-p004: T3（新しい依存の package と module）

Parent: [WS126](../ws.md)
Status: planning（保留。2026-10-09 ユーザー D1: クリック「今は足さない」→ ctypes・sqlite3・bz2・lzma・zstd は今は足さず、今の module で p005 へ進む）
Disposition: normal
Queue / attempts: none
Goal: D1 で選んだ追加の module（既定案 `ctypes`・`sqlite3`）のために依存の package を加え、Python に組み込む。
Prerequisites: p002 cleared、D1 の判断、`libs/libffi` の所有の調整（WS115 の glib・ws034-p026 と共有）。planning の理由はこの 2 つ。
Investigation bound: 4 時間。bz2・lzma・zstd も選ぶなら別の Phase に分ける。

## 範囲

- `userland/packages/libs/libffi`（MIT。autotools だけで、libtool が未知の OS で共有 library を作らない問題と version script がある。台帳 §3.2・§3.3）。共有 library の SONAME と symbol versioning の無いことを `check-dynamic-elf.py` で確かめる。
- `userland/packages/libs/sqlite`（amalgamation の tarball、public domain、autoconf の簡単な build）。
- 各 package の provenance と license の監査、menuconfig の依存（python3 が選ばれたら選ぶ）。
- Python の `_ctypes`・`_sqlite3` を有効にし、P4 の試験を `plan/ws126/tests/` に。

## 受け入れ

- 依存の package が stage にでき ELF の検査 PASS、`_ctypes`・`_sqlite3` が作られる。guest で P4 を試す（受け入れの本体は p005）。

## 検証

ELF の検査、試しの guest の実行。libffi の host の試験（あれば）。

## 所有 path

`userland/packages/lang/python3/`、`userland/packages/libs/libffi/`・`libs/sqlite/`（新規。libffi は WS115 も使うので、作る側を main が決める）、`plan/ws126/`。

## 依存・未決の判断

p002、D1、libffi の所有。
