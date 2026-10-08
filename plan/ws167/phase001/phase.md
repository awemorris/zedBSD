<!-- awesome-plan project=zedbsd record=ws167-p001 -->

# ws167-p001: GPU の command の protocol の独自化（要件と設計）

Phase ID: `ws167-p001`
Parent: [WS167](../ws.md)
Status: cleared（2026-10-08 Q1 の判定（sweep-beta2-rc2 §2）: H1〜H3 決定済み、p002 が cleared）（旧: planning（2026-10-05 P1 generation17、q730。設計の第 1 版。code は §6 のユーザーの判断（license と UAPI の置き場所）の後。2026-10-05 夜: H1〜H3 決定））
Phase disposition: normal
Queue: q730（ベータ2 の P1 の列の 2 番目）

## 範囲

- ユーザーの指示（2026-10-05、原文）:「GPUのコマンドが、Venusプロトコル番号を流用しているので、独自の名前と番号にする。ただしVenusと一致している内容からスタートする。Venusの番号を再利用したことをヘッダに書いて、Googleの著作権表示を外せるようにする。」
- 入る: GPU の command（libvulkan が GPU の node に送る Vulkan の command の stream の command の番号）の、独自の名前の header（版 1 は番号が Venus と同じ）。
  その header を libvulkan・i915 の Vulkan の実行器・`venus-frame` の 3 か所が共有する。Google の著作権の表示と `LICENSE-PROTOCOL` を外せる形にする。
  license の記録（`license-components.json`・`API-PROVENANCE.md`）の直し。
- 入らない: 番号を Venus と違う値にすること（QEMU の host の renderer は virglrenderer の Venus なので、番号を変えると QEMU で GPU が動かなくなる。
  §4）。record（struct）の符号化の形の変更。kernel の virtio-gpu の Venus の driver（`src/drivers/gpu/venus/`）の名前（virtio-gpu の capset の名前で、
  host との約束）。

## 1. 今の形（2026-10-05 の main を読んだ）

| 場所 | 今 | 番号の出どころ |
| --- | --- | --- |
| `userland/desktop/libvulkan/opcodes.h` | `enum vulkan_opcode { VULKAN_OPCODE_vkCreateInstance = 0, …, VULKAN_OPCODE_vkExecuteCommandStreamsMESA = 180 }`、145 個。file の先頭に「virglrenderer 1.1.0 の Venus protocol から選んだ数値の宣言、Copyright 2020 Google LLC、MIT」 | `tools/maintain-dispatch.noct` が virglrenderer の `vn_protocol_renderer_defines.h`（commit `1aeaf5e1…`）の `VK_COMMAND_TYPE_*_EXT` を読んで生成。生成の文字列に Google の表示が入っている |
| `userland/desktop/libvulkan/LICENSE-PROTOCOL` | 「opcodes.h の数値の宣言は virglrenderer から選んで名前を変えた。renderer の実装は含まない」と MIT の全文（Copyright 2020 Google LLC）。package が `/usr/share/licenses/libvulkan/LICENSE-PROTOCOL` に入れる | — |
| libvulkan の 14 の source | `VULKAN_OPCODE_*` を 87 か所で使う（`sync.c`・`pipeline.c`・`resources.c`・`instance.c`・`descriptors.c`・`commands.c`・`context.c` ほか） | opcodes.h |
| i915 の Vulkan の実行器 `src/drivers/gpu/i915/render/` | command の番号を **数字のまま** `case 0U: /* vkCreateInstance */` の形で 61 か所（`command.c` 40・`instance.c` 14・`fence.c` 7）。`internal.h` に「Venus の wire の opcode を Vulkan の serialization としてだけ再利用」 | libvulkan と同じ値（header を共有していない） |
| `userland/gpu/venus/client.c`（`venus-frame`、診断の道具） | 自分の `enum venus_command`（0・2・7・8・11・137・155・178・179 の 9 個） | 同じ値の写し |
| `include/libc/vulkan/API-PROVENANCE.md` | 公開の Vulkan の header（Khronos、1.3.269）と、opcodes.h の数値の出どころ（virglrenderer の file と SHA-256）を記録 | — |
| `tools/release/license-components.json` | 「libvulkan's Venus protocol declarations (virglrenderer 1.1.0, Google)」の項（109〜120 行）、LICENSE-PROTOCOL を install する | — |
| capset（`context.c` 27〜、i915 `vulkan.c` 275〜） | GPU の capset の id 4（virtio-gpu の Venus の capset）、先頭の wire の版と vk.xml の版、zedBSD の 168 byte の拡張（magic `0x5a424453`） | virtio-gpu の約束 |

