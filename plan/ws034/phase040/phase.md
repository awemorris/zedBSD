<!-- awesome-plan project=zedbsd record=ws034p040 -->

# ws034-p040: libc の iconv（UTF-8 と ASCII）

Phase ID: `ws034-p040`
Parent: [WS034](../ws.md)
Status: **cleared**（q331-i01、2026-09-23）
Phase disposition: normal
Queue: q331（q331-i01）
実行: メインセッション

## なぜ

ws034-p005（libc・カーネル是正の受け皿）に集めた不足のうち、iconv を1つの Phase として切り出した。
glib が必須とし、wget の IRI・git・VLC・gdb も使う（inventory §3.7）。
**2026-09-23 のユーザー決定: iconv は libc に実装する（ASCII と UTF-8 だけ）。** GNU libiconv は使わない。

## 入れたもの

| 場所 | 内容 |
| --- | --- |
| `include/libc/iconv.h` | POSIX の `iconv_t`・`iconv_open`・`iconv`・`iconv_close` |
| `src/libc/iconv.c` | UTF-8 と ASCII の相互変換。名前は大文字小文字を区別せず `UTF-8`・`UTF8`・`ASCII`・`US-ASCII`・`ANSI_X3.4-1968`・`646`、空の名前は locale の文字集合（UTF-8）。変換先の `//TRANSLIT`（持てない文字を `?`）と `//IGNORE`（持てない文字と不正な入力を捨てる）。どちらも戻り値の「元に戻せない変換」の数に数える。それ以外の文字集合は `EINVAL` |
| `src/libc/libc.mk` | `ZEDBSD_LIBC_USER_EXTRA_SOURCES` に `iconv.c`（全 platform の静的・動的 libc に入る） |
| `plan/ws034/tests/` | `iconv-test.c`・`run-iconv-host-test.sh`（host 試験）、`iconv-target.c`（ゲストの smoke） |

UTF-8 は RFC 3629 どおり厳密に検査する: overlong、surrogate、U+10FFFF 超は `EILSEQ`。
先頭2 byte で「決して正しくならない」と分かった時点で `EILSEQ` にし、続きを待たない。
入力が文字の途中で終われば `EINVAL`、出力が足りなければ `E2BIG`（その文字は読まずに残す）。
どちらの文字集合も状態を持たないので、`iconv(cd, NULL, ...)` は 0 を返すだけである。

## 検証

| 検証 | 結果 |
| --- | --- |
| host 試験（ASan・UBSan） | PASS。名前の受理と拒否、1〜4 byte と境界（U+007F・U+0080・U+07FF・U+0800・U+FFFF・U+10000・U+10FFFF）、不正な入力8種、途中で終わる入力、`E2BIG`、`//TRANSLIT`・`//IGNORE`、ASCII 側 |
| **host の glibc との突き合わせ** | 乱数の入力 200,000 件を、UTF-8→UTF-8 と UTF-8→ASCII で両方に変換させた。170,328 件で戻り値・errno・読んだ量・書いた量・出力がすべて一致。残り 29,672 件は **glibc の方が緩い**もの（U+10FFFF 超を受け取る、または決して正しくならない先頭2 byte の後に続きを待つ）で、試験が分類して数える。それ以外の食い違いは0 |
| amd64 `world`・`disk-image` | PASS。`libc.so` が `iconv_open`・`iconv`・`iconv_close` を出す。sysroot に `iconv.h` |
| ゲスト（QEMU、KVM） | `ICONV PASS output=a??! irreversible=2`（`a`・`é`・`€`・`!` を `ASCII//TRANSLIT` へ）。`ISO-8859-1` は `EINVAL` |

証拠: `evidence/`。

## 気づいたこと

- 試験用の program を cross toolchain で link すると、`build/amd64/dynamic`（既定の build 木）の `libc.so` を使う。
  別の `BUILD=` で作った新しい libc を試すときは、その `libc.so` を link の入力に直接渡す必要がある
  （toolchain の wrapper が自分の `-L` を先に置くため）。
- base の `iconv` コマンド（`userland/base/iconv`）は独自の変換を持っている。この libc の iconv を使う形へ
  寄せるのは別の作業とし、今回はしていない。
