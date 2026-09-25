<!-- awesome-plan project=zedbsd record=ws035p040 -->

# ws035-p040: 互換 libz

Phase ID: `ws035-p040`
Parent: [WS035](../ws.md)
Status: planning
Phase disposition: normal
Queue: なし
実行: メインセッション

## 範囲

`userland/base/libz-compat` に deflate/inflate の独自実装を置き、`/lib/libz-compat.so` を作る。
公開ヘッダは `include/libc/compat/zlib.h`（sysroot 経由で `/usr/include/compat/zlib.h`）。
使う側は `#include <compat/zlib.h>` と書く。本家は package として `/usr/include/zlib.h` へ入るので、
ファイル名は同じまま置き場だけを分ける。
**zlib のコードは使わない**（Zlib ライセンスで独自実装）。

**使うのは base のプログラムだけ**である。packages の外部プロジェクト（curl・git・
freetype など）は本家 zlib（ws034-p021）を使う（2026-09-23 ユーザー決定）。
だから gzip ファイル API のような「外部プロジェクトが要る機能」を追う必要が無い。

関数名は本家と同じ（`deflate`・`inflate` …）。`ZLIB_VERSION` は本家と同じ値
（記録上 1.3.2、`plan/ws034/package-inventory.md`）を返す。

2026-09-23 ユーザー指示: 「最適化はあまりしなくてよい。deflate/inflate を素直に実装する。
zlib のすべての機能を実装する必要はない」。

## 実装するもの（案）

| 区分 | 内容 |
| --- | --- |
| stream | `z_stream`、`deflateInit_`／`deflateInit2_`／`deflate`／`deflateEnd`、`inflateInit_`／`inflateInit2_`／`inflate`／`inflateEnd`、`deflateReset`／`inflateReset` |
| 一括 | `compress`／`compress2`／`compressBound`／`uncompress` |
| 検査 | `crc32`／`adler32` |
| 包み | zlib（RFC 1950）、raw deflate（windowBits 負）、gzip（windowBits +16） |

`inflate` は動的ハフマンを含む全ブロック型を読む。`deflate` は**出力が正しければよい**ので、
まずは stored と固定ハフマンで始め、動的ハフマンは後から足してよい。
`Z_NO_FLUSH`／`Z_SYNC_FLUSH`／`Z_FINISH` を扱う。

## 実装しないもの（案）

`gzopen` 等の gzip ファイル API、`deflateSetDictionary`／`inflateSetDictionary`、
`inflateBack`、`deflateParams`、`deflateTune`、`gzprintf`、`z_off64_t` 系の 64bit 別名。
必要が出たときに足す。

## 検証（案）

- **host fixture**: 自分で deflate したものを自分で inflate して元に戻る（round trip）。
  種類の違う入力（全部同じ byte、乱数、テキスト、空、1 byte、window より長いもの）。
  3つの包み（zlib・raw・gzip）。`crc32`／`adler32` を既知の値と照合。
  **host の zlib で deflate したものを自分の inflate が読めること**と、
  **自分が deflate したものを host の zlib が読めること**（相互運用）。
- 壊した入力を食わせて、sanitizer 違反を出さず `Z_DATA_ERROR` を返すこと。
- amd64 build warning 0。

## 決まっていること

[ws.md](../ws.md) の「互換ライブラリの決定」を参照。名前・SONAME・ヘッダ・本家との使い分け・
優先順位はすべて決定済み。**未決定は無い。**

**いつ入れるか**: zdesktop が要るようになったとき（D6）。先に作り置きしない。
p041（libpng-compat）の前提なので、実際には p041 と一緒に入る。
