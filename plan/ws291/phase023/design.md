<!-- awesome-plan project=zedbsd record=ws291-p023-design -->

# ws291-p023 設計: 共有の image の画素の並びを client と compositor で決める（window mode、nvrtx の block-linear から）

- 親: [phase.md](phase.md)・[WS291](../ws.md)。Queue: q1781-i01（P3、2026-10-31、worktree `p3-ws291wsi`、基点 main d1014941e）。
- 状態: 設計（code は変えていない）。レビュー: [design-review.md](design-review.md)。ユーザーへの問い: [user-questions.md](user-questions.md)。
- この文書の行番号は基点 d1014941e の物。

## 0. 前提の決定と結論の要約

### 0.1 この設計が従うユーザーの決定（要点）

- 2026-10-31: RTX の subs-1000 は cockpit の 0j の (b)（画面に出す image を GPU の得意な並びにする）で進める。image の並びは普通は client が求める物なので、その扱いを示す（phase.md）。
- 2026-10-31（Q1 経由）: compositor の image の形式と並びは compositor が決め、client は compositor が示した物から選ぶだけにする。
- 2026-10-31（Q1 経由）: game mode は WS309 p015（P2、q1782）で文字どおりの zero-copy にする（client は compositor が貸す image に直に描く、全 GPU）。window mode は変えない（p015 の範囲では）。
  → この設計（p023）は **window mode（compositor が合成する場合）** を受け持ち、共有の image の並びの表し方と protocol の版は p015 の貸す image にもそのまま使える形にする。

### 0.2 結論

1. **今の copy の根は nvrtx の中にある**。nvrtx は 1 level の色の image を、`VK_IMAGE_TILING_OPTIMAL` で作られても pitch-linear に置く（§1.1）。
   Wayland の swapchain の app の image は OPTIMAL（`wsi-swapchain.c:1626`）なので、nvrtx では pitch-linear になり、深度付きの pass は毎回 block-linear の影に描いて
   pass の終わりに全画面を copy する。WSI の共有の image（pitch-linear）への copy は別にもう 1 回、present ごとにある（`wsi-swapchain.c:2549`）。
2. **段 A（protocol も ABI も変えない、nvrtx だけ）**: OPTIMAL・1 level・色の attachment の image を block-linear に置く。pass ごとの影の copy が無くなり、
   copy は present ごとの 1 回（app の image → 共有の image）だけになる。これで phase.md の「多くても present ごとに 1 回」を満たす。subs-1000 の見込みは約 16 fps
   （p014 の診断 nc: copy を書かない kernel で 16.07 fps）。NVK の 75 fps との残りは run ごとの GPU の固定の約 56 µs（p014 の内訳）で、この設計の範囲の外。
3. **段 B〜D（window mode の 0 copy）**: compositor が「この device の、この形式の OPTIMAL の image を読める」と `kl_gpu_buffer_v1` の版 4 の event で示し、
   client の WSI はそれを見て、共有の image そのものを OPTIMAL（nvrtx では block-linear）で作り、app に直に描かせる（present の copy が 0）。
   compositor は同じ image を sampler で読んで合成する。並びは `gpu_image_descriptor.tiling` の既存の field に新しい値 `GPU_IMAGE_OPTIMAL`
   （「書き出した device 自身の OPTIMAL の並び、width・height・format・usage で決まる、同じ device の中だけで有効」）で表す。HAL は変えない。
4. **p015 との関係で重要な点**: game mode で client が貸された **pitch-linear** の display の image に直に描くと、nvrtx では pass ごとの影の copy が戻る
   （段 A の効果が game mode で消え、subs-1000 は今の約 11 fps に戻る見込み）。nvrtx で貸す image を block-linear にするには nvrtx の display が
   block-linear を scanout できる事が要る（今の nvrtx の display は pitch だけを program する、§1.6）。この部品（段 E）をここで設計し、p015 に渡す。
5. 効果の大きさの正直な見積もり: 段 A の後の present の copy は 1 frame に 1 回（1080p で GPU 約 29 µs と WSI の submit 1 回）。window mode の 60 fps の desktop では
   frame の 0.2〜0.5 % 程度で、fps の差としてはほぼ見えない。段 B〜D の価値は fps よりも、memory の帯域（1080p で 1 frame 約 16.6 MB の読み書き）・遅延・
   p015 との共通の部品にある。どこまで今やるかはユーザーに問う（[user-questions.md](user-questions.md) Q1）。

## 1. 読んだ事実（基点 d1014941e）

### 1.1 nvrtx の影の copy と image の並び

- 影を使う条件: 色の target が pitch-linear で深度の target がある時（`src/drivers/gpu/nvrtx/vk/vk-execute.c:1093-1098`）。3D engine は深度付きでは
  block-linear の色の target 0 にしか描かない（同 1136-1146 の comment、Mesa の `nvk_linear_render_copy` を手順の参考）。
- 影の確保と pass の始めの copy（LOAD の時、ただし前の pass が同じ target に copy して他が書いていなければ省く: `execute_kept_holds`）: `vk-execute.c:1147-1224`、
  `1233-1265`。pass の終わりの影 → target の全画面の copy: `vk-execute.c:1276-1345`（`execute_shadow_copy` の quad）。
- **1 level の色の image は tiling に関わらず pitch-linear**: `vk-resource.c:309-322`（`resource_image_layout`、`image->levels == 1U` で pitch-linear、
  2 level 以上だけ `image->tiled = true`、同 326-327）。create の時に `info.tiling` は NV12 の確かめ（`vk-resource.c:424`）以外で見ていない
  （`resource_create_image` `vk-resource.c:198-255`）。
- block-linear の image は既に各所で扱える: 色の target（`vk-execute.c:2894-2896` → `exec-push.c:445-449`、一 GOB の block）、quad の source
  （`vk-execute.c:2965`・`2786-2790`、tiled の texture header）、sampler（`vk-execute.c:2005-2006`）、CPU の copy（`vk-execute.c:2604-2617`、
  画素ごとに GOB の位置を計算）、memory の境界（`vk-memory.c:405`・`482`、tiled は GOB の 512 B）。`vkGetImageSubresourceLayout` は tiled の image に
  空の layout を返す（`vk-resource.c:510-524`、仕様でも LINEAR だけが定義）。
- 形式の機能: 色の形式は optimal と linear の機能が同じ（`vk-device.c:649-650`）。storage image は無い（`vk-device.c:707-709`）。
- mview の subs-N は 1 frame を N 回の submit に分け、2 回目からは色も深度も LOAD の pass（`userland/tests/mview/renderer.c:1011-1032`、`2407-2470`）。
- 計測（p014 q1764-i01 の節、[phase014](../phase014/phase.md)）: subs-100 の GPU の 1 run は約 89 µs、copy を書かない診断（nc）で約 60 µs、つまり copy 約 29 µs、
  run ごとの固定の費用 約 56 µs。nc の subs-1000 は 16.07 fps、big-1000 は 116.1（copy ありの r と同じ）、subs-100 は 153.6（r は 106）。
  copy を遅らせる案（`plan/ws291/tests/nvrtx-shadow-defer.patch`）は libvulkan の私用の fence のため効かなかった。

### 1.2 libvulkan の WSI

