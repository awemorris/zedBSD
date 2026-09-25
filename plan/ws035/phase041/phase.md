<!-- awesome-plan project=zedbsd record=ws035p041 -->

# ws035-p041: 互換 libpng

Phase ID: `ws035-p041`
Parent: [WS035](../ws.md)
Status: planning
Phase disposition: normal
Queue: なし
実行: メインセッション
依存: p040（libz）

## 範囲

`userland/base/libpng-compat` に PNG の読み書きの独自実装を置き、`/lib/libpng-compat.so` を作る。
公開ヘッダは `include/libc/compat/png.h`（`/usr/include/compat/png.h`）。
使う側は `#include <compat/png.h>` と書く。
**libpng のコードは使わない**（Zlib ライセンスで独自実装）。圧縮は p040 の `libz-compat` を使う（`#include <compat/zlib.h>`）。

**使うのは base のプログラムだけ**である。GTK・Qt・freetype は本家 libpng（ws034-p027）を使う
（2026-09-23 ユーザー決定）。`PNG_LIBPNG_VER_STRING` は本家と同じ値
（記録上 1.6.58）を返す。

2026-09-23 ユーザー指示: 「decode と encode。encode は filter とかこだわらなくていい。
decode も、テスト用に用意する RGBA32 の png がロードできる、正常系が通るくらいの荒い実装でよい」。

## 実装するもの（案）

### decode

- signature、`IHDR`、`PLTE`、`IDAT`、`IEND`。未知の chunk は飛ばす。CRC を検査する。
- **色形式は RGBA8（色型6・bit深度8）を確実に**。他（grayscale、palette、RGB、16bit、alpha無し）は
  足せる範囲で足し、対応していないものは明示して失敗する。
- filter 5種（None・Sub・Up・Average・Paeth）は**読む側は全部要る**。書く側が何を選ぼうと、
  外から来る PNG はどれでも使ってくるため。
- interlace（Adam7）は**対応しない**。そう宣言して失敗する。

### encode

- RGBA8 を書く。filter は **None 固定でよい**（ユーザー指示）。
- `IHDR`／`IDAT`／`IEND`、CRC、zlib 包み。

## 実装しないもの（案）

`tRNS`・`gAMA`・`sRGB`・`iCCP` 等の補助 chunk の解釈（読み飛ばす）、Adam7、
progressive read API（`png_process_data`）、`setjmp` によるエラー処理の完全な再現、
16bit、palette の書き出し。

## API の形（決定、2026-09-23 ユーザー）

**simplified API だけを作る**（`png_image`／`png_image_begin_read_from_file`／
`png_image_finish_read`／`png_image_write_to_file`）。
RGBA32 を読む・書くだけならこれで足り、構造体も1つで済む。

**従来 API**（`png_create_read_struct` 等）は作らない。それを使う GTK・Qt・freetype は
本家を使うためである。自分たちの base のプログラムで足りないものが出たときに、
必要な分だけ足す。そのときは `png_create_read_struct()` の版照合に
`PNG_LIBPNG_VER_STRING` を合わせる。

## 検証（案）

- **host fixture**: 自分で書いた PNG を自分で読んで画素が一致する（round trip）。
  **host の Python（zlib＋自前の展開）または `pngcheck` 相当で、書いた PNG が正しいと確かめる**。
  host にある実物の PNG を読んで、寸法と既知の画素が一致する。
- filter 5種それぞれを使った PNG を読んで一致する（入力は host 側で作る）。
- 壊した入力（CRC 不一致、途中で切れた IDAT、寸法 0、巨大な寸法）で
  sanitizer 違反を出さずに失敗する。
- amd64 build warning 0。

## 決まっていること

[ws.md](../ws.md) の「互換ライブラリの決定」を参照。名前・SONAME・ヘッダ・API の範囲・
本家との使い分け・優先順位はすべて決定済み。**未決定は無い。**

**いつ入れるか**: zdesktop が PNG の画像（アイコン等）を扱う段（p013 のタスクバー、
p025 のフレーム描画あたり）に着手するときに、p040 と一緒に前へ入れる（D6）。
