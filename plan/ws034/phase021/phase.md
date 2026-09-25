<!-- awesome-plan project=zedbsd record=ws034p021 -->

# ws034-p021: zlib・expat（`packages/libs`）

Phase ID: `ws034-p021`
Parent: [WS034](../ws.md)
Status: **cleared**（q329-i01、2026-09-23）
Phase disposition: normal
Queue: q329（q329-i01）
実行: メインセッション

## 範囲

本家の zlib と expat を外部 package として入れる。版・入手元・検証値は ws034-p001 の
`plan/ws034/package-inventory.md`（zlib 1.3.2、expat 2.8.5、どちらも署名確認済み）。
base のプログラムは WS035 の libz-compat を使い、packages の外部プロジェクトは本家の zlib を使う（WS035 D2）。

## 入れたもの

| 場所 | 内容 |
| --- | --- |
| `userland/packages/libs/zlib/` | CMake（WS032 の `zedbsd.cmake` toolchain）で shared だけを build。patch 1件: `CMakeLists.txt` の version script を zedBSD では付けない（loader が symbol version を読まず、`check-dynamic-elf.py` が拒否するため。inventory §3.3）。static・test は作らない |
| `userland/packages/libs/expat/` | CMake。symbol versioning は既定で off のまま。`xmlwf` を入れ、examples・tests・manual（docbook が要る）は作らない。entropy は `getrandom`・`arc4random` が sysroot で見つかる |
| `Makefile` | `ZEDBSD_PACKAGE_LINKS`（`DEST=TARGET`）。rootfs の tree の規則だけが読み、symlink を作る。config stamp にも入れたので、link が変われば tree を作り直す |
| `plan/ws034/tests/` | `zlib-roundtrip.c`（圧縮して戻して比べる）、`config-amd64-libs.mk`（userland の試験 config に zlib・expat を足す） |

### 版付き SONAME の載せ方（inventory §3.4 の決定）

**library の実体を SONAME の名前で置き、`libfoo.so` を SONAME への symlink にする。**

- `/usr/lib/libz.so.1`（実体）、`/usr/lib/libz.so` → `libz.so.1`
- `/usr/lib/libexpat.so.1`（実体）、`/usr/lib/libexpat.so` → `libexpat.so.1`

loader は `DT_NEEDED` の名前（SONAME）でファイルを開くので symlink をたどらずに済み、target 上の compiler は
`-lz` で `libz.so` を見つける。完全な版の名前（`libz.so.1.3.2`）は置かない。
置き場所は `/usr/lib`（rtld は `/lib` の次に `/usr/lib` を探す）。header・`.pc`・licence も `/usr` の下に置く。
OpenSSL（WS032）が `/lib` に置いている件は ws034-p037 で揃える。

package 同士で header と library を見せる仕組み（package prefix、pkg-config wrapper）は ws034-p025 の範囲で、
今は各 package の stage（`build/packages/<name>/stage/usr`）にある。

## 検証

| 検証 | 結果 |
| --- | --- |
| `make zlib`・`make expat` | PASS。`check-dynamic-elf.py`: SONAME `libz.so.1`・`libexpat.so.1`、NEEDED は `libc.so` だけ、symbol version の section 無し。`xmlwf` は `libexpat.so.1` と `libc.so` |
| amd64 `disk-image`（`config-amd64-libs.mk`） | PASS |
| ゲスト（QEMU、KVM）で `zlib-roundtrip` | `ZLIB PASS version=1.3.2 in=65536 packed=2748 crc32=d7cc0bfc`。**crc32 と圧縮後の大きさが host の Python zlib と一致** |
| ゲストで `xmlwf` | 正しい XML は exit 0、閉じていない tag は `/root/bad.xml:1:22: mismatched tag` で exit 2。`xmlwf -v` は `expat_2.8.5` |
| ゲストの UFS に symlink | `ls -l /usr/lib` で `libz.so -> libz.so.1`、`libexpat.so -> libexpat.so.1` |

証拠: `evidence/`。

## 残したこと

- i386（pcat）では build していない（受入環境は QEMU amd64、WS034 決定4）。
- ライセンスの機械監査（全文検索）はしていない。zlib は Zlib、expat は MIT（inventory の判定）で、
  licence の文面を `/usr/share/licenses/<name>/` に入れた。