- 共有の image（compositor・display に渡す image）は常に LINEAR で作る: `userland/desktop/libvulkan/wsi-image.c:110`、記述は `tiling = GPU_IMAGE_LINEAR`
  （同 207）、row pitch は `vkGetImageSubresourceLayout` の値（同 186-205）。取り込み（`vulkan_wsi_shared_image_import`）も LINEAR だけ（同 279-285・313）。
- swapchain の 2 つの経路（`wsi-swapchain.c`）:
  - **direct**: app の image そのものが共有の image（LINEAR）で、display が直に読む。`VK_KHR_display` の primary plane の時だけ
    （`swapchain_direct_possible` `wsi-swapchain.c:1740-1780`、条件 `display_mode != NULL` は 1751-1753、linear で色の attachment に描ける形式は 1773-1777）。
    image の作成は 1585-1611。
  - **copy**: app の image は OPTIMAL の普通の image（1615-1650、`create.tiling = VK_IMAGE_TILING_OPTIMAL` は 1626）、共有の image（presentation）を
    別に作り（`swapchain_native_images_create` 1653-1730）、present ごとに `present_copy`（2549、`vkCmdBlitImage`・`vkCmdCopyImage` 3598・3608）。Wayland はいつもこちら。
- acquire は、copy の経路でも、同じ番号の presentation の image が compositor から返った（`image_available`）まで待つ（`wsi-swapchain.c:4099-4104`）。
  つまり app の image の番号 i と wl_buffer の番号 i は今も 1 対 1 で、返りの待ちは既にある。
- present の結果の合成: `VK_ERROR_OUT_OF_DATE_KHR`・`VK_SUBOPTIMAL_KHR` を 2 つの結果から選ぶ（`wsi-swapchain.c:3474-3479`）。
- Wayland の側（`wsi-wayland.c`）: 共有の image ごとに fd と 64 B の記述を `kl_gpu_buffer_v1.create_buffer` で送る（`wayland_import` 623-675、657）。
  factory は版 3 までに丸めて bind（1234-1236）。版 3 なら premultiplied alpha を示す（331-332）。
- 外部 memory の互換の確かめ（`vkGetMemoryFdKHR` の allocation の経路）は deviceUUID と driverUUID を比べる（`memory.c:798-799`）。しかし nvrtx の
  deviceUUID は固定の定数（`src/drivers/gpu/nvrtx/vk/vk-device.c:303`）で、同じ型の GPU が 2 枚あると区別できない。

### 1.3 kernel の共有の image の記述と import

- 記述 `struct gpu_image_descriptor`（`include/uapi/gpu.h:117-131`、64 B）には既に `tiling`（u32）と `reserved` がある。値は `GPU_IMAGE_LINEAR = 1` だけ（同 53）。
- export の確かめ `gpu_image_validate`（`src/drivers/gpu/gpu.c:5214-5266`）: LINEAR 以外は `EOPNOTSUPP`（5234）、stride・offset・`stride*height` の範囲を
  LINEAR の式で確かめる（5245-5263）。export の ioctl（5392-5486）は記述を `shared->image` に保ち、driver の `export_resource` に渡す。
- import（`gpu_import_ioctl` 5489-5640）: 受け手の記述は空でなければ拒む（kernel の記録だけを返す）、別の device の capability は `GPU_IMPORT_SCANOUT` の時だけ
  （5560-5563、他は `EXDEV`）、foreign の scanout は `gpu_scanout_import`（5161-5212、display driver の `import_image`）。import した資源は記述を持たない
  （`struct gpu_resource` `gpu.c:155-172`）。
- allocation だけの共有（`include/uapi/gpu-allocation.h`、`GPU_ALLOCATION_EXPORT`）は「並びを課さない」不透明な metadata（`gpu.c:4748-4776`）。libvulkan の
  `vkGetMemoryFdKHR`（libmedia の NV12 の OPTIMAL の image の共有 `userland/desktop/libmedia/vkvideo-runtime.c:995-1019`・`2660-2690`）がこれを使う。
- display の present（`gpu_display_present_resource` `gpu.c:4282-4320`）は request の stride・height で範囲を確かめて driver に渡す。並びの情報は request に無い。
- driver の共有の口 `struct drv_gpu_share_ops`（`include/drivers/gpu/gpu-share.h`）: export・release・import・2 つの任意の physical view。HAL ではない
  （driver framework の内部の header）。

### 1.4 `kl_gpu_buffer_v1`（今は版 3）

- client 側の定義: request は destroy・create_buffer（`nha`: new_id・fd・記述の array）・set_acquire_fence（版 2）・set_alpha（版 3）、**event は無い**
  （`userland/desktop/libwayland/protocol.c:899-910`、header `keiland-gpu-buffer-v1-client-protocol.h`）。
- compositor 側: 版 3（`userland/desktop/libkeiland-backend-zedbsd/gpu-buffer-zedbsd.c:47-53`）。bind の hook `kl_backend_gpu_bind`（同 237-247）は何も送らない
  （Linux の dma-buf の backend はここで format と modifier の event を送る: `libkeiland-backend/dmabuf/gpu-dmabuf.c:104-150`）。
- 記述の解読 `kwl_gpu_buffer_decode`（`gpu-zedbsd.c:52-127`）: LINEAR 以外は `EINVAL`（84）。取り込みの image は LINEAR・SAMPLED（`gpu-buffer-zedbsd.c:494-512`）で、
  memory type・size・offset・row pitch を記述と比べる（524-541）。dedicated の import を libvulkan が kernel の記録と比べる
  （`userland/desktop/libvulkan/dedicated.c:31-104`、LINEAR だけ 42・50）。create_buffer の失敗は `EPROTO`（`gpu-buffer-zedbsd.c:157-162`）で client への
  protocol の誤りになる。

### 1.5 compositor の合成・game mode・画面の撮影

- 合成: 取り込んだ image を sampler で読む（`gpu-buffer-zedbsd.c` の `buffer_adopt`）。
- game mode（今の V1 の暫定、`userland/desktop/wayland/scanout.c:8-46`）: fullscreen で content type が game の window を、backend が client の buffer を
  `GPU_RESOURCE_IMPORT`（`GPU_IMPORT_SCANOUT`、`scanout-zedbsd.c:466`）と `GPU_DISPLAY_PRESENT`（BLOB、記述の stride・format をそのまま 281 前後）で出す。
  benchmark（`plan/ws291/tests/rtx-p013.sh` の mview `--content=game`）はこの経路で測っている。
- 画面の撮影（`userland/desktop/wayland/shot.c:20-25`）: compositor が自分の swapchain の image を Vulkan で読み戻す（game mode なら撮影の時に 1 frame 合成する）。
  client の buffer を CPU が直に読む経路は無い。→ client の buffer の並びを変えても keiland-shot は compositor の合成の結果を読むだけで、そのまま使える。

### 1.6 nvrtx の display

- window の image は pitch だけを program する（`src/drivers/gpu/nvrtx/display/disp-records.c:194-201`・`1006-1011`、`nvrtx-disp.h:146-152`、
  `NVRTX_DISP_STORAGE_PITCH_TURING = 0x10`）。present の blob は request の stride だけで surface を作る（`display/present.c:829-843`）。
