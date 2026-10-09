<!-- awesome-plan project=zedbsd record=ws126 -->

# WS126: Python 3 の package

<!-- awesome-plan-current:start -->
Status: planning
Primary Milestone: MG002
Objectives: O2
Parent: [Master](../master.md)
Focused goal: fg019（ベータ1）
Queue: none
Resume point: 2026-10-09 P1 q916: p002 cleared（Q1）。p003 は cleared 候補（`_ssl`・`_hashlib`、CA bundle /etc/ssl/cert.pem、tls-loopback.py は host で PASS、guest は p005 で）。次は p004（D1 の判断待ち）か p005（ws125-p002 の成果待ち）。
2026-10-02 user:「Python の module の範囲はまずコアが動くことを目指します。そのあとpipも目指しますが、後回しでいいです。」→ ベータ1 はまず core（外部依存の無い標準 module）で動くこと、release の image に入れる。pip（ensurepip）は後回し。
Target: ベータ3 の合間の仕事（2026-10-09 のユーザーの一覧に WS126 が明示され、下の 2026-10-05 の「ベータ4 以降」より新しいので優先、Q1 2026-10-09）。旧: **ベータ4 以降**（2026-10-05 user「WS037, WS044,WS048,WS141, WS112, WS118, WS124, WS125, WS126, WS119, WS096, WS097, WS039, WS038, WS144, WS143, WS146,WS147, WS152,  WS119, WS080, は、ベータ4以降としてください。…WS027, WS015, WS047, WS028, WS017,  WS077, はキャンセルします。」）
<!-- awesome-plan-current:end -->

## 目標（2026-10-02 ユーザー（ベータ1、リリース目標 10/17））

「userland/packages/python3 を追加」

- 外部 package として CPython 3 を build/install する。置き場所は `userland/packages/lang/python3`（2026-10-02 user 承認）。
- 版の提案: **3.15.0**（PEP 790 の予定で 2026-10-01 に公開、rc2 は 2026-09-01）。p001 で公開と署名（Sigstore／PGP）を確かめる。公開されていないか zedBSD で重大な問題があれば、3.14 系の最新の bugfix 版にする。
- license: PSF-2.0（同梱の第三者部分を含めて監査する）。

## ベータ1 の到達目標と受け入れ条件（案）

| # | 基準 | 確かめ方 |
| --- | --- | --- |
| P1 | guest で `python3 -V` と `python3 -c 'import sys; print(sys.version)'` が版を出す | SSH |
| P2 | 代表の module の smoke 試験（`plan/ws126/tests/smoke.py`）が全部 PASS: `os` `sys` `re` `json` `datetime` `pathlib` `subprocess`（`/bin/echo`）`threading` `socket`（loopback TCP）`select` `asyncio`（loopback）`zlib` `hashlib`（sha256・blake2）`unicodedata` `decimal` `struct` `math` `random`/`secrets` `tempfile` `shutil` `locale`（UTF-8 の入出力）`multiprocessing`（fork） | SSH |
| P3 | `ssl`: CA bundle を読み、loopback の TLS の server と client が通信する。`hashlib` の OpenSSL の経路 | SSH |
| P4 | D1 で選んだ追加の module（既定案: `ctypes` で libc の `strlen` を呼ぶ、`sqlite3` の in-memory DB） | SSH |
| P5 | `python3 -m test` の選んだ集合（`test_json test_re test_datetime test_os test_subprocess test_socket test_threading test_zlib test_hashlib test_unicodedata test_pathlib test_asyncio` など、p001 で確定）の結果を記録し、指定した集合は PASS。失敗は Bug か制限として記録 | SSH |
| P6 | 端末で対話の REPL が起動し、式を評価して終了できる（pyrepl か基本の REPL のどちらで動いたかを書く） | SSH の pty、Keiland の terminal |
| P7 | `/usr/share/licenses/python3/LICENSE`、provenance、license の監査。image の増分を記録（`test/`・`idlelib`・`tkinter`・`turtledemo` は入れない案） | 文書と image |
| P8 | Python を選んだ image でも `plan/tools/boot-test.sh` の login prompt が出る | boot-test の PNG |

## module と依存（既存と不足）

