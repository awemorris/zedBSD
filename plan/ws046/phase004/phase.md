<!-- awesome-plan project=zedbsd record=ws046p004 -->

# ws046-p004: 実 package を guest で build

Phase ID: `ws046-p004`
Parent: [WS046](../ws.md)
Status: cleared（q404-i01、2026-09-24。guest の compile の遅さは BUG-033 として ws046-p007 に引き継ぎ、残りの package の build は ws046-p008 に）
Queue: q404（q404-i01）

## 目的

guest（amd64、QEMU。zedBSD の `/bin/sh`・utility・`/usr/bin/make`・clang）で、autotools の package を
`./configure && make && make check && make install DESTDIR=...` する。WS042 の残り（expat の build）もここで確かめる。

## 対象

1. expat 2.8.5（automake・libtool、依存の追跡あり。`config.sub` に `zedbsd*` を足し `--build=x86_64-unknown-zedbsd`、ws042-p005 と同じ）。
2. zlib 1.3.2（手書きの configure と Makefile）。
3. coreutils 9.12（gnulib を含む大きな automake の package。調べる範囲を決めて進め、止まる所は理由を記録する）。

## 受け入れ

- expat と zlib が guest で configure・build・check・install まで status 0。install される file の一覧が host（GNU make）と同じか、違いが system の違いで説明できる。
- coreutils は止まる所まで進め、make の不具合は直し、他の不具合（sh・utility・kernel・libc・compiler）は分類して記録する。
- make の差分試験 91/91 が下がらない。

## 結果（q404-i01、2026-09-24）

2026-09-24 ユーザー判断: 「これはコンパイルできたとしても遅すぎます。issueとして扱います。…現在のphaseはclearedにしますが、課題が残ったので引き継ぐ形にしましょう。」
guest の compile の遅さを [BUG-033](../../bugs/BUG-033.md) にし、解決を ws046-p007 に、guest での package の build の残りを ws046-p008 に引き継いで cleared とした。

### 確かめたこと

| 検証 | 結果 |
| --- | --- |
| host で coreutils 9.12 を我々の make で configure・build | status 0、109 の program ができて動く（`evidence/host-coreutils-status.txt`） |
| host で coreutils の `make check`（我々の make） | coreutils の試験: PASS 609・SKIP 152・FAIL 0。gnulib の試験: PASS 496・SKIP 106・FAIL 1（`test-update-copyright.sh`）。この 1 件は GNU make でも同じく失敗し、host の perl が `ja_JP.UTF-8` の locale を持たないため（環境）（`evidence/host-coreutils-check.txt`） |
| host で expat（p003 で確かめ済み） | build・check（1/1）・install、install の file の一覧が GNU make と同じ |
| guest で expat の `./configure`（`--build=x86_64-unknown-zedbsd`、依存の追跡あり） | status 0。make の 3 つの検査（`$(MAKE)`、入れ子の変数、include）が yes |
| guest で expat の `make` | 再帰の make → libtool（我々の `/bin/sh`）→ clang の連鎖が動き、`lib/` の 8 つの `.lo` ができた。約 51 分で止め、終わっていない（BUG-033） |

### 引き継ぎ

- **ws046-p007**（BUG-033）: guest の clang の遅さを測って直す。
- **ws046-p008**: guest で expat の build・check・install を最後まで、zlib、coreutils（p004 の残り。WS042 の残りの expat の build も）。p007 の後。