- export は記述を見ない（`src/drivers/gpu/nvrtx/share.c:85-116`、`UNUSED_PARAMETER(image)`）。scanout の制約は pitch の 64 B・offset の 256 B（同 216-245）。
- hardware は block-linear を読める: NVIDIA open-gpu-doc の `clc57e.h`（MIT）の `NVC57E_SET_STORAGE` に BLOCK_HEIGHT（bit 3:0、ONE_GOB = 0）と
  MEMORY_LAYOUT（bit 4、BLOCKLINEAR = 0、PITCH = 1）がある。Linux の nouveau（v6.12、`dispnv50/wndwc57e.c` `wndwc57e_image_set`、`wndw.c:296-308`）は
  block-linear の時 pitch の field に「行の bytes / 64」（GOB の幅の数）を入れ、ctxdma に memory の kind を与える（`wndw.c:50-110`）。
  **nvrtx の ctxdma（`NVRTX_DISP_HANDLE_VRAM`）で kind 0 の page の block-linear を window が正しく読むかは未確認**（§15、危険 R3）。

### 1.7 他の GPU

- amdrdna: 色の image は全て pitch-linear（`src/drivers/gpu/amdrdna/vk/vk-resource.c:12`・`275`・`348`）。
- i915: 色は linear、深度は Y-tiled（`src/drivers/gpu/i915/vk/vk-image.c:420-421`）。linear の色と tiled の深度は組める（影は無い）。
- powervr: image に linear と optimal の形がある（`src/drivers/gpu/powervr/vk/vk-image.c:200`・`297-301`）。
- 共有の確かめ: powervr（`render.c:1833`）・bcm2711（`share.c:315`）・mali の display（`display/scanout.c:52`）は LINEAR だけを受ける。
  powervr・bcm2711 は同じ allocation を別の記述で再び export するのを拒む（`powervr/render.c:1895` 前後の `same_image`、`bcm2711/share.c:347`）。
- venus（QEMU）: 命令は host の Venus にそのまま送られる（`include/uapi/gpu-op.h:117-127`）。zedBSD の独自の op を host は知らない。
- 影の問題（深度付きで linear の色に描けない）は nvrtx だけ。他の GPU では tiled の並びは帯域と速さの改善で、必須ではない。

## 2. 要件・範囲・前提

- 要件 R1: 深度付きの pass ごとの全画面の copy を無くす（subs-N）。R2: copy は present ごとに多くても 1 回、合成の時は 0。R3: 並びは compositor が示し、client はそこから選ぶ。
  R4: 読む相手が同じ device でない・示されていない・失敗した時は今の pitch-linear と copy に戻る。R5: HAL を変えない。他の GPU が後で自分の並びを使える形。
  R6: p015（game mode、貸す image）と同じ表し方。
- 範囲の外: game mode の経路そのもの（p015）、run ごとの GPU の固定費（p014 の残りの候補）、Linux・FreeBSD の backend（dma-buf の modifier は既にある）、
  他の GPU の tiled の並びの実装（口だけ用意する）。
- HAL（`include/hal/hal.h`）は変えない。変えるのは UAPI（`include/uapi/gpu.h` の値 1 つと意味）、driver framework の内部の header（`gpu-share.h` の任意の口 1 つ）、
  libvulkan・libwayland・compositor の backend・nvrtx。HAL の差分の案は無い（`plan/ws291/phase023/proposed/` は作らない）。

## 3. 全体の形（段）

| 段 | 何を | 変える所 | protocol | 効果 |
| --- | --- | --- | --- | --- |
| A | nvrtx: OPTIMAL・1 level・色の attachment を block-linear に | nvrtx の vk だけ | 変えない | pass ごとの影の copy が 0、present ごとの copy 1 回（window・game とも、p015 の前） |
| B | 並びの値 `GPU_IMAGE_OPTIMAL` と kernel の確かめ、nvrtx の export の記録 | UAPI・gpu.c・gpu-share.h・nvrtx/share.c | 変えない | B だけでは見た目は変わらない（C・D・p015 の土台） |
| C | libvulkan: OPTIMAL の共有の image の export・import、device の identity | libvulkan | 変えない | 同上 |
| D | `kl_gpu_buffer_v1` 版 4: compositor が読める並びを示す。WSI の Wayland の direct の経路 | libwayland・compositor の backend・wsi-wayland・wsi-swapchain | 版 4（製品の判断） | window mode の present の copy が 0 |
| E | nvrtx の display の block-linear の scanout（p015 の貸す image のため） | nvrtx/display | 変えない | p015 の game mode で nvrtx の影を出さない |
| F | 回帰と計測、記録 | 試験だけ | — | 受け入れ |

A は B〜E に依存しない。B → C → D の順。E は B に依存し、p015 の nvrtx の部分の前に要る。

## 4. 段 A: nvrtx の OPTIMAL の色の attachment を block-linear に

### 4.1 規則

- `resource_image_layout` に create の `tiling` と `usage` を渡し、**`VK_IMAGE_TILING_OPTIMAL` かつ 1 level かつ色の形式かつ `VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT`** の
  image を、2 level 以上の tiled の image の level 0 と同じ並び（一 GOB の block、行は GOB の幅の倍数、高さは GOB の高さの倍数、`vk-resource.c:326-339`）に置く。
- 色の attachment でない OPTIMAL の 1 level の image（upload して sample するだけの texture）は今の pitch-linear のまま。理由: CPU の copy の道
  （`vk-execute.c:2604-2617`）は tiled の image に画素ごとの 4 B の copy をするので、buffer の行が quad で扱えない時の upload が遅くなる。影は色の attachment
  にしか起きないので、そこだけを変えれば R1 に足りる。
- LINEAR の image は今のまま（影は残る。LINEAR の色に深度付きで描くのは `VK_KHR_display` の direct の chain と、app が明示に LINEAR を選んだ時だけ）。
- 外部 memory の印のある OPTIMAL の image（`VkExternalMemoryImageCreateInfo`）も同じ規則にしてよい: Vulkan の OPAQUE_FD の規則（作成の parameter が同じ image なら
  同じ並び）に従い、取り込む側が同じ parameter で作れば同じ並びになる（nvrtx の並びは parameter の決定的な関数）。libmedia の NV12（`video` の並び）は変わらない。

### 4.2 影響を確かめる所（実装の Phase で全部読む）

| 所 | 今 | 段 A の後 |
| --- | --- | --- |
| 色の target（`execute_image_target`） | `image->tiled` で block_linear | 同じ（tiled になる） → 影の条件（1093）に入らない |
| quad の source・sampler の header | tiled を扱う | 同じ |
| present の copy（app の image → 共有の LINEAR） | pitch → pitch の quad | tiled の source → pitch の target の quad（既に使う形: 影の終わりの copy と同じ） |
| CPU の copy（buffer ↔ image） | 画素ごと（tiled） | 色の attachment に CPU の copy をする app だけ遅くなる（mview・compositor は quad で足りる見込み、PROF の行で確かめる） |
| `vkGetImageSubresourceLayout` | tiled は空 | 同じ（仕様どおり、LINEAR だけが定義） |
| memory の境界と大きさ | tiled は 512 B | 同じ。大きさは行を 64 B・高さを 8 行に丸めた分だけ増える（1920x1080 は 0 B 増） |
| 影の `kept` と LOAD の copy の省略 | pitch の target | 対象が減るだけ（LINEAR の target では今のまま） |
| compositor の中間の target（glass・blur など OPTIMAL の色の attachment） | pitch | block-linear（sample は tiled の header）。desktop の 60 fps と画面の撮影で確かめる |

### 4.3 見込み

