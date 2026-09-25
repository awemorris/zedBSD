<!-- awesome-plan project=zedbsd record=ws035p023 -->

# ws035-p023: `libc/include/*` を `include/libc/` へ移す

Phase ID: `ws035-p023`
Parent: [WS035](../ws.md)
Status: cleared（q320-i01、2026-09-23）
Phase disposition: normal
Queue: q320（q320-i01）
実行: メインセッション

## 範囲

`libc/include/` の139ファイル（95 entry）を `include/libc/` へ移す。sysroot・rootfsへは従来どおり
`/usr/include/` の直下へ入れる（`toolchain/llvm/sysroot.mk` は `include/libc/*` の規則を既に持っていた）。
Vulkanはp035で先に移済み。参照の置換。計画の記録は書き換えない。

## 結果（2026-09-23）

- **移動**: 139ファイル（`git mv`）。`libc/` はツリーから消えた。
- **参照の置換**: 206ファイル。うち148は素直な置換、58は `-Ilibc/include` のように `-I` が直前に付く形で、
  最初の置換が単語境界の判定で取りこぼしていた分。重複した `-Iinclude/libc` も1つにまとめた。
- **build**: `make -j32 disk-image` EXIT 0、`hdd-image.img` 797,966,336 byte、`vmunix` 2,110,120 byte。
  kernel単体のbuildもwarning 0。残る警告は外部package（OpenSSL、LLVM）のsource由来。
- **include監査**: `--require-none` **PASS**（kernel: libc 0、libc-vulkan 3、uapi 56。HAL: libc 0）。
- **boot-test**: **PASS**（行頭の `login:`）。`boot-test/login.png`。
- sysrootのヘッダが `/usr/include/` 直下に入ることを確認（`aio.h`、`arpa/`、`X11/` 等）。

## 途中で直した取りこぼし（すべてbuildの失敗として現れた）

1. **`userland/base/libc/*.c` の誤変換**（p003由来、24ファイル）: `libc/<名前>.c` の形に一致して
   `userland/base/src/libc/*.c` に化けていた。p003では既存のsysrootが使われて表に出ず、sysrootを作り直して発覚。元に戻した。
2. **object path・pattern rule**（6 platform）: `$(BUILD)/libc/heap.o`、`libc/%.S` のように `.c`/`.h`/`.mk` 以外の形。
   `src/libc/` を指すよう直した。
3. **noctの展開済みsource**: patchが変わったため `refusing to replace existing source tree` で停止。作り直した。
4. **noctのpatch**: `0001` の必要ファイル一覧（このリポジトリのパス）は `include/libc/stdint.h` へ直す必要があり、
   `0002` の削除行（上流ファイルの元の中身）は変えてはいけない。一括置換が両方を同じに扱っていたため、
   0002を元に戻し、0001をpatchとして正しい形に書き直した。
5. **include監査の分類順**: `include/libc/` の判定が `include/libc/vulkan/` より先にあり、Vulkanがlibc扱いになっていた。
   順番を直した（`kernel-include-audit.py`）。

## 学び

refactorの確認は、build成果物（sysroot、`build/<arch>`、展開済みsource）を消した状態で行う。
既存の成果物があると、取りこぼしが隠れる。またroot自身が共有の `build/NoctLang` を誤って消し、host用noctの
再buildが必要になった。消す対象は限定する。

## 範囲外

bootヘッダの移動（p004）、amd64以外のbuild、実機。host fixtureの46件は流していない（移動だけのPhaseのため）。
