<!-- awesome-plan project=zedbsd record=ws035p035 -->

# ws035-p035: kernelのinclude整理と検査

Phase ID: `ws035-p035`
Parent: [WS035](../ws.md)
Status: cleared（q318-i02、2026-09-23。結果は [results.md](results.md)、rootによる補完は下記）
Phase disposition: normal
Queue: q318（q318-i02）
実行: `phase-runner`（Opus 5.5、High）

## 範囲・手順・受け入れ

正本は [kcrt-design.md](../kcrt-design.md) の §10.2（範囲、手順1〜8、受け入れ条件）。ただし下の決定で §7（Vulkan）を置き換える:
Vulkanのヘッダ（`libc/include/vulkan/` 一式）は `include/libc/vulkan/` へ移し（p023に先行してVulkanだけ移す）、i915のdriverは
`#include <libc/vulkan/vulkan_core.h>` のように、今読んでいるヘッダをそのまま `libc/` 接頭辞付きで読む（1本に寄せる必要はない）。
kernelのinclude検査は `include/libc/vulkan/` 以下を許可し、他のlibcヘッダは0件を要求する。userland・sysroot・rootfsでは従来どおり
`<vulkan/vulkan.h>` で読めるよう、`/usr/include/vulkan/` へコピーする。libcの残りのヘッダは、この Phase では
`libc/include/` のまま（`include/libc/` への移動はp023）。`render/vulkan-codec.inc` と生成器は、Vulkan構造体を扱う
設計のまま変えない（commandがVenus由来のVulkan構造体であるのは意図した設計）。

## ユーザー決定（2026-09-23、設計より優先）

- **kernelとlibcの関係（2026-09-23ユーザー明確化・訂正）**: このOSはkernelとlibcが完全にモノリシックである。
  Vulkanのヘッダはlibcの一部で、`include/libc/vulkan/` に置く。**kernelがincludeしてよい `libc/` のヘッダは `libc/vulkan/*` だけ**
  （必要なヘッダを直接includeしてよい）。それ以外のlibcヘッダはkernel・driver・HALからincludeしない。
  commandの中身がVenus由来のVulkan構造体であるのは意図した設計であり、i915 driverがVulkanヘッダを読むのは正しい。
  `userland/base/libvulkan` はoptionのpackageではなく、必須の構成要素がbuild単位に分かれているだけである。
  kernelで除くのは、接頭辞なしの標準Cヘッダ名（`<stdio.h>`・`<string.h>` 等）による暗黙の読込みと、libcのobjectのlinkである。
  `I915_STREAM_MAGIC` の経路は試験用としてUAPIに残す。
- libcの公開ヘッダの最終的な置き場所は `include/libc/`（p023で移す）。sysrootとrootfsへは `/usr/include/` 直下にコピーする。
- 承認済み: kernel・HAL共通のcompile flagの変更（`-nostdlibinc`、`-fno-builtin`）、`include/uapi/hosted.h` とfixtureへの
  `-DKERN_UAPI_NATIVE`、明示的な `memcpy`・`memset` の `kern_*` への改名、`locale-record.c` の削除、`hal_memset` 4箇所の置換。
  HALは `#include` のパス変更とcompile flagだけ。HALの宣言・実装・責務は変えない。
- amd64以外のbuildは壊れてもよい。

## rootによる補完（2026-09-23）

サブエージェントはユーザー指示で停止した。残っていた確認をrootがメインセッションで実行した。

- **kernel build**: 最終ツリーから `BUILD=build/p035-check` でamd64 vmunixをbuild。warning 0、`amd64 vmunix check: PASS`、
  recipe内の `check-kernel-includes` もPASS。
- **fixture回帰（46件）**: 基準ツリー（`1625884c` の複製 `/tmp/p035/baseline-head`）と最終ツリーの両方で実行し、
  終了状態を突き合わせた。**46件すべてsame、差異0**。成功17件、以前から失敗29件（`perf.c` 未link、削除済みファイル参照等。
  p035とは無関係で、WS026・WS031が担当）。サブエージェントが一度壊したws031 display 6件も基準と同じ失敗に戻っている。
  記録は `host-tests-final/summary.tsv`。
- **disk-image**: `make -j32 disk-image`（ユーザーのconfig.mk）がEXIT 0、797,966,336 byte。
- **boot-test**: `plan/tools/boot-test.sh` が **PASS**。画面の行頭に本物の `login:` prompt。
  PNG `boot-test-final/login.png`、文字 `login.txt`。判定を行頭のpromptに限る修正後の実行で、誤判定ではない。

未達のまま残るもの: `directory-fsync-host-test.mk`（削除済み `io-stats.c` を参照。移動前から失敗。WS026）。
p023への引き継ぎ事項（libcのinclude guard名、108 fixtureの `-Ilibc/include`）はresults.mdのとおり。