- subs-1000: 約 10.9 → 約 16 fps（nc の 16.07 が上限の目安。nc は終わりの copy だけを省いた診断で、段 A は LOAD の copy も起きない）。subs-100: 約 106 → 約 150。
- big-1000・scene-1500・scene-5000: 1 frame の pass が少ないので揺れの内（nc の big-1000 116.1、scene-1500 162.6）。
- desktop（frame-ops）: compositor の自分の swapchain は direct の LINEAR で深度なし（影は元から無い）。中間の target の並びが変わるだけで、後退しない見込み。

## 5. 並びの表し方

### 5.1 決め: `tiling` の新しい値 `GPU_IMAGE_OPTIMAL`

`include/uapi/gpu.h` に `#define GPU_IMAGE_OPTIMAL 2U` を足し、意味を次のとおりにする（記述の構造と大きさは変えない）。

- 「`device_id` の device が、`width`・`height`・`format`・`usage` で `VK_IMAGE_TILING_OPTIMAL`・1 level・1 layer・1 sample の 2D の image を作った時の、
  その device 自身の並び」。その device の中だけで意味を持つ（別の device への import は拒む）。
- `stride` と `offset` は 0（並びは driver が決めるので、記述には書かない）。`allocation_bytes` は今までどおり資源の大きさ。
- 並びの中身（nvrtx なら block-linear・一 GOB の block）は driver の私事。zedBSD では Vulkan の driver が kernel の中に 1 つずつあり、client・compositor・display が
  同じ boot の同じ driver の同じ関数で並びを決めるので、版の食い違いが起きない。
- p015 の貸す image もこの値を使える（compositor が display の swapchain を OPTIMAL で作り、その記述を client に渡す）。

### 5.2 選ばなかった案

| 案 | 内容 | 選ばない理由 |
| --- | --- | --- |
| 明示の並びの code（Linux の DRM format modifier の型、例 NVIDIA の block-linear と block の高さを 64 bit で） | 並びを値で名指す | 64 bit を記述に入れるには構造の版を上げ export・import の ioctl の ABI が変わる。zedBSD の buffer は machine の外に出ず、読む相手は同じ kernel の同じ driver なので、明示の値の利点（別の driver・別の OS との相互運用）が無い。libvulkan と各 driver に `VK_EXT_image_drm_format_modifier` を実装する量も大きい。後で要れば `tiling` の別の値（vendor の値の範囲）を足せる |
| 32 bit の vendor の code を `tiling` に（例 NVIDIA block-linear の block 高さ h） | 明示だが構造は変えない | 表を UAPI に置く量が増え、driver が block の高さを image の大きさで変えたくなった時に code が増える。読む相手が同じ driver だけの間は不透明で足りる（AHA の方針、master-design-policy §2.6） |
| allocation だけの共有（`GPU_ALLOCATION_EXPORT`）で OPTIMAL を運ぶ | Vulkan の OPAQUE_FD の規則そのまま | display の scanout の import（`GPU_IMPORT_SCANOUT`）は image の記述の capability を要る（`gpu.c:5553`）。compositor の取り込みも image の記述（dedicated の確かめ）で作ってある。image の記述の道に OPTIMAL を足す方が変更が小さい |
| client が並びを決め、compositor は受けるか拒むか | Q1 の最初の見立て (1) | ユーザーの決定（compositor が決め、client は示された物から選ぶ）に合わない。今の create_buffer の失敗は client への fatal な protocol の誤り（§1.4）で、試して戻る形も作れない |

## 6. kernel（段 B）

### 6.1 GPU の core（`src/drivers/gpu/gpu.c`）

- `gpu_image_validate`: `tiling == GPU_IMAGE_LINEAR` は今のまま。`tiling == GPU_IMAGE_OPTIMAL` は、`stride == 0`・`offset == 0`・形式と usage の確かめ（今と同じ）の後、
  device の `share->image_layout`（下の新しい任意の口）を呼んで「その記述の image に要る bytes」を受け、`bytes <= resource->bytes` を確かめる。口の無い driver は
  `EOPNOTSUPP`（今と同じ結果、libvulkan は copy の経路に戻る）。他の値は `EOPNOTSUPP`。
- `gpu_import_ioctl`: `shared->image.tiling != GPU_IMAGE_LINEAR` で `foreign != 0` なら `EXDEV`（`GPU_IMPORT_SCANOUT` でも）。`gpu_scanout_import` は LINEAR だけ。
- `gpu_display_present_resource`（4308）: request の stride で範囲を確かめる今の式は OPTIMAL では意味が無い。present の並びは driver が backing の記録から決める
  （§6.3）ので、core は「OPTIMAL の資源の present は driver が `stride == 0` を受ける」形にする。これは p015（game mode）が使う時に要る変更で、段 E とまとめる。
- `struct gpu_resource` に記述を持たせない（並びは driver の backing が持つ、§6.3）。

### 6.2 driver framework の口（`include/drivers/gpu/gpu-share.h`）

`struct drv_gpu_share_ops` の末尾に任意の口を 1 つ足す（位置の初期化子の driver はそのまま NULL になる）:

```c
/* Optional: bytes an OPTIMAL image of this description takes; NULL means the backend shares linear images only. */
int (*image_layout)(void *device, const struct gpu_image_descriptor *image, uint64_t *bytes);
```

- `GPU_INFO` の capabilities に `GPU_CAP_OPTIMAL_SHARE`（新しい bit）を、core が「`share->image_layout != NULL`」から立てる（driver が bit を手で立てない）。
  libvulkan はこの bit で OPTIMAL の export を試すかを決める（試して EOPNOTSUPP を受ける往復を省く）。
- HAL ではない（driver framework の内部の header）。既存の driver（amdrdna・i915・powervr・venus・mali・bcm2711・ghci）は変えなくてよい。

### 6.3 nvrtx（`src/drivers/gpu/nvrtx/share.c` ほか）

- `image_layout`: 記述の width・height・format・usage から段 A の規則で並びを計算し（vk の `resource_image_layout` と同じ関数を共有の場所に出す）、bytes を返す。
  usage が色の attachment を含まない・形式が違う時は `EOPNOTSUPP`。
- `share_export`: 今は記述を捨てる（`UNUSED_PARAMETER(image)`）。backing に「export された並び」（LINEAR か OPTIMAL、width・height・format・usage、LINEAR なら stride・offset）を
  記録する。同じ backing を別の記述で再び export するのは `EINVAL`（powervr・bcm2711 の `same_image` と同じ考え）。
- bind の確かめ（多重の守り）: OPTIMAL の記録のある backing の memory に bind する image は、同じ並び（tiled・pitch・width・height・format）でなければ失敗。
  安全（範囲）は既存の確かめ（image の bytes が memory の残りに収まる `vk-memory.c:420-428`）で既に守られており、これは正しさのための確かめ。

## 7. libvulkan（段 C）

### 7.1 OPTIMAL の共有の image の作成と export

- `vulkan_wsi_shared_image_create_usage`（`wsi-image.c:52-230`）に「並び」の引数を足す。OPTIMAL の時: `create.tiling = VK_IMAGE_TILING_OPTIMAL`、usage は呼び手の usage に
  `SAMPLED`（compositor の取り込みの確かめが要る）と `TRANSFER_SRC` を足す。`vkGetImageSubresourceLayout` は呼ばない。記述は `tiling = GPU_IMAGE_OPTIMAL`・
  `stride = 0`・`offset = 0`。export は今の `vulkan_memory_image_fd`（`memory.c:600-650`）のまま（kernel が `image_layout` で確かめる）。