番号以外（record の符号化）は zedBSD の実装（`codec.c`、i915 は `vulkan-codec.inc` に生成）で、virglrenderer の code は入っていない（LICENSE-PROTOCOL の記述）。
Google の表示が付いているのは **番号の表（opcodes.h）とその生成の道具の文字列** だけ。

## 2. 設計

### 2.1 新しい header

`include/uapi/gpu-op.h`（新）。kernel（i915 の実行器）と userland（libvulkan・`venus-frame`）が共有するので UAPI に置く（`include/uapi/gpu*.h` の並び。
GPU の node に `GPU_COMMAND`・`GPU_COMMAND_SUBMIT`（`include/uapi/gpu.h` 53・58）で渡す stream の中身の約束）。名前の前置きは、既にある ioctl の
`GPU_COMMAND`・`GPU_COMMAND_SUBMIT`・`GPU_COMMAND_WAIT`・`GPU_COMMAND_MAX` とぶつからないよう `GPU_OP_` にする。

```c
/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Kei GPU command protocol, version 1: the numbers of the Vulkan
 * commands in the stream libvulkan submits to a GPU node.
 *
 * The numbers of version 1 reuse those of the Venus protocol's wire
 * format 1 (virglrenderer 1.1.0) for the same Vulkan commands, so that a
 * Venus renderer on a virtual machine's host accepts the stream as it
 * is.  The names, this file and every later number are zedBSD's own.
 */

enum gpu_op {
	GPU_OP_CREATE_INSTANCE = 0,			/* vkCreateInstance */
	GPU_OP_DESTROY_INSTANCE = 1,			/* vkDestroyInstance */
	...
	GPU_OP_GET_DEVICE_QUEUE2 = 155,			/* vkGetDeviceQueue2 */
	...
	GPU_OP_SET_REPLY_STREAM = 178,			/* the stream's replies go to a resource */
	GPU_OP_SEEK_REPLY_STREAM = 179,			/* moves the reply position */
	GPU_OP_EXECUTE_STREAMS = 180			/* runs command streams held in resources */
};

#define GPU_OP_PROTOCOL_VERSION		1U
```

- 名前: `GPU_OP_` と、Vulkan の command の名前から `vk` を除いて大文字・下線にした物（`vkCreateInstance` → `GPU_OP_CREATE_INSTANCE`）。
  Vulkan の command でない 3 つ（Venus の MESA の拡張の command）は、働きを表す独自の名前（`SET_REPLY_STREAM`・`SEEK_REPLY_STREAM`・`EXECUTE_STREAMS`）。
  名前の対応は各行の comment の Vulkan の名前（Khronos の名前）だけで表し、Venus の名前（`VK_COMMAND_TYPE_*_EXT`）は使わない。
- 番号: 版 1 は今の opcodes.h の 145 個と全部同じ（§4 の理由）。将来 zedBSD だけの command を足す時は、Venus が使っていない範囲（例: `0x10000` から）に
  置く（Venus の renderer が知らない番号は host の Venus には送らない。i915 の実行器だけが受ける）。
- header の comment に「版 1 の番号は Venus の wire format 1 の番号を再利用した」と書く（ユーザーの指示）。Google の著作権の表示は書かない（§3、§6 の H1）。

### 2.2 3 か所の置き換え

| 場所 | 変更 |
| --- | --- |
| libvulkan | `opcodes.h` を消し、`#include <uapi/gpu-op.h>`。`VULKAN_OPCODE_vkX` の 87 か所を `GPU_OP_X` に（機械的な置き換え、名前の対応表は header から作る）。`LICENSE-PROTOCOL` を消し、package の install から外す |
| `maintain-dispatch.noct` | virglrenderer の file を読む部分（`PINNED_PROTOCOL_DEFINES` の引数と opcodes.h の生成）を消す。dispatch の表（`dispatch-table.inc`・`api-commands.tsv`）の生成は Khronos の公開の header だけから今のまま。番号の真実は `gpu-op.h` 自身（もう生成しない） |
| i915 の実行器 | `case 0U: /* vkCreateInstance */` の 61 か所を `case GPU_OP_CREATE_INSTANCE:` に。数字の写しが消え、libvulkan と同じ header を見る。`internal.h` の comment を「Kei GPU command protocol（`uapi/gpu-op.h`）」に |
| `venus-frame` | `enum venus_command` を消して `gpu-op.h` を使う（program の名前・Venus の capset の話は host との約束なので今のまま） |
| license の記録 | `license-components.json` の Venus の項を消す。`API-PROVENANCE.md` の「opcodes.h の数値の出どころ」の節を「版 1 の番号は Venus の wire format 1 と同じ値（再利用）、表は zedBSD の `gpu-op.h`」に書き換える。`kei-nightly` の THIRD-PARTY（QEMU・virglrenderer そのものの配布）は別の話で変えない |

