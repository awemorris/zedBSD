<!-- awesome-plan project=zedbsd record=ws035p036 -->

# ws035-p036: softfloatを `src/libc/` へ移し、ファイル名を整理する

Phase ID: `ws035-p036`
Parent: [WS035](../ws.md)
Status: cleared（q321-i01、2026-09-23）
Phase disposition: normal
Queue: q321（q321-i01）
実行: メインセッション

## 指示（2026-09-23ユーザー）

「softfloatは、src/libc/softfloat/に移します。あと、ファイル名を整理して。ファイルも整理して。sparcv9だけフォルダが
あるのはおかしい。フラットにしよう。それから、zed-のプレフィクスのファイル名はやめよう。」
直後に「あ、src/libc/直下でいいわ。」（`src/libc/softfloat/` ではなく `src/libc/` の直下）。

## 調査（移動前）

softfloatはkernelでは使っていない。userlandのlibc・libm専用で、`build/<arch>/dynamic/` に置かれる。
amd64・pcat・pc98が32/64 bit、arm64・sparcv9は128 bit（`long double` が128 bit）も使う。
x68kはbuildに使わず、host試験の除外設定だけ。kernelは `-mgeneral-regs-only` で浮動小数点を使わない。

## 移動と改名

| 移動前 | 移動後 |
| --- | --- |
| `src/softfloat/zed-softfloat.c`・`.h` | `src/libc/softfloat.c`・`.h` |
| `src/softfloat/zed-softfloat128.c`・`.h` | `src/libc/softfloat128.c`・`.h` |
| `src/softfloat/compiler-runtime.c` | `src/libc/compiler-runtime.c` |
| `src/softfloat/compiler-runtime128.c` | `src/libc/compiler-runtime128.c` |
| `src/softfloat/sparcv9/compiler-runtime.c` | `src/libc/compiler-runtime-sparcv9.c` |
| `src/softfloat/softfloat.mk` | `src/libc/softfloat.mk` |

- `zed-` の接頭辞を外した。
- sparcv9だけのディレクトリをやめ、`-sparcv9` の接尾辞にした（`crt0-amd64.S` 等と同じ付け方）。
- include guardを `KERN_ZED_SOFTFLOAT_H` → `LIBC_SOFTFLOAT_H`、`KERN_ZED_SOFTFLOAT128_H` → `LIBC_SOFTFLOAT128_H` へ。
- `src/softfloat/` はツリーから消えた。

## 結果

- 参照16ファイルを置換（Makefile、5 platformの `vmunix.mk`、sysroot生成、libcのsource、`tools/kstyle/kverify.sh`）。
- **build**: `build/amd64` を消した状態から `make -j32 disk-image` エラー0。`hdd-image.img` 797,966,336 byte。
  warningは1件で、OpenSSHがOpenSSLの非推奨APIを使うもの（この変更とは無関係）。
- **boot-test**: **PASS**（行頭の `login:`）。`boot-test/login.png`。
- 途中、`build/amd64` をまるごと消したため sysroot が無くなり `zedbsd-target-toolchain-ready` で1度停止した。
  `make sysroot-amd64` で解消。消す範囲は `build/<arch>/sysroot` と `dynamic` 程度に絞るほうが速い。

## 範囲外

amd64以外のbuild（arm64・sparcv9は128 bit版を使うが、buildは通らないまま。WS036）、実機。