- 条件: context の capabilities に `GPU_CAP_OPTIMAL_SHARE`。無ければ OPTIMAL を作らず呼び手に `VK_ERROR_FORMAT_NOT_SUPPORTED`（呼び手は LINEAR・copy の経路へ）。

### 7.2 OPTIMAL の image の取り込み

- `vulkan_wsi_shared_image_import`（`wsi-image.c:236-403`、display の WSI が使う）と compositor の取り込み（`gpu-buffer-zedbsd.c` → `vkAllocateMemory` の dedicated の import →
  `memory_dedicated_check` → `vulkan_dedicated_check`）で、`tiling == GPU_IMAGE_OPTIMAL` を受ける:
  - 作る image: `VK_IMAGE_TILING_OPTIMAL`、format・extent は記述、**usage は記述の usage そのもの**（並びが usage で決まるため、受け手が SAMPLED だけにすると並びが変わる）。
  - `vulkan_dedicated_check`: OPTIMAL の時は image の tiling が OPTIMAL・usage が記述と同じ・size が allocation 以内・memory type を確かめ、offset と row pitch は比べない。
- 別の device の記述（kernel が `EXDEV`）は `VK_ERROR_INVALID_EXTERNAL_HANDLE`。

### 7.3 device の identity を compositor に見せる

- compositor は libvulkan だけを使い、kernel の GPU の型を読むのは backend の `gpu-zedbsd.c` だけ（Guardrail）。版 4 の event で「どの device の並びか」を示すため、
  compositor の backend は自分の Vulkan の device の kernel の identity（`gpu_device_info.device_id`、u64）を知る必要がある。
- 決め: libvulkan は `VkPhysicalDeviceIDProperties.deviceLUID` に kernel の device の identity（`GPU_DEVICE_QUERY` の `device_id`、8 B）を入れ、`deviceLUIDValid = VK_TRUE` にする
  （zedBSD での意味として libvulkan の README と `external-properties.c` に書く）。driver の返す値の上書きは libvulkan の中で行う（driver は変えない）。
- 選ばなかった案: deviceUUID を使う（nvrtx は固定値 `vk-device.c:303`、仕様は再起動をまたいで同じ値を求めるので boot ごとの identity を入れるのは合わない）。
  libvulkan の独自の関数を export する（標準の型で足りる）。backend が `/dev/gpu0` を開いて問う（compositor の Vulkan の device と同じ node である保証が無い）。
- client の WSI は自分の context の fd で `GPU_DEVICE_QUERY` を直に問える（libvulkan の中）。

## 8. `kl_gpu_buffer_v1` 版 4（段 D、製品の判断）

### 8.1 追加する event

| event | 引数 | 意味 |
| --- | --- | --- |
| `layout`（版 4） | `uint device_hi, uint device_lo, uint format, uint tiling, uint flags` | compositor が `device` の `format`（`GPU_PIXEL_*`）の `tiling`（`GPU_IMAGE_*`）の buffer を合成できる（flags bit 0 = SAMPLED、他は 0、将来の SCANOUT などに予約） |
| `layouts_done`（版 4） | なし | ここまでで一揃い。後で送り直す時は、`layout` を全部送り直してから `layouts_done` |

- bind の直後（`kl_backend_gpu_bind`）に送る。LINEAR は版 1 から常に読めるので送らない（送っても良いが WSI は LINEAR を常に可とする）。
- 送り直し: compositor の device が変わった時など（window mode では普通は起きない）。送り直しの前に作られた buffer は、compositor が読める限り受け続ける。
  読めなくなった buffer は今の取り込みの失敗の扱い（その buffer の最初の import の時に決まるので、作り直しの後の新しい buffer には影響しない）。
- 版の互換: 版 3 の compositor（event 無し）→ client は版 3 で bind し LINEAR（今と同じ）。版 4 の compositor と版 3 の client → event は版 4 の bind にだけ送るので、
  古い client は今と同じ。create_buffer の request の形は変えない（記述の `tiling` が OPTIMAL になり得るだけ）。版 4 の compositor は、示していない並びの buffer を
  今と同じく `EPROTO` で拒む。
- 定義の置き場: client の `libwayland/protocol.c`（interface の events の表、版 4）と header、compositor の `wayland/protocol.c`（global の版は backend の
  `kl_backend_gpu_global_version` が返す 4）、backend の `gpu-buffer-zedbsd.c`（bind で送る）。

### 8.2 compositor が何を示すかの決め方（compositor が決める）

- backend は自分の device について、`vkGetPhysicalDeviceImageFormatProperties2`（`VK_IMAGE_TILING_OPTIMAL`、format は B8G8R8A8・R8G8B8A8、usage
  `COLOR_ATTACHMENT | SAMPLED | TRANSFER_SRC`、`VkPhysicalDeviceExternalImageFormatInfo` の OPAQUE_FD）で import できるかを問い、できる形式だけを
  `layout(device, format, GPU_IMAGE_OPTIMAL, SAMPLED)` で示す。libvulkan はこの問いに、context に `GPU_CAP_OPTIMAL_SHARE` がある時だけ IMPORTABLE を返す
  （`external-properties.c` の image の external の答えに加える）。
- 版 4 でも、compositor は示す物を自分の都合で減らしてよい（例: 今の game mode の V1 の暫定が client の buffer を scanout する間、§9.4）。

### 8.3 選ばなかった案

| 案 | 選ばない理由 |
| --- | --- |
| surface ごとの feedback の object（Linux の linux-dmabuf の feedback の形） | window mode の合成で読める並びは compositor の device で決まり surface に依らない。surface ごとに変わる理由（scanout の可否）は game mode のもので p015 が貸す image で扱う。必要になれば版 5 で足せる（AHA） |
| create_buffer の失敗を fatal でない event にする（`failed`） | ユーザーの決定（compositor が示し client が選ぶ）では試して戻る必要が無い |
| 新しい interface（`kl_gpu_buffer_v2`） | event を足すだけなので版を上げる方が小さい。request の形も変えない |

## 9. WSI の選び方と作り直し（段 D）

### 9.1 選び方（swapchain の作成の時、Wayland の surface）

1. factory を版 4 までで bind し、bind の直後の roundtrip（今の初期化の roundtrip に重ねる）で `layout` の一揃いを受ける。
2. 自分の device の identity（`GPU_DEVICE_QUERY`）・format・`GPU_IMAGE_OPTIMAL` が示されていて、context に `GPU_CAP_OPTIMAL_SHARE` があり、app の usage が
   OPTIMAL の共有の image で持てる物（COLOR_ATTACHMENT・TRANSFER_SRC・TRANSFER_DST・SAMPLED、`wsi-wayland.c:327-329` と同じ集合）、`imageArrayLayers == 1` なら
   **Wayland の direct の chain**: swapchain の image そのものを OPTIMAL の共有の image で作り（`swapchain_image_create` の direct の枝 `wsi-swapchain.c:1585-1611` を
   Wayland にも使う）、各 image を `wayland_import` で wl_buffer にする（`swapchain_native_images_import` 1787-1815 と同じ形）。present は copy を記録しない（2536-2538）。
