<!-- awesome-plan project=zedbsd record=ws035p034 -->

# ws035-p034: kcrtとheap、vmunixへのlibcのlinkをやめる

Phase ID: `ws035-p034`
Parent: [WS035](../ws.md)
Status: cleared（q318-i01、2026-09-23。結果は [results.md](results.md)）
Phase disposition: normal
Queue: q318（q318-i01）
実行: `phase-runner`（Opus 5.5、High）

## 範囲・手順・受け入れ

正本は [kcrt-design.md](../kcrt-design.md) の §10.1（範囲、手順1〜6、受け入れ条件）。設計の §3（kcrt API と置換規則）、§4（`kern_snprintf`）、
§5（compilerの暗黙の呼出し）、§8.1（`src/kern/heap.c`）、§13（実装の出発点）に従う。
この Phase ではヘッダの読み方（`-isystem sysroot`）は変えない。変えるのは次の p035。

## ユーザー決定（2026-09-23、設計より優先）

- **kernelとlibcの関係（2026-09-23ユーザー明確化）**: このOSはkernelとlibcが完全にモノリシックである。driverがlibcを
  参照することは許される。Vulkanのヘッダはlibcの一部で、`include/libc/vulkan/` に置く。driverは `<libc/vulkan/vulkan.h>` を
  includeしてよい。`userland/base/libvulkan` はoptionのpackageではなく、必須の構成要素がbuild単位に分かれているだけである。
  kernelで除くのは、接頭辞なしの標準Cヘッダ名（`<stdio.h>`・`<string.h>` 等）による暗黙の読込みと、libcのobjectのlinkである。
- libcの公開ヘッダの最終的な置き場所は `include/libc/`（p023で移す）。sysrootとrootfsへは `/usr/include/` 直下にコピーする。
- 承認済み: kernel・HAL共通のcompile flagの変更（`-nostdlibinc`、`-fno-builtin`）、`include/uapi/hosted.h` とfixtureへの
  `-DKERN_UAPI_NATIVE`、明示的な `memcpy`・`memset` の `kern_*` への改名、`locale-record.c` の削除、`hal_memset` 4箇所の置換。
  HALは `#include` のパス変更とcompile flagだけ。HALの宣言・実装・責務は変えない。
- amd64以外のbuildは壊れてもよい。