置き換えの前後で **送る byte が 1 つも変わらない** ことを確かめる（§5）。

### 2.3 変えない物

- record の符号化（`codec.c`・`vulkan-codec.inc`）、capset の id と形、kernel の virtio-gpu の Venus の driver の名前と transport。
- 公開の Vulkan の header（`include/libc/vulkan/`、Khronos の Apache-2.0 / MIT）。

## 3. license（Google の表示を外せる理由の案）

- Google の表示（MIT）が要るのは、virglrenderer の「著作物」を写した時。今 tree にあるのは、Vulkan の command の名前と、それに割り当てた **整数の対応**
  だけ（LICENSE-PROTOCOL の記述どおり「数値の宣言を選んで名前を変えた」）。
- 案: (1) 番号は相互運用のための interface の事実（どの Vulkan の command が何番か）で、創作的な表現ではない。(2) 新しい header は Venus の file の
  文・構造・名前（`VK_COMMAND_TYPE_*_EXT`）を写さず、zedBSD の名前・comment・並びで書く。(3) 生成の道具も Venus の file を読まなくなる。
  よって新しい header に Google の著作権の表示と MIT の全文は要らず、「番号を再利用した」という事実の記載で足りる。
- ただし、これは license の判断で、プロジェクトの方針として決めるのはユーザー（§6 の H1）。外す前の版（今の opcodes.h と LICENSE-PROTOCOL）は git の履歴に
  残る。

## 4. なぜ番号を変えないか（版 1）

- QEMU の試験と Windows 版（WINQ-EMU）では、guest の libvulkan の stream を host の virglrenderer の Venus の renderer が解釈する。番号を変えると host が
  command を知らず、GPU が動かなくなる。
- i915 の実行器（実機）は zedBSD の code なので番号は自由だが、libvulkan は 1 つの stream の形で両方に送っている。番号を 2 つ持つと、libvulkan が送る先で
  表を切り替える複雑さが増える。
- ユーザーの指示「Venusと一致している内容からスタートする」とも合う。将来、zedBSD だけの command は Venus の使わない範囲に足す（§2.1）。

## 5. 確かめ（p002 で）

- 置き換えの前後で libvulkan と i915 の実行器の object の **送る・受ける番号が同じ**: 機械的な確かめとして、header の 145 個の値を今の opcodes.h と比べる
  host の script（両方を読んで名前の対応と値を照合）。
- build: zedBSD の `bin/vkdemo`・libvulkan・vmunix（i915 の実行器）を warning 0。Linux の Keiland の build（libvulkan は zedBSD 用だけなら不要、確かめる）。
- QEMU（T1）: Venus の経路の vkdemo と compositor の起動（GPU が動く）。実機（UAT）: i915 の実行器の経路（5330）。
- `grep -rn "Google" userland/desktop/libvulkan include src/drivers/gpu` が空。

## 6. 人間の判断が要る点

| ID | 問い | 案 |
| --- | --- | --- |
| H1 | license: 番号の表を zedBSD の名前で書き直し、「Venus の番号を再利用した」と書いて、Google の著作権の表示と `LICENSE-PROTOCOL` を外してよいか（§3） | 外す（番号は interface の事実で、Venus の文・名前・構造を写さない） |
| H2 | UAPI に `include/uapi/gpu-op.h` を足してよいか（kernel の i915 の実行器と libvulkan が共有する。今は libvulkan の中と kernel の数字の写し） | 足す。名前は `GPU_OP_*`、protocol の名前は「Kei GPU command protocol」 |
| H3 | 名前の付け方: `GPU_OP_CREATE_INSTANCE`（Vulkan の名前から `vk` を除いた大文字、`GPU_COMMAND_*` は既存の ioctl と重なるので使わない）か、`GPU_OP_vkCreateInstance`（Vulkan の名前のまま） | `GPU_OP_CREATE_INSTANCE`（独自の名前、coding style の大文字の定数） |

### 決定（2026-10-05 夜、ユーザー、Q1 経由）

H1: 番号の表を zedBSD の名前で書き直し、Google の著作権の表示と `LICENSE-PROTOCOL` を外す。H2: `include/uapi/gpu-op.h` を足す（UAPI の追加の承認）。H3: `GPU_OP_CREATE_INSTANCE` の形の名前。いずれも案のとおり。

## 7. 段（案）

| Phase | 内容 | 依存 |
| --- | --- | --- |
| p002 | header、3 か所の置き換え、生成の道具と license の記録の直し、値の照合の script、build。T1 で QEMU の GPU（vkdemo・compositor） | H1〜H3 |
| p003 | 全文規約の見直し | p002 |

## 結果

（設計の第 1 版。ユーザーの判断 H1〜H3 待ち）