3. そうでなければ今の copy の経路（app の image は OPTIMAL で、nvrtx では段 A で block-linear、present ごとに LINEAR の共有の image へ 1 回 copy）。
4. direct の作成・export・import の途中の失敗が `VK_ERROR_FORMAT_NOT_SUPPORTED`・`VK_ERROR_FEATURE_NOT_PRESENT` なら copy の経路に戻る（今の
   `wsi-swapchain.c:1153-1170` の戻りと同じ規則）。他の失敗は失敗。

- 選ばなかった案: LINEAR のままの Wayland の direct（amdrdna・i915 は linear に深度付きで描けるので copy を省けるが、nvrtx では影が戻る。driver が「linear の描画が
  本物の速さか」を示す口が要る。今の Wayland が copy を選ぶ理由は code に書かれていない（`wsi-swapchain.c:1751` の comment だけ）ので、別の判断として残す（user-questions Q4））。

### 9.2 acquire と返り

- direct の chain でも、image i の再利用は wl_buffer i の release まで待つ（今の copy の経路と同じ `image_available`、`wsi-swapchain.c:4099-4104`）。新しい待ちは要らない。
- mailbox: compositor が置き換えた buffer を返すのも今と同じ。copy の経路と違い、compositor が合成で読んでいる間 app はその image を持てないので、
  `minImageCount` は今の Wayland の値で足りるかを確かめる（mview `--images=4` と desktop の app の既定）。

### 9.3 作り直し（`VK_SUBOPTIMAL_KHR`）

- `layout` の送り直しで、今の chain の並び（OPTIMAL）が示されなくなった時、lease に「最適でない」を立て、以後の `vkQueuePresentKHR` と `vkAcquireNextImageKHR` は
  `VK_SUBOPTIMAL_KHR` を返す（成功の扱い、今の結果の合成 3474-3479 に入れる）。app が作り直すと、新しい chain は新しい示しで選ぶ。
- 逆（OPTIMAL が新しく示された）も `VK_SUBOPTIMAL_KHR` を返して良いが、損は copy 1 回だけなので返さない（app の作り直しの負担の方が大きい）。
- `VK_ERROR_OUT_OF_DATE_KHR` はこの仕組みでは返さない（surface の大きさなど今の理由だけ）。
- p015 との境（§14）: game mode に入る・出るで swapchain の image を貸す image に替えるのは p015 の仕組み（app は気づかない、WS309 p015 の判断 1）で、
  この §9.3 の作り直しは使わない。

### 9.4 今の game mode の V1（p015 が入るまで）との組み合わせ

- 今の game mode は client の buffer をそのまま scanout する（§1.5）。段 D で client の buffer が OPTIMAL になると、nvrtx の display は（段 E の前は）それを読めず、
  scanout の import（`GPU_IMPORT_SCANOUT`）を driver が `ENOTSUP` で断る → compositor は合成で出す（今の「direct=0」の扱い）。性能は copy の経路と同じ（合成 1 回）だが、
  ユーザーの計測の決定（3D は direct scanout で測る）に合わなくなる。
- 決め: **段 D は p015 の game mode（貸す image）が main に入った後に入れる**。p015 の後は game mode が client の window mode の buffer を scanout しないので、
  この組み合わせは起きない。段 A は p015 の前でも後でも入れられる。

## 10. compositor の取り込みと描画・撮影

- `kwl_gpu_buffer_decode`（`gpu-zedbsd.c:52-127`）: `tiling == GPU_IMAGE_OPTIMAL` を受ける（stride・offset は 0、範囲の確かめは kernel の記録に任せ、
  allocation_bytes が 0 でない事・memory type だけを見る）。`struct kwl_buffer_layout` に tiling と usage を足す。
- `buffer_image`（`gpu-buffer-zedbsd.c:476-575`）: OPTIMAL の時は `VK_IMAGE_TILING_OPTIMAL`・usage は記述の usage で image を作り、`vkGetImageSubresourceLayout` の比べを
  しない（libvulkan の dedicated の確かめが行う）。
- 合成（sampler）は image の view を通すので変わらない。nvrtx は tiled の header で sample する。
- 画面の撮影は compositor の swapchain の読み戻しで、client の buffer の並びに依らない（§1.5）。画素の比べ（`rtx-p013.sh` の SHOT）はそのまま使える。
- `scanout-zedbsd.c`（今の game mode の V1）: §9.4 の決めにより段 D の後は使われない見込みだが、OPTIMAL の buffer を受けた時は `kl_backend_scanout_begin` で
  「並びが LINEAR でない」を理由に断る（ログ `KL SCANOUT refused step=layout`）。p015 がこの file を置き換えるなら p015 に従う。

## 11. 状態・資源の所有と寿命・並行性

- 並びの記録の持ち主: kernel の core は `shared->image`（capability の寿命）、nvrtx は backing（最後の参照まで）。記述は不変（export で 1 回決まる）。
- WSI: 並び（LINEAR か OPTIMAL）は chain の不変の性質（作成の時に決まり、作り直しでだけ変わる）。lease の「最適でない」の印は surface の mutex の下で、
  Wayland の event を処理する thread が立て、present・acquire が読む（今の `lost` の印 `wsi-wayland.c:54` と同じ扱い）。
- compositor: 示す並びの表は backend の bind の時に作る不変の表（device が変わった時だけ作り直し、main の thread）。
- lock の新しい順序は無い。kernel の `image_layout` は資源の lock の外で呼んでよい純な計算（nvrtx の node の lock を取らない）。

## 12. 失敗と回復

| 失敗 | 振る舞い |
| --- | --- |
| driver に `image_layout` が無い（OPTIMAL の export が `EOPNOTSUPP`） | libvulkan は `GPU_CAP_OPTIMAL_SHARE` で事前に分かるので試さない。試した時も copy の経路 |
| compositor が版 3・OPTIMAL を示さない・別の device | copy の経路（今と同じ） |
| OPTIMAL の export の後、compositor の取り込みの失敗 | 示した並びの取り込みの失敗は compositor の不具合。今と同じ `EPROTO`（client の接続の誤り）で、記録 `KWL IMPORT_ERROR` で見つける。回避の再試行はしない |
| 別の device の OPTIMAL の記述の import | kernel が `EXDEV`、libvulkan は `VK_ERROR_INVALID_EXTERNAL_HANDLE` |
| 同じ backing を別の記述で再 export | nvrtx が `EINVAL`、libvulkan の export の失敗 → copy の経路 |
| 示しの取り下げ | `VK_SUBOPTIMAL_KHR`（§9.3）、作られた buffer は読めるまで使われる |
| GPU の reset・device の喪失 | 今の扱い（chain の失敗）。並びの仕組みは新しい回復を持たない |

## 13. 他の GPU への広げ方

- driver は `share->image_layout` を実装するだけで、libvulkan・compositor・protocol の変更なしに OPTIMAL の共有に参加する（compositor は自分の device の Vulkan の問いの答えで
  示し、client は自分の device が示されていれば選ぶ）。
- amdrdna: 今の OPTIMAL は pitch-linear なので、参加しても並びは linear のまま、得るのは「window mode の present の copy が 0」。後で DCC・swizzle を入れる時に並びの関数を
  変えれば、記述も protocol も変えずに広がる。i915: 色が linear の間は同じ。Y・Tile4 にすれば同じ口で広がる（display の scanout は別の確かめ）。
