<!-- awesome-plan project=zedbsd record=ws035p010 -->

# ws035-p010: libtruetype

Phase ID: `ws035-p010`
Parent: [WS035](../ws.md)
Status: cleared（q323-i06、2026-09-23）
Phase disposition: normal
Queue: q323（q323-i06）
実行: メインセッション

## 範囲

TrueType フォントを読み、グリフを anti-alias で描く独自実装のライブラリ。
`cmap`・`glyf`・`hmtx` を読み、`/lib/libtruetype.so` に置く。**FreeType のコードは使わない**（Zlib）。

## 実装

| ファイル | 内容 |
| --- | --- |
| `include/libc/truetype.h` | 公開 API 7個。sysroot 経由で `/usr/include/truetype.h` に入る |
| `userland/base/libtruetype/internal.h` | face、outline、共有の宣言 |
| `face.c` | table directory、`head`・`maxp`・`hhea`、collection、寸法 |
| `cmap.c` | subtable の選択と、format 4・format 12 の探索 |
| `outline.c` | `loca`/`glyf` の読み取り、単純グリフ・複合グリフ、`hmtx` の advance |
| `render.c` | 走査線による塗りつぶしと anti-alias |
| `glyph.c` | 公開の計測・描画 |
| `exports.map` | 公開 7 シンボルだけを出す |

### 読むもの

- **table directory**: 全テーブルをファイル範囲で検査してから保持する。以降はテーブル内の索引が
  ファイル内であることを前提にできる。フォントは外から来るファイルなので、何も信用しない。
- **cmap**: format 4（BMP）と format 12（全 Unicode）。platform 3/10 と 0＋format 12 を最優先、
  次に 3/1、次に platform 0。format 12 は二分探索。
- **glyf**: 輪郭点の flag と差分座標。2次ベジエは線分へ展開する（制御点の外れ具合で分割数を決め、上限16）。
  複合グリフは深さ8まで。scale・matrix は読み飛ばして位置だけ使う（立った文字を描くため）。
- **hmtx**: `hmetric_count` を超えるグリフは最後の advance を共有する。

### 描くもの

1ピクセル行を4回サンプリングする。各サンプル行が輪郭と交わる x を集めて並べ、
**non-zero winding** で内側の区間を決める（`o` の中が空くのはこの規則による）。
区間の端のピクセルは覆われた割合、間は全部。被覆は 0..255 の 8bit で返す。

### API の形で決めたこと

`truetype_render_glyph()` は **bitmap の byte 数を引数に取る**。
グリフが要る大きさはフォントを読んで初めて分かるので、呼び出し側に計算させると
壊れたフォントが呼び出し側の memory 破壊になる（下記の検証で実際に起きた）。
入らないときは `EINVAL` を返し、**`metrics` には必要な大きさが入っている**ので、
確保し直して呼べる。

## 検証

### host fixture（`plan/ws035/tests/truetype-test.c`、27項目）

ラスタライザは他の実装とバイト比較しても意味がない（サンプリングが違えば同じ正しい形でも値が違う）。
**間違った reader が間違える所**を見る。

| 見たもの | 期待 |
| --- | --- |
| フォントでないファイル、切り詰めたファイル、無い face 番号 | 拒否 |
| 縦寸法 | ascent>0、descent<0、line_height ≥ ascent−descent |
| `I`・`o`・`.` の索引 | 0 でなく、互いに違う |
| private-use 文字 | 0 |
| `I` | ascent の3/4より高く、高さより幅が狭く、ink がある |
| **`o` の中央行** | 背景→ink→背景→ink→背景 の**4回の変化**（穴が開いている） |
| `o` の中央列 | 3分割の中央で ink が無い行がある |
| `.` | `o` より低く、`o` より上へ出ない |
| 空白 | 何も描かず、pen は進む |
| 計測と描画 | 同じ値 |
| 幅・高さの足りない bitmap | `EINVAL` |
| 倍の大きさ | より大きいグリフ |

実フォント4種で PASS（`Hack-Regular`・`Hack-Bold`・`Hack-Italic`・`VL-Gothic-Monaco`）。
`DroidSansFallbackFull` は **SKIP**（exit 77）: 空白の次が U+0E3F という fallback フォントで、
Latin を持たない。ライブラリの誤りではないので、失敗と区別する。

通常ビルドと **ASan/UBSan** の両方で PASS。

### 壊れたフォントへの耐性

1フォントにつき 3000 回、毎回ランダムな8バイトを書き換えて open→索引→描画を行う
（ASan/UBSan つき、2フォントで計6000回、約 84,000 グリフ）。
**最終形では違反 0**。

### target build

amd64 `world`: warning 0。`/lib/libtruetype.so` 15,440 byte、
SONAME `libtruetype.so`、NEEDED は `libc.so` のみ、公開シンボル 7。
`/usr/include/truetype.h` が sysroot 経由で入る。

## 検証で見つけて直したもの

1. **`line_height` が `ascent − descent` より小さくなりうる**。ascent は上へ、descent は下へ
   別々に丸めるので、合計が「丸めた合計」を超える（Hack-Regular 48px で 45−(−12)=57 に対し 56）。
   行を line_height で積むと、自分が返した箱が1ピクセル重なる。**下限を入れた。**
2. **壊れたフォントが呼び出し側の memory を壊す**。ファズが `memset` の
   global-buffer-overflow を出した。グリフの高さは描くまで分からないのに、
   API は呼び出し側が確保済みである前提だった。**bitmap の byte 数を引数にした**（上記）。
3. 試験の誤り2件: `o` の変化回数は行末が ink で終わるため3回に見えていた（行の外は背景として数える）。
   Latin を持たないフォントを失敗と扱っていた（SKIP にした）。

### 入れて、やめたもの

format 12 に ASCII が無いのを見て「フォントが複数の subtable に文字を分けている」と誤読し、
二次 subtable を引く経路を足した。実際には `DroidSansFallbackFull` は
**どちらの subtable にも Latin を持たない** fallback フォントだった。
根拠が消えたので**その経路は外した**。試験で動かせない道を残さない。

## 制限

- **hinting をしない。** 小さい字は FreeType の hinting 付きより滲む。
- **kerning をしない**（`kern`・`GPOS` を読まない）。字送りは `hmtx` の advance だけ。
- **CFF（OTTO）を読まない。** 輪郭は `glyf` のみ。
- 複合グリフの scale・matrix を適用しない。位置だけ使う。
- グリフあたり 4096 点・256 輪郭・深さ8まで。超えると `ENOTSUP`／`ENOSPC`。
- 大きさは 1..1024 px/em。グリフの箱は 2048 px まで。
- **キャッシュを持たない。** 計測してから描くと輪郭を2回読む。
  どれだけ memory を抱えるかはライブラリが決めることではない。

## 受け入れ

- `/lib/libtruetype.so` が入り、公開 API は `truetype.h` の 7 個だけ。
- host fixture 27項目が実フォント4種で PASS（通常・sanitizer）。
- 破損フォント 6000 件で sanitizer 違反 0。
- amd64 build warning 0。`git diff --check` PASS。