| 層 | module | 依存 | 状態 |
| --- | --- | --- | --- |
| T1（外部依存なし） | core、純 Python の標準 library、`_socket` `select` `_posixsubprocess` `termios` `fcntl` `mmap` `resource` `_multiprocessing`（`sem_open`）`_json` `_datetime` `math` `cmath` `unicodedata` `_hashlib` 無しの `hashlib`（同梱 HACL*）`_decimal`（同梱の libmpdec が無ければ `_pydecimal`）`dbm.ndbm`（libc の ndbm） | libc | libc に pty・termios・locale・`dlopen`・`sem_open`・`socketpair`・`getaddrinfo`・libm（`lgamma` `erf` `fma` 等）がある。`getloadavg`・`timerfd`・`complex.h`・uuid は無い（無くても build できる、`os.getloadavg` は無し） |
| T1 | `zlib` | zlib | `userland/packages/libs/zlib` がある（base の `libz-compat` は inflate だけなので使わない） |
| T2（既存 package） | `_ssl` `_hashlib` | OpenSSL 3.5、CA bundle | `security/openssl`・`security/ca-certificates` がある |
| T2 | `pyexpat` `_elementtree` | expat | `libs/expat` がある（同梱 copy でもよい、p001 で決める） |
| T3（新 package が要る） | `_ctypes` | libffi（MIT、台帳で 3.8.0 を照合済み） | 無い。WS115 の glib（ws034-p026）も要るので共有になる |
| T3 | `_sqlite3` | SQLite（public domain） | 無い |
| T3 | `_bz2` `_lzma` `_zstd` | bzip2・xz・zstd | 無い |
| 範囲外の案 | `readline`（GNU readline は GPL、base の libedit は一部の互換だけ）、`_curses`（base の curses の wide の API の不足が未調査）、`tkinter`、`_gdbm`、`_uuid` | — | 入れない |

cross build: CPython は同じ版の build 用の Python（`--with-build-python`）を要る。host は Python 3.13.5 なので、同じ tarball から host 用を作る（p002）。`config.sub` と `configure` の `ac_sys_system`（zedbsd の共有 library の flag、`LDSHARED`・`CCSHARED`）に patch が要る見込み。拡張 module は共有 object（`dlopen`）を既定案とし、動かなければ `Modules/Setup.local` で静的に組み込む（技術判断として p001 で決める）。

## ユーザーの判断が要る点

- **D1（module の範囲）**: 既定案は T1＋T2 を必須、T3 は `ctypes` と `sqlite3` をベータ1 に入れ、`bz2`・`lzma`・`zstd` は時間があれば。readline・curses・tkinter は範囲外。
- **D2（pip）**: `ensurepip`（同梱の wheel）を入れて `pip` を使えるようにするか。既定案は入れる（network と ssl は P3 で確かめる）が、PyPI からの取得の確認は範囲外。
- **D3（image の既定）**: WS124・WS125 と同じ（既定案は menuconfig の既定 n、release は WS129）。

## Phase

| Phase | 目的 | Status | 依存 | 目安 |
| --- | --- | --- | --- | --- |
| [ws126-p001](phase001/phase.md) | 取得・検証・license 監査、zedbsd の configure の試行、module ごとの依存の実測、cross build の方針 | cleared 候補（2026-10-09 P1） | — | 2〜3h |
| [ws126-p002](phase002/phase.md) | host 用 Python（同じ版）と、target の interpreter（`libpython`、`python3`）と T1 の cross build・stage | cleared（2026-10-09 Q1 判定。guest の試しは p005 へ）（P1 q916、guest の試しは p005 へ回す案） | p001 | 3〜4h |
| [ws126-p003](phase003/phase.md) | T2: OpenSSL（`_ssl`・`_hashlib`）と expat | cleared 候補（2026-10-09 P1 q916、guest は p005） | p002 | 2h |
| [ws126-p004](phase004/phase.md) | T3: 選んだ依存の package（`libs/libffi`・`libs/sqlite` ほか）と `_ctypes`・`_sqlite3` ほか | planning（D1 の判断待ち、libffi の所有の調整） | p002、D1 | 3〜4h |
| [ws126-p005](phase005/phase.md) | menuconfig への登録、標準 library の tree の image への導入、guest の受け入れ P1〜P8 | planning | p003（p004 を行うならそれも）、ws125-p002 | 3h |
| [ws126-p006](phase006/phase.md) | 全文規約と回帰、制限の整理（必須の最終確認） | planning | p005 | 2h |

Graph: p001 → p002 → p003 → p005 → p006、p002 → p004 → p005、ws125-p002 → p005。

## Event history

2026-10-02 / ws126-plan-detail-20261002: 計画担当が既存 package（OpenSSL・expat・zlib・curl）と libc の API を調べ、module の層・受け入れ条件・Phase を詳細化。p001 planned、Queue none。版 3.15.0 は公開の予定日に基づく提案で、取得・照合は p001。