- powervr: optimal の並び（twiddled 等）を持つ。VF2 の display（dc8200）は linear だけを読む見込みなので、game mode（p015）では LINEAR の貸し、window mode では OPTIMAL、と
  compositor が分けて決める（compositor が決めるの決定に合う）。
- venus: host の driver が並びを決め、zedBSD の kernel は知らない。`image_layout` を実装しない（LINEAR だけ、今のまま）。
- 他の GPU の参加は各 WS（driver の担当）の判断で、この設計の Phase には入れない。

## 14. WS309 p015（game mode の V2）との関係

- 共通の部品: 貸す image の記述は `gpu_image_descriptor`、並びは `tiling`（LINEAR か OPTIMAL）。compositor が display の swapchain をどちらで作るかを決め、client の WSI は
  貸された記述の並びで取り込む（§7.2 の取り込みをそのまま使う）。
- **nvrtx で重要**: 貸す image が LINEAR だと、client の深度付きの pass は毎回影の copy に戻る（段 A の効果が game mode で消え、subs-1000 は今の約 11 fps）。
  nvrtx では貸す image を OPTIMAL（block-linear）にし、display がそれを scanout する必要がある → 段 E。
- 推す取り決め（P2・Q1 へ）: (1) p015 の貸す image の記述は §5.1 の `tiling` を使う。(2) compositor は display の swapchain を、display が scanout でき client の GPU が
  OPTIMAL で共有できる時は OPTIMAL、他は LINEAR で作る（決めるのは compositor）。(3) window mode の chain と game mode の貸す image は、なるべく同じ並び・同じ parameter に
  する（p015 の「app は気づかない」入れ替えで、記録済みの command buffer が参照する image の並びが変わらない方が driver の入れ替えが小さい）。これは p015 の設計に
  任せ、p023 は並びの表し方と取り込みを先に用意する。
- 順序: 段 A（いつでも）→ 段 B・C（p015 の nvrtx の部分の前）→ 段 E（p015 の nvrtx の確かめの前）→ 段 D（p015 の game mode が main に入った後、§9.4）。

## 15. 段 E: nvrtx の display の block-linear の scanout（p015 向け）

- 変更: `nvrtx_disp_window_image`（`disp-records.c:990-1015`）が surface の並びを受け、block-linear の時は SET_STORAGE に MEMORY_LAYOUT = BLOCKLINEAR（0）・
  BLOCK_HEIGHT = ONE_GOB（0）、pitch の field に「行の bytes / 64」（nvrtx の tiled の行は GOB の幅 64 B の倍数なので同じ値）を入れる。
  Value referenced: NVIDIA open-gpu-doc `classes/display/clc57e.h`（MIT、`NVC57E_SET_STORAGE_BLOCK_HEIGHT`・`_MEMORY_LAYOUT`）。手順の参考: Linux v6.12
  `drivers/gpu/drm/nouveau/dispnv50/wndwc57e.c`（`wndwc57e_image_set`）と `wndw.c`（block-linear の pitch の値）。値だけを使い、名前・構造は写さない（Guardrail、WS037 の規則）。
- present の blob（`present.c:829-843`）は backing の記録（§6.3）から並びを取り、request の stride（OPTIMAL では 0）と記録を比べる。scanout の import は
  OPTIMAL の backing を、段 E が有効な時だけ受ける（`nvrtx.disp.bl` の起動の parameter、既定 0 で、確かめた後に 1）。無効の時は `ENOTSUP`（「scan できない」の既存の意味）。
- 危険 R3（未確認）: nouveau は ctxdma に memory の kind を与えて block-linear を読ませる。nvrtx の VRAM の ctxdma（RM が作る）と kind 0 の page で window が block-linear を
  正しく読むか。読まなければ、block-linear 用の ctxdma（kind 付き）を別に作るか、backing の page の kind を GSP の RPC で変える（深度の S8Z24 の kind の変更と同じ道
  `vk-execute.c` の `nvrtx_vk_memory_kind`）が要る。実機の probe の Phase で最初に確かめる。
- 確かめの方法の問題: keiland-shot は display の読んだ画素を見ない（compositor の Vulkan の読み戻し、§1.5）。自動の確かめには display の CRC（NVIDIA の head の CRC の
  notifier、nouveau の `crcc57d.c` の類）が要り、実装は約 3 LW 増える。CRC 無しなら、ユーザーが画面を見る試験として「ユーザーが拾える試験の一覧」に置く（AGENTS.md §3）。
  どちらにするかはユーザーに問う（user-questions Q3）。

## 16. 試験

### 16.1 host（各段で、build は warning 0）

- 段 A: `plan/ws291/tests/nvrtx-draws-bench.sh`（executor の host の bench）に「OPTIMAL・1 level の色 + 深度」の pass を足し、(1) 影の quad の数が 0、(2) 描いた結果を
  CPU で GOB から戻した画素が、今の kernel（影あり）の pitch の結果と全画素で一致、(3) present の copy（tiled → pitch）の結果も一致。既存の nvrtx-vk・vk-d3 の host 試験が 0 failure。
- 段 B: kernel の core の host 試験（`plan/ws014/tests/gpu-uapi-layout.c`・`gpu-sharing.c` の形）に、OPTIMAL の export（`image_layout` あり・なし、stride・offset が 0 でない、
  bytes が足りない）・別の device への import（`EXDEV`）・foreign の scanout の拒否を足す。nvrtx の `image_layout` と再 export の拒否の host 試験。
- 段 C: `vulkan_dedicated_check` の host 試験（`dedicated.c` は host で試せる形）に OPTIMAL の受け・拒み（usage 違い・tiling 違い）を足す。
- 段 D: compositor の `kwl_gpu_buffer_decode` の host 試験（OPTIMAL の受け、他の値の拒否）。protocol の event の符号化（libwayland の client の listener）。
- 段 E: `nvrtx_disp_window_image` の push の語（block-linear の時の SET_STORAGE と pitch）の host 試験（push の byte の比べ）。

### 16.2 QEMU（Venus、Q1 の割当で 1 本の script）

- Venus は OPTIMAL の共有に参加しない（§13）ので、版 4 の compositor が何も示さず、client が LINEAR・copy の経路に戻る事を確かめる: `plan/tools/boot-test.sh` で login の
  画面、window mode の Vulkan の app（mview `--windowed --content=none`）を keiland-shot で撮り、今の main の画と比べる（画素差 0）。console・serial の log では判定しない。

### 16.3 実機（10.0.10.3 の RTX 2070、Q1 の割当、lock `/tmp/zedbsd-rdna.lock`）

- 段 A（kernel だけの入れ替え）: `rtx-p013.sh OUT base=<main の vmunix> a=<段 A の vmunix>`（`SHOT=1 ROUNDS=2 PROF=1`、LIST は `rtx-ab.list`）。
  受け: subs-1000 が base より 30 % 以上速い（目安 約 16 fps、nc の 16.07 を上限の目安）、subs-100 も同様、big-100・big-1000・scene-1500・scene-5000 が base の揺れの内
  （p014 の表の幅）、one・each・subs の still の画素差 0（base の one とも）、PROF の perf の行で 1 pass に影の quad が無い事。
- desktop: `rtx-desk-main.sh`（RUNS=1）で launch・type・output・drag・home・wiseview の各段が 60 fps 前後（p014 の d1・desk2 と同じ幅）。
- 段 D（p015 の後）: window mode の bench を足す（mview `--windowed`・`--content=none`、big-1000 と subs-1000 と scene-5000、`rtx-p013.sh` の LIST に window 用の list を足す）。
  受け: window mode で present の copy が 0（PROF の行の WSI の copy の数と、`VULKAN_ZEDBSD_SUBMIT_TIMING` の present の submit）、fps が段 A の window mode 以上、
  画素差 0（keiland-shot は合成の結果を読む）。desktop の 60 fps。
- 段 E: probe の script（1 本、配置 → 起動 → block-linear の既知の模様の buffer と同じ内容の pitch の buffer を交互に flip → 回収 → 要約）で、display の例外の通知が無い事・
  flip の完了の数。画素の正しさは CRC（Q3 の答えによる）かユーザーの目（一覧に置く）。
- 各 window の終わりに file と kernel を戻して cksum 一致、Debian に戻る（今の script の手順）。

## 17. Phase と受け入れ条件（WS291 の Phase として Q1 が切る案）

| 案の Phase | 内容 | 依存 | 受け入れ | 見積もり |
| --- | --- | --- | --- | --- |
| p023 本体（この Queue） | 設計・レビュー・問い | — | design.md・design-review.md・user-questions.md | 済み |
| A（例 p024） | nvrtx の OPTIMAL の色の attachment を block-linear に（§4）、host 試験、実機の A/B と desktop | — | §16.1 段 A、§16.3 段 A と desktop | 実装 3 LW、実機 2 LW |
| B（例 p025） | `GPU_IMAGE_OPTIMAL`、core の確かめの委譲、`image_layout`、`GPU_CAP_OPTIMAL_SHARE`、nvrtx の export の記録と再 export の拒否（§6） | A | §16.1 段 B、amd64 の vmunix warning 0、他の GPU の config（rpi4・visionfive2・armv7 など driver を含む物）の build | 4 LW |
| C（例 p026） | libvulkan の OPTIMAL の export・import・dedicated の確かめ、deviceLUID の identity、external の答え（§7） | B | §16.1 段 C、QEMU の §16.2（LINEAR の後退なし） | 4 LW |
| E（例 p027、p015 と調整） | nvrtx の display の block-linear の scanout（§15）、probe | B、Q3 の答え | §16.1 段 E、§16.3 段 E（CRC なら自動、無ければ一覧） | 4 LW（CRC を入れるなら +3 LW） |
| D（例 p028） | 版 4 の protocol、compositor の示しと取り込み、WSI の Wayland の direct の chain と SUBOPTIMAL（§8〜§10） | C、p015 の game mode が main、Q1・Q2 の答え | §16.1 段 D、§16.2、§16.3 段 D | 6 LW |
| 回帰（D に含めるか別） | subs・big・scene・desktop・画素・QEMU の回帰を tip で 1 回 | D | 上の各受け | 2 LW |

- 8 LW は実時間で約 2〜3 時間（Q1 の換算）。A だけなら約 5 LW（1.5〜2 時間）。全部で約 23 LW（CRC 無し）。
- WS291 の最後の「規約の全文の見直し」の Phase は WS の既存の計画に従う（この設計で新しく足さない）。
- 各 Phase の C の code には `plan/coding-style.md` の全文を当て、source の comment に開発の管理情報（WS・Phase の番号）を書かない。

## 18. 危険と未確認

- R1: 段 A の効果の上限は run の固定費で決まる（subs-1000 は約 16 fps まで）。NVK の 75 fps には run の固定費の内訳（p014 の候補 (2)）が別に要る。
- R2: 段 A で色の attachment に CPU の copy（画素ごと）をする app が遅くなる（今の観測の範囲の app にあるかは未確認。PROF の行と desktop で見る）。
- R3: nvrtx の display が kind 0 の page の block-linear を読むか（§15）。未確認。
- R4: Wayland の direct の chain で、compositor が合成で image を持つ間 app が待つ時間が増え、`minImageCount` が足りない app が遅くなるか（§9.2）。未確認。
- R5: deviceLUID の zedBSD での意味付けは、Windows の D3D との相互運用を前提にした app には誤解の元（zedBSD にその app は無い）。
- R6: 今の Wayland の chain が copy を選ぶ理由が code に無い（§9.1）。direct にした時に今の copy が隠していた問題（例: 合成の読みと app の次の書きの順序、
  acquire の fence）が出る可能性。direct の display の経路（既にある）と同じ待ちの規則なので低いと見るが、未確認。
- 確かめていない事: 段 A〜E のどれも実装・実機で試していない（設計だけ）。nvrtx の BL の scanout の値は MIT の header と GPL の nouveau の手順から読んだだけで、
  hardware での確かめは無い。

## 19. 出典の扱い（Guardrail の「出典」）

- この設計の文書は plan/ の中の物で、code には出典を block ごとに書く（file の先頭には書かない）。段 E の値は NVIDIA open-gpu-doc（MIT）の `clc57e.h` を
  「Value referenced:」、手順は Linux v6.12 の nouveau（GPL-2.0、WS037 の規則で値と手順の参照は可、名前・構造・comment は写さない）を「Procedure referenced:」で、
  それぞれ該当の block の直前の複数行の comment に書く。
- 段 A の並びは nvrtx の既存の tiled の並び（Mesa 25.0.7 の nil の値を既に引用、`nvrtx-vk-objects.h:82-84`・`vk-resource.c:270-273`）を使うので新しい出典は無い。
- この文書に引いた code の行は zedBSD 自身の物（Zlib）と、上の 2 つの外部の file の名前と field の名前だけ。外部の code の文は写していない。

## 20. 判断の記録（主な物）

| 判断 | 選んだ物 | 根拠 | 選ばなかった物 |
| --- | --- | --- | --- |
| R1 の解き方 | nvrtx の中で OPTIMAL の色の attachment を block-linear（段 A） | 影の根が nvrtx の並びの規則（§1.1）で、Wayland の app の image は既に OPTIMAL。protocol 無しで pass ごとの copy が消える | 共有の image だけを block-linear にする（WSI の copy の経路では app の image が影を作り続ける）。私用の fence を wire で示す（0j の (a)、ユーザーが選ばなかった） |
| 並びの値 | 不透明の `GPU_IMAGE_OPTIMAL`（同じ device の中だけ） | 構造も ioctl も変えない、読む相手は同じ kernel の同じ driver | 64 bit の modifier、32 bit の vendor の code（§5.2） |
| 決める者 | compositor が示し、client が選ぶ | ユーザーの決定 | client が決める、試して戻る |
| protocol | 版 4 の `layout`・`layouts_done`（factory に、surface ごとでない） | 合成の可否は device で決まる、AHA | surface の feedback、`failed` の event、新しい interface（§8.3） |
| device の identity | deviceLUID に kernel の device_id | compositor は libvulkan だけを使う（Guardrail）、標準の型 | deviceUUID、独自の関数、`/dev/gpu0` を開く（§7.3） |
| 段 D の時期 | p015 の game mode の後 | 今の V1 の game mode が client の buffer を scanout するので、OPTIMAL にすると direct scanout の計測が崩れる（§9.4） | p015 の前に入れて surface ごとに示しを変える（surface の feedback が要る） |
| 段 A の対象 | 色の attachment の usage がある物だけ | 影はそこだけ、upload の CPU の copy を遅くしない | OPTIMAL の 1 level の全部 |
