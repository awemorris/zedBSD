<!-- awesome-plan project=zedbsd record=ws291-p023-design -->

# ws291-p023 設計: 共有の image の画素の並び（明示の並びの値・driver の好みの一覧・nvrtx の block-linear）

- 親: [phase.md](phase.md)・[WS291](../ws.md)。Queue: q1781-i01（P3、2026-10-31、worktree `p3-ws291wsi`、基点 main d1014941e）。
- 状態: 設計の第 2 版（2026-10-31）。ユーザーの答え（§8.1・§11 の I7）を同日に反映。第 1 版（commit 2b92488b1）を design-reviewer の指摘（[design-review.md](design-review.md)）と、その後のユーザーの決定と
  Q1 の interface の決め（下の §0.1）で書き直した。ユーザーへの残りの問いは [user-questions.md](user-questions.md)。
- この文書の行番号は基点 d1014941e の物。

## 0. 前提の決定と結論

### 0.1 この設計が従う決定（要点、ユーザーの決定は Q1 経由で受けた）

- 2026-10-31 ユーザー: RTX の subs-1000 は cockpit の 0j の (b)（画面に出す image を GPU の得意な並びにする）で進める（phase.md）。
- 2026-10-31 ユーザー: compositor の image の形式と並びは compositor が決め、client は compositor が示した物から選ぶ。
- 2026-10-31 ユーザー: game mode は WS309 p015（P2、q1782）で文字どおりの zero-copy にする（client は compositor が貸す image に直に描く、全 GPU）。
- 2026-10-31 ユーザー: compositor の image（合成の経路と game mode の貸す image の両方）は、GPU が描けて display が scanout できる形式と並びの組の中で一番速い物にする
  （block-linear・tiled・scanout が読めるなら圧縮）。画質を大きく落とさない（RGB565 などの色の深さを落とす形式は使わない）。好みの順は driver ごとの一覧にする。
- 2026-10-31 ユーザー: RTX の block-linear は必須で、実装する。OPTIMAL の色の image の block-linear の置き方（pass ごとの影の copy を無くす）と NVC57E の block-linear の
  scanout を、この設計の直後に p023 の実装として P3 が行う。nvrtx の部分はユーザーの問いで止めない。
- 2026-10-31 Q1 の interface の決め（P2 と P3 の案を合わせた物）: 並びは明示の値で名指す（render と display が別の device の機械 — VF2 の PowerVR と dc8200、Pi の V3D と HVS、
  ODROID の Mali と display — で両側が同じ並びを名指せるように）。driver の好みは ioctl 46 `GPU_IMAGE_LAYOUTS` で問う。共有の部分（UAPI・`gpu-share.h`・`gpu.c` の core）と
  nvrtx は P3、libvulkan・protocol・compositor の貸し出しと選び方は P2（`src/drivers/gpu/nvrtx/` と core には触れない）。HAL は変えない。

### 0.2 結論

1. **今の copy の根は nvrtx の並びの規則にある**。nvrtx は 1 level の色の image を、`VK_IMAGE_TILING_OPTIMAL` で作られても pitch-linear に置く（§1.1）。
   Wayland の swapchain の app の image は OPTIMAL（`wsi-swapchain.c:1626`）なので pitch-linear になり、深度付きの pass は毎回 block-linear の影に描いて全画面を copy する。
2. **nvrtx の段 A**（protocol も ABI も要らない）: OPTIMAL・1 level・色の attachment の image を block-linear（一 GOB の block）に置く。pass ごとの影の copy が無くなり、
   copy は present ごとの 1 回（app の image → 共有の image）だけになる。subs-1000 の見込みは約 11 → 約 16 fps（p014 の診断 nc の 16.07 fps が目安）。NVK の 75 fps との残りは
   run ごとの GPU の固定の約 56 µs（p014 の内訳）で、この設計の範囲の外。
3. **並びの表し方**: `gpu_image_descriptor.tiling` に family を上位 16 bit に持つ明示の値（例 `GPU_IMAGE_NV_BLOCK_LINEAR`）を足し、`reserved` の語を `layout_parameter`
   （NV では block の高さの log2 と page の kind）にする。構造の大きさは変えない。kernel の記録が正で、export → kernel → import → p015 の貸す image の metadata まで変えずに運ぶ。
4. **driver の好みの一覧**: `GPU_IMAGE_LAYOUTS`（ioctl 46）。形式・大きさ・要る用途（RENDER・SAMPLE・SCANOUT・SHARE）と display を入れ、速い順に最大 16 の
   {tiling, parameter, flags} を返す。口の無い driver には core が LINEAR を今の規則で答える（今の driver は何も変えずに今と同じ）。
5. **p015 への要点**: game mode で client が借りる image が pitch-linear だと、nvrtx では pass ごとの影の copy が戻る（subs-1000 は約 11 fps に戻る）。nvrtx は
   `GPU_IMAGE_LAYOUTS` で block-linear を SCANOUT 付きで先に答え、display が block-linear を scanout する（§8）ので、compositor が一覧の先頭を選べば game mode も影が出ない。
6. 効果の正直な見積もり: 段 A の後に残る copy は 1 frame に 1 回（1080p で GPU 約 29 µs と WSI の submit 1 回）。window mode の 60 fps の desktop では fps の差としてほぼ見えない。
   window mode の 0 copy（P2 の部分）の価値は fps より memory の帯域（1080p で 1 frame 約 16.6 MB の読み書き）と、game mode と共通の部品にある。

## 1. 読んだ事実（基点 d1014941e）

### 1.1 nvrtx の影の copy と image の並び

- 影を使う条件: 色の target が pitch-linear で深度の target がある時（`src/drivers/gpu/nvrtx/vk/vk-execute.c:1093-1098`）。3D engine は深度付きでは
  block-linear の色の target 0 にしか描かない（同 1136-1146 の comment、Mesa の `nvk_linear_render_copy` を手順の参考）。
- 影の確保と pass の始めの copy（LOAD の時、ただし前の pass が同じ target に copy して他が書いていなければ省く `execute_kept_holds`）: `vk-execute.c:1147-1224`・`1233-1265`。
  pass の終わりの影 → target の全画面の copy: `vk-execute.c:1276-1345`。
- **1 level の色の image は tiling に関わらず pitch-linear**: `vk-resource.c:309-322`（`resource_image_layout`、2 level 以上だけ `image->tiled = true`、326-339）。
  create の時に `info.tiling` は NV12 の確かめ（`vk-resource.c:424`）でしか見ない（`resource_create_image` 198-255）。create の chain は外部 memory の宣言 1 つを読み飛ばすだけ
  （共有の decoder `src/drivers/gpu/i915/vk/vk-codec.inc:1590` → `vk-codec.c:362-376`、`[present][sType][pNext = 0][handle types]` の形だけを知る）。
- block-linear の image は既に各所で扱える: 色の target（`vk-execute.c:2894-2896` → `exec-push.c:445-449`、一 GOB の block）、quad の source（`vk-execute.c:2965`・`2786-2790`）、
  sampler（`vk-execute.c:2005-2006`）、CPU の copy（`vk-execute.c:2604-2617`、画素ごとに GOB の位置）、memory の境界（`vk-memory.c:405`・`482`、tiled は 512 B）。
  影と mip の image は pitch の kind（kind 0）の page の上で block-linear に描いて読んでいる（3D engine と texture は kind 0 で block-linear が動く）。
  `vkGetImageSubresourceLayout` は tiled の image に空の layout を返す（`vk-resource.c:510-524`）。
- buffer ↔ image の copy は、buffer の始まりと行が揃う時だけ GPU（quad）で、他は CPU が BAR1 越しに行う（`vk-execute.c:147-148`・`2490-2505`）。tiled の image の CPU の
  copy は 1 画素 4 B ずつ（2604-2617）。OPTIMAL・色の attachment の image を詰めた buffer に読み出す app が実在する: notes（`userland/desktop/notes/render.c:1480-1481`・`1645-1656`）、
  libbrowser（`userland/desktop/libbrowser/paint/vulkan.c:477`・`1961-1962`）、libglesv2（`userland/desktop/libglesv2/framebuffer.c:707`・`3000`）、WSI の CPU の経路
  （`wsi-swapchain.c:3055`）（design-reviewer の指摘 5 で確かめた）。
- 行の丸め: pitch-linear の行は 256 B（`nvrtx-vk-objects.h:35`）、block-linear の行は 64 B（同 85-86）。高さは block-linear だけ 8 行に丸める。
- 形式の機能: 色の形式は optimal と linear が同じ（`vk-device.c:649-650`）。storage image は無い（`vk-device.c:707-709`）。
- mview の subs-N は 2 回目からの submit で色も深度も LOAD（`userland/tests/mview/renderer.c:1011-1032`・`2407-2470`）。
- 計測（[phase014](../phase014/phase.md) の q1764-i01）: subs-100 の GPU の 1 run は約 89 µs、copy を書かない診断（nc）で約 60 µs、copy 約 29 µs、run ごとの固定 約 56 µs。
  nc の subs-1000 16.07 fps、subs-100 153.6（r は 106）、big-1000 116.1（r と同じ）。

### 1.2 libvulkan の WSI

- 共有の image は常に LINEAR（`wsi-image.c:110`・`207`）、取り込みも LINEAR だけ（279-285・313）。
- swapchain の 2 つの経路: direct（app の image が共有の image、`VK_KHR_display` の primary plane だけ、`swapchain_direct_possible` 1740-1780、条件 1751-1753）と
  copy（app の image は OPTIMAL 1615-1650、共有の image を別に作り present ごとに `present_copy` 2549）。Wayland はいつも copy。
- direct の image の作成と export は `swapchain_group_create` の中（1139-1141、1518-1520）で、失敗はそのまま失敗（1153-1170 の copy への戻りは import の段だけ）。
  `vulkan_memory_image_fd` は export の ioctl の失敗を全部 `VK_ERROR_OUT_OF_DEVICE_MEMORY` にする（`memory.c:643-644`）（指摘 1）。
- acquire は copy の経路でも同じ番号の presentation の image の返りまで待つ（4099-4104）。
- Wayland: fd と 64 B の記述を `kl_gpu_buffer_v1.create_buffer` で送る（`wsi-wayland.c:623-675`）、factory は版 3 までで bind（1234-1236）、初期化の roundtrip は 1 回で
  factory はその中で bind（1200・1239）（指摘 8）。
- `vkGetMemoryFdKHR` は allocation の capability（`GPU_ALLOCATION_EXPORT`、`memory.c:198-207`）で image の記述を持たず、dedicated の import は image の capability だけを受ける
  （`memory.c:744-746`）（指摘 4）。nvrtx の deviceUUID は固定値（`vk-device.c:303`）。

### 1.3 kernel の共有の image の記述と import

- `struct gpu_image_descriptor`（`include/uapi/gpu.h:117-131`、64 B）は `tiling` と `reserved` を持つ。値は `GPU_IMAGE_LINEAR = 1` だけ（53）。
- export の `gpu_image_validate`（`gpu.c:5214-5266`）: `reserved != 0` を拒む（5226）、LINEAR 以外は `EOPNOTSUPP`（5234）。→ `reserved` を `layout_parameter` に変えても、
  今の呼び手（全部 0 を入れる）はそのまま通る。`reserved` を読む他の所: powervr（`render.c:1895`）・bcm2711（`share.c:348`）の同じ記述の比べ、mali の host 試験（`tests/mali-scanout-host-test.c:182`）。
- import（5489-5640）: 受け手の記述は空でなければ拒む、別の device は `GPU_IMPORT_SCANOUT` の時だけ（5560-5563）、foreign の scanout は `gpu_scanout_import`（5161-5212）。
  import した資源は記述を持たない（`struct gpu_resource` 155-172）。
- display の present（4282-4320）は request の stride・height で範囲を確かめる。stride 0 は 0 B として通る（4308-4310）（指摘 3・18）。
- driver の口: `struct drv_gpu_share_ops`（`include/drivers/gpu/gpu-share.h`）は export・release・import と 2 つの任意の物理の view。`struct drv_gpu_ops`
  （`include/drivers/gpu/gpu.h:124-158`）は末尾に任意の table の pointer を並べる（driver は `sizeof` と指定子つき・位置の初期化子の両方で作る、末尾の追加は互換）。
  display だけの node（powervr・mali の display）は share の table を持てない（`gpu.c:1381-1397`、share は BLOB と SHARE を要る）。
- ioctl の番号は 'G' の 0〜45 が使われている（46 は Q1 が `GPU_IMAGE_LAYOUTS` に割り当てた）。capabilities の空き bit は 131072 から上。

### 1.4 `kl_gpu_buffer_v1`・compositor・画面の撮影

- 版 3、event は無い（`libwayland/protocol.c:899-910`、`gpu-buffer-zedbsd.c:47-53`）。bind の hook（237-247）は何も送らない。
- 記述の解読は LINEAR だけ（`gpu-zedbsd.c:84`）、取り込みの image は LINEAR・SAMPLED（`gpu-buffer-zedbsd.c:494-541`）、失敗は `EPROTO`（157-162）。
  `uapi/gpu.h` を include してよいのは `gpu-zedbsd.c` と `scanout-zedbsd.c` だけ（`plan/tools/gpu-boundary/v1-check.sh`）。
- 今の game mode（V1 の暫定）は client の buffer を `GPU_IMPORT_SCANOUT`（`scanout-zedbsd.c:466`）で取り込み、**client が送った記述**（271 の `record->description`）の stride で
  present する（281）。nvrtx の `share_import` は flags を見ない（`share.c:155`）、present の blob は stride 0 も通す（`present.c:838`）（指摘 3）。
- 画面の撮影は compositor の swapchain の読み戻し（`wayland/shot.c:20-25`）。client の buffer を CPU が読む経路は無い。

### 1.5 nvrtx の display

- window の image は pitch だけを program する（`display/disp-records.c:194-201`・`1006-1011`、`nvrtx-disp.h:146-152`）。present の blob は request の stride で surface を作る
  （`present.c:829-843`）。export は記述を捨てる（`share.c:85-116`）。scanout の制約は pitch 64 B・offset 256 B（`share.c:216-245`）。
- hardware は block-linear を読む: NVIDIA open-gpu-doc `classes/display/clc57e.h`（MIT）の `NVC57E_SET_STORAGE` は BLOCK_HEIGHT（bit 3:0、ONE_GOB = 0）と MEMORY_LAYOUT
  （bit 4、BLOCKLINEAR = 0、PITCH = 1）。Linux v6.19 の nouveau（`dispnv50/wndwc57e.c` `wndwc57e_image_set`、`wndw.c` の atomic の確かめ）は block-linear の時 pitch の field に
  「行の bytes / 64」を入れ、ctxdma に memory の kind を与える（`nv50_wndw_ctxdma_new`）。**nvrtx の VRAM の ctxdma と kind 0 の page で window が block-linear を読むかは未確認**（R3）。

### 1.6 他の GPU

- amdrdna: 色は全部 pitch-linear（`amdrdna/vk/vk-resource.c:12`・`275`・`348`）。i915: 色は linear、深度は Y-tiled（`i915/vk/vk-image.c:420-421`）。powervr: linear と optimal
  （`powervr/vk/vk-image.c:200`・`297-301`）。powervr・bcm2711・mali の display は LINEAR の共有だけ（`powervr/render.c:1833`、`bcm2711/share.c:315`、`mali/display/scanout.c:52`）。
  venus は host が並びを決める（`gpu-op.h:117-127`）。深度付きで linear の色に描けない（影が要る）のは nvrtx だけ。

## 2. 範囲と分担

| 部分 | 持ち主 | 内容 |
| --- | --- | --- |
| 共有の部分（§4・§5） | P3（この設計の実装 I1） | UAPI（並びの値・`layout_parameter`・`GPU_IMAGE_LAYOUTS`・`GPU_CAP_IMAGE_LAYOUTS`）、`gpu-share.h`・`gpu.h` の任意の口、`gpu.c` の core の規則と既定の答え、`reserved` の改名に伴う powervr・bcm2711・mali の試験の直し |
| nvrtx（§6〜§8） | P3（I2〜I6） | 段 A の置き方、好みの一覧と確かめの口、私用の record の解読、export の記録と再 export の拒否、import と present の規則、block-linear の scanout、CPU の copy の速さ |
| libvulkan・protocol・compositor・WSI（§9） | P2（WS309 p015 と window mode） | 私用の pNext の encode、`vkGetImageSubresourceLayout` の値の記述への写し、選び方（renderer と display の一覧の先頭）、`kl_gpu_buffer_v1` の並びの field、貸し出し、失敗の戻り |

- HAL（`include/hal/hal.h`）は変えない。変えるのは UAPI と driver framework の内部の header（`gpu.h`・`gpu-share.h`）で、HAL の差分の案は無い（`proposed/` は作らない）。
- 範囲の外: run ごとの GPU の固定費（p014 の残り）、他の GPU の速い並びの実装（口だけを用意する。各 driver の WS が行う）、Linux・FreeBSD の backend。

## 3. 段の全体

| 段 | 何を | 持ち主 | 依存 |
| --- | --- | --- | --- |
| I1 | 共有の部分（UAPI・core・口）、host 試験、全 port の kernel の build | P3 | — |
| I2 | R3 の probe（nvrtx の display が kind 0 の block-linear を読むか）を先に、診断の patch で | P3 | — |
| I3 | nvrtx 段 A（OPTIMAL の色の attachment を block-linear に）と CPU の copy の 16 B 化 | P3 | — |
| I4 | nvrtx の口（好みの一覧・確かめ）、私用の record、export の記録、import・present の規則、`vkGetImageSubresourceLayout` | P3 | I1 |
| I5 | nvrtx の block-linear の scanout（SET_STORAGE）と present の並び | P3 | I2・I4 |
| I6 | 実機の A/B・desktop の fps と画素・回帰 | P3（Q1 の割当） | I3〜I5 |
| P2 側 | libvulkan・protocol・compositor・WSI | P2 | I1（merge） |

## 4. 並びの表し方（I1、UAPI）

### 4.1 `tiling` の値の番号の付け方

- `tiling` の上位 16 bit を family、下位 16 bit を family の中の並びにする。family 0 は共通（`GPU_IMAGE_LINEAR = 1`、今のまま）。
- family の番号を予約する: 0 共通、1 NVIDIA、2 AMD、3 Intel、4 Imagination（PowerVR）、5 Arm（Mali）、6 Broadcom（V3D・HVS）、7 VeriSilicon（dc8200）。番号の予約だけで、
  並びの値は driver が実装する時に足す。
- 今回足す値は 1 つ: `GPU_IMAGE_NV_BLOCK_LINEAR = 0x00010001`（NVIDIA の 2D block-linear、GOB は 64 B × 8 行、block は幅 1 GOB・高さ 2^h GOB）。
- 意味の定義（NV の block-linear）: `stride` は行の bytes（64 の倍数）、`offset` は 512 の倍数、image の bytes は `stride × align(height, 8 × 2^h)`。
  `layout_parameter` の bit 3:0 = h（0〜5）、bit 15:8 = page の kind（0 = device の既定）、他の bit は 0。今 nvrtx が作るのは h = 0・kind 0（`layout_parameter = 0`）。
  画素の位置は GOB の中で 16 B の piece に分かれる（Value referenced: Mesa 25.0.7 `src/nouveau/nil/copy.rs` の GOB の 16 B、nvrtx が既に `execute_gob_offset` で引用）。
- LINEAR の `layout_parameter` は 0 でなければならない（今の `reserved == 0` の規則と同じ）。

### 4.2 `reserved` → `layout_parameter`

- `struct gpu_image_descriptor` の `uint32_t reserved;` を `uint32_t layout_parameter;` に改名する（位置・大きさは同じ）。今の呼び手は全部 0 なので互換。
- 改名で直す所: `gpu.c:5226`、`powervr/render.c:1895`、`bcm2711/share.c:348`、`tests/mali-scanout-host-test.c:182`（全部 I1 で P3 が直す。userland の libvulkan・compositor は
  `reserved` を名で読んでいない）。

### 4.3 `GPU_IMAGE_LAYOUTS`（ioctl 46）

```c
#define GPU_CAP_IMAGE_LAYOUTS		131072U		/* the core sets it when the backend has the layout operations */
#define GPU_LAYOUT_RENDER		1U		/* a colour attachment the renderer draws to */
#define GPU_LAYOUT_SAMPLE		2U		/* sampled by the renderer */
#define GPU_LAYOUT_SCANOUT		4U		/* the display named by display_id scans it out */
#define GPU_LAYOUT_SHARE		8U		/* exported and imported between opens of this device */
#define GPU_IMAGE_LAYOUTS_MAX		16U

struct gpu_image_layout {
	uint32_t tiling;
	uint32_t parameter;
	uint32_t flags;
	uint32_t reserved;
};

struct gpu_image_layouts {
	uint32_t version;
	uint32_t size;
	uint32_t format;		/* GPU_PIXEL_* */
	uint32_t width;
	uint32_t height;
	uint32_t uses;			/* GPU_LAYOUT_* every listed entry must have */
	uint32_t display_id;		/* 0: no display is asked about */
	uint32_t count;			/* out */
	struct gpu_image_layout entry[GPU_IMAGE_LAYOUTS_MAX];	/* out, fastest first */
};

#define GPU_IMAGE_LAYOUTS		_IOWR('G', 46, struct gpu_image_layouts)
```

- 入力の確かめ（core）: version・size、format は BGRA8888・RGBA8888（色の深さを落とす形式は無い）、width・height は 0 でない、uses は既知の bit だけ、SCANOUT を求めるなら
  display_id は 0 でない、count と entry は全部 0（古い出力を受けない）。
- 出力の確かめ（core、driver の答えに対して）: count ≤ 16、各 entry の flags は既知の bit で uses を全部含む、SCANOUT は display_id が 0 でない時だけ、LINEAR の parameter は 0、
  LINEAR でない値は family が予約の範囲、reserved は 0、count より後の entry は 0。破れば `EIO`。
- 既定（driver に口が無い時、core が答える）: LINEAR を 1 つ、flags は RENDER|SAMPLE（`GPU_CAP_COMMAND` の時）、SHARE（`GPU_CAP_SHARE` の時）、SCANOUT（display_id が 0 でなく
  `GPU_CAP_DISPLAY` の時。どの generation で使えるかは今の `GPU_DISPLAY_CONSTRAINTS` が別に答える）。uses を満たさなければ count 0。→ 今の driver は何も変えずに今の答え。
- `GPU_INFO` の capabilities に、core が「口がある」から `GPU_CAP_IMAGE_LAYOUTS` を立てる（driver の登録の capabilities に入れるのは拒む、今の未知の bit の規則のまま）。

## 5. kernel の core の規則（I1、`gpu.c`・`gpu-share.h`・`gpu.h`）

### 5.1 任意の口

display だけの node も一覧と確かめを答えられるように、share の table とは別の table にし、`struct drv_gpu_ops` の末尾に pointer を足す（定義は `gpu-share.h`）。

```c
struct drv_gpu_layout_ops {
	/* Bytes from the descriptor's offset a non-linear image needs; EOPNOTSUPP for a layout this device cannot read or write. */
	int (*image_layout)(void *device, const struct gpu_image_descriptor *image, uint64_t *bytes);
	/* The device's layouts for the request, fastest first; the core checks the answer. */
	int (*image_layouts)(void *device, void *session, struct gpu_image_layouts *request);
};
```

- 登録の確かめ（`gpu_ops_validate`）: table があれば 2 つとも NULL でない事。

### 5.2 規則

| 場面 | 規則 |
| --- | --- |
| export（`gpu_image_validate`） | LINEAR: 今のまま、`layout_parameter == 0`。LINEAR でない: version・size・device_id・資源・形式・usage・width・height は今と同じ確かめ、`offset ≤ 資源の bytes`、device の `image_layout` が bytes を返し `bytes ≤ 資源の bytes − offset`。口が無ければ `EOPNOTSUPP`（今と同じ結果） |
| 同じ device の import | 今のまま（export で確かめた記録を返す） |
| 別の device の import | 今のまま `GPU_IMPORT_SCANOUT` の時だけ。LINEAR でない記述は、受ける device の `image_layout` がその記述を受ける（0 を返し bytes が allocation に収まる）時だけ、他は `EXDEV`。その後の `gpu_scanout_import` と display driver の `import_image` は今のまま |
| 再 export（同じ allocation を別の記述で） | core は資源ごとの export を追わないので、driver が拒む（`EINVAL`、`gpu-share.h` の契約に書く。powervr・bcm2711 の `same_image` と同じ） |
| scanout できない並び | driver の `import_resource`（`GPU_IMPORT_SCANOUT`）・`import_image` が `ENOTSUP`（今の契約の意味） |
| display の present（BLOB） | core は今の範囲の確かめを保つ。LINEAR でない資源の範囲は core には分からないので、driver が backing の記録の bytes で確かめる事を契約にする（`gpu.h` の present の注に書く） |
| `GPU_IMAGE_LAYOUTS` | §4.3 |

### 5.3 host 試験（I1）

- `plan/ws014/tests/gpu-uapi-layout.c` に新しい構造の大きさ・offset・ioctl の値の確かめを足す（ILP32・LP64）。
- core の振る舞い（既定の答え、driver の答えの確かめ、LINEAR でない export の確かめ、別の device の import の `EXDEV`）は、`plan/ws014/tests/gpu-framework.c`・`gpu-sharing.c`
  の fixture で試す（今は runner が無いので、`plan/tools/gpu-boundary/run-dedicated-host.sh` の形の短い runner を `plan/ws291/tests/` に置く）。build できない時はその事を報告し、
  kernel の全 port の build（warning 0）と、決まった値の照合だけにする。

## 6. nvrtx 段 A: OPTIMAL の色の attachment を block-linear に（I3）

### 6.1 規則

- `resource_image_layout` に create の tiling・usage と私用の record（§7.2）を渡す。record が無い時: **`VK_IMAGE_TILING_OPTIMAL`・1 level・色の形式・`COLOR_ATTACHMENT`** を
  `GPU_IMAGE_NV_BLOCK_LINEAR`（h = 0、2 level 以上の tiled の level 0 と同じ置き方）にする。他は今のまま。
- record がある時: record の並びに従う（LINEAR か NV の block-linear で h = 0。他の h・kind は `VK_ERROR_FORMAT_NOT_SUPPORTED`）。usage に依らない（取り込む側が usage を変えても
  並びは record で決まる、指摘 2・10 の答え）。
- LINEAR の image は今のまま（影は残る。`VK_KHR_display` の direct の chain と app が明示に LINEAR を選んだ時だけ）。

### 6.2 CPU の copy の速さ（指摘 5 の答え）

- tiled の image の CPU の copy（`vk-execute.c:2604-2617`）を、GOB の中で続いている 16 B（4 画素）の piece ごとに写す形にする（`execute_gob_offset` の置き方で 16 B は連続）。
  行の始まりと終わりの端は画素ごと。これで BAR1 越しの書き読みの回数が約 1/4 になる。
- 確かめ: host の bench（`nvrtx-draws-bench.sh`）に「1 level の tiled の色の image ↔ 幅の揃わない buffer」の画素の照合を足す。実機の desktop の確かめに notes と browser を入れる（§10）。

### 6.3 影響の表

| 所 | 段 A の後 |
| --- | --- |
| 色の target | tiled → 影の条件（1093）に入らない |
| present の copy（app の image → 共有の LINEAR） | tiled の source → pitch の target の quad（影の終わりの copy と同じ形） |
| CPU の copy | 16 B ごと（§6.2） |
| `vkGetImageSubresourceLayout` | 1 level の tiled も offset 0・rowPitch = 行の bytes・size を返す（§7.3、OPTIMAL に対する値は仕様の外の zedBSD の約束） |
| memory | 行は 64 B、高さは 8 行に丸める。1920x1080 は pitch-linear と同じ 8,294,400 B |
| compositor の中間の target（glass・blur） | block-linear で描き tiled の header で sample。desktop の画素の照合で確かめる（§10） |

### 6.4 見込み

subs-1000 約 10.9 → 約 16 fps、subs-100 約 106 → 約 150。big-1000・scene は揺れの内。desktop は後退しない見込み。

## 7. nvrtx の口と記録（I4）

### 7.1 好みの一覧と確かめの口

- `image_layouts`: 形式 BGRA・RGBA に、[NV block-linear h = 0（RENDER|SAMPLE|SHARE、display_id が nvrtx の display で block-linear の scanout が有効なら |SCANOUT）、LINEAR（RENDER|SAMPLE|SHARE、
  display_id があれば |SCANOUT）] の順で、uses を満たす物だけを返す。width・height は 8192 以下（`NVRTX_VK_IMAGE_SIDE_MAX` と display の上限の小さい方、実装で確かめる）。
- `image_layout`: NV block-linear で h = 0・kind 0、stride が 64 の倍数で `width × 4` 以上、offset が 512 の倍数の時、`stride × align(height, 8)` を返す。他は `EOPNOTSUPP`。

### 7.2 私用の record（P2 と合わせる、Q1 経由）

- libvulkan は LINEAR 以外の並びを `VK_IMAGE_TILING_OPTIMAL` と私用の pNext の構造で頼む。wire の形の案（P2 が encode、nvrtx が decode）:
  - sType: `GPU_OP_RECORD_IMAGE_LAYOUT = 0x7a650001`（Khronos の番号の範囲の外、`include/uapi/gpu-op.h` に置く。今の 2 つの私用の record は Venus の extension 385 の offset を
    借りているが、385 の空きの offset は Mesa が後で使い得るので借りない）。
  - 本体: `[u32 tiling][u32 parameter]`。chain は Venus の protocol の pNext の形（`[u64 present][u32 sType][次の pNext][本体]`）で、外部 memory の宣言と並んでよい。
  - `GPU_CAP_IMAGE_LAYOUTS` のある context にだけ送る（今の decoder は外部 memory の宣言 1 つしか知らないので、他の driver に送ると読み違える）。
- nvrtx の decode: nvrtx の `resource_create_image` で、共有の `i915_vkc_dec_VkImageCreateInfo` の代わりに pNext の chain を一般に読む nvrtx の decoder（外部 memory の宣言と
  layout の record を知り、他の sType は `EINVAL`）を使う。共有の decoder（i915 の file）は変えない。

### 7.3 `vkGetImageSubresourceLayout`

- 1 level の tiled の色の image にも offset 0・rowPitch・size を返す（P2 の libvulkan がこれを記述の stride に写す）。2 level 以上の tiled は今のまま空。

### 7.4 export の記録・再 export・bind

- `share_export` は記述を backing に記録する（LINEAR か NV block-linear、width・height・format・stride・offset・parameter）。同じ backing の 2 回目の export は、記述が同じなら通し、
  違えば `EINVAL`。
- 正しさの守り（指摘 12）: export する側の image が bind された時（`vk-memory.c` の bind）に、image の並び（tiled・pitch・width・height）を backing に「最初に bind された並び」として
  記録し、export の記述と比べる（違えば export を `EINVAL`）。import した側の bind も、記録があれば同じ並びの image だけを受ける。範囲の安全は今の bind の確かめ（image の bytes が
  memory の残りに収まる）で既に守られる。

### 7.5 import と present の規則（指摘 3 の答え）

- `share_import`: `GPU_IMPORT_SCANOUT` で、backing の記録が NV block-linear で block-linear の scanout が無効（`nvrtx.disp.bl=0`）なら `ENOTSUP`。
- present の blob（`present.c:829-843`）: stride 0 を拒む。backing の記録がある時は、request の stride・width・height・format が記録と同じ事を確かめ、並びは記録から取る
  （block-linear なら §8 の SET_STORAGE、範囲は記録の bytes）。記録の無い blob（export されていない、今の compositor の自分の image など）は今の LINEAR の扱い。
- 今の game mode（V1）の守り: compositor の backend（`scanout-zedbsd.c`）が client の送った記述でなく kernel の返す記述（`import.image`）を使い、LINEAR でない物を断る直しは
  P2 側（userland）に頼む（Q1 経由）。nvrtx の上の 2 つで、誤った画面が黙って出る事は kernel の側で起きない。

## 8. nvrtx の block-linear の scanout（I2・I5）

- I2（probe、先に）: 診断の patch（commit しない、`plan/ws291/tests/` に patch で置く）で、既知の模様を block-linear と pitch の 2 つの blob に描き、SET_STORAGE を block-linear に
  した window で交互に出す。display の例外の通知（`nvrtx.disp` の notifier の状態）が無い事・flip の完了を確かめ、画素は CRC か目で見る（§8.1）。読めない時は block-linear 用の
  ctxdma（kind 付き、RM の ctxdma の作成の parameter）か backing の page の kind の変更（`nvrtx_vk_memory_kind` の GSP の RPC、深度の S8Z24 と同じ道）を試す。
- I5: `nvrtx_disp_window_image`（`disp-records.c:990-1015`）が surface の並びを受け、block-linear の時 SET_STORAGE に MEMORY_LAYOUT = BLOCKLINEAR（0）・BLOCK_HEIGHT = h、
  pitch の field に「行の bytes / 64」を入れる。`struct nvrtx_disp_surface`（`nvrtx-disp.h:356-363`）に並びの field を足す。起動の parameter `nvrtx.disp.bl`（既定は I2 の結果と §8.1 の答えで決める）。
  - Value referenced: NVIDIA open-gpu-doc `classes/display/clc57e.h`（`NVC57E_SET_STORAGE_BLOCK_HEIGHT`・`_MEMORY_LAYOUT`）。Procedure referenced: Linux v6.19
    `drivers/gpu/drm/nouveau/dispnv50/wndwc57e.c`（`wndwc57e_image_set`）と `wndw.c`（block-linear の pitch の値）。値だけを使い、名前・構造・comment は写さない（Guardrail の WS037 の規則）。
- cursor・overlay の plane は今のまま pitch。

### 8.1 画素の確かめ（ユーザーの答え済み）

keiland-shot は display が読んだ画素を見ない（§1.4）。2026-10-31 ユーザーから、CRC は作らず目で見て確かめる、見るまでは block-linear の scanout を起動の parameter
で既定 0 にする、その間 RTX の game mode は 1 frame に 1 回の copy のままでよい、という趣旨の答えを受けた。→ `nvrtx.disp.bl`（既定 0）。目視の手順は
[phase.md](phase.md) の「ユーザーの目視の確かめ」。P3 の自分の確かめ（I2 の probe、display の例外の通知と flip の完了）は行う。nvrtx は `nvrtx.disp.bl=0` の間、
`GPU_IMAGE_LAYOUTS` の block-linear に SCANOUT を付けず、`GPU_IMPORT_SCANOUT` の block-linear を `ENOTSUP` で断る（compositor は LINEAR を貸す）。

## 9. P2 側との約束（libvulkan・protocol・compositor・WSI）

この設計は P2 の部分を実装しない。design-reviewer の指摘のうち P2 の部分に効く物を、約束として渡す（Q1 経由）。

1. 選び方: renderer の一覧（RENDER|SAMPLE）と display の node の一覧（SCANOUT）の両方に載る最初の並び。同じ device なら同じ fd、別の device なら両方の fd（Q1 の決め）。
   window mode の合成だけなら SCANOUT は要らない。
2. 共有の image の usage には SAMPLED と TRANSFER_SRC を足し、取り込む側は記述の usage そのもので作る（Q1 の決め）。並びは私用の record で決まり usage には依らない（§6.1）。
3. 失敗の戻り（指摘 1）: export の ioctl の `EOPNOTSUPP`・`ENOTSUP`・`EINVAL` を `VK_ERROR_FEATURE_NOT_PRESENT` に写し、direct の group の作成の失敗でも copy の経路へ戻る。
4. 貸す image の export の経路（指摘 4）: p015 の判断 3 は `vkGetMemoryFdKHR`（allocation の capability、記述を持たない）を推していたが、並びを kernel の記録で運ぶには
   image の capability（`GPU_RESOURCE_EXPORT`）が要る（`GPU_IMPORT_SCANOUT` も image の capability を要る `gpu.c:5550-5553`）。p015 の「app は気づかない入れ替え」は同じ
   VkImage の memory の付け替えになり得るが、nvrtx は 2 回目の bind を断る（`vk-memory.c:424`）。P2 の設計で決めてもらう（nvrtx に要る物があれば P3 に返す）。
5. protocol の bind と event（指摘 8・13）: 版を上げて event で並びを送るなら、WSI は 2 回目の roundtrip が要る。`uapi/gpu.h` の値を compositor の backend で作るのは
   `gpu-zedbsd.c` に置く（v1-check）。版 1〜3 の client が新しい compositor で動く確かめに `forge-guest.sh`・`fence-guest.sh` を使う。
6. 今の V1 の game mode が client の buffer を scanout する間は、`scanout-zedbsd.c` が kernel の記述（`import.image`）を使い LINEAR でない物を断る（指摘 3）。
7. deviceLUID の案（第 1 版 §7.3）は取り下げた（`GPU_IMAGE_LAYOUTS` と fd で device を問えるので要らない。指摘 9）。
8. compositor の側の LINEAR でない取り込み（2026-10-31 P2 の指摘を受けて足した。設計は P3、実装は P2）: game mode を外れた（withdraw の）後、compositor は貸した buffer を
   `kl_gpu_buffer_v1` の取り込み（今は LINEAR だけ）で合成する。nvrtx が block-linear を SCANOUT 付きで答える（`nvrtx.disp.bl=1`）前に、次が要る:
   - `gpu-zedbsd.c` の `kwl_gpu_buffer_decode`: `tiling` が LINEAR でない記述を受ける（`stride`・`offset` は記述のまま、範囲は kernel の記録が正なので allocation の中かだけを見る）。
     `struct kwl_buffer_layout` に tiling・layout_parameter・usage を足す。kernel の型を読むのは今どおりこの file だけ（v1-check）。
   - `gpu-buffer-zedbsd.c` の `buffer_image`: LINEAR でない時は `VK_IMAGE_TILING_OPTIMAL`、usage は記述の usage、pNext に私用の record（sType `GPU_OP_RECORD_IMAGE_LAYOUT`、
     {tiling, parameter}）を付ける。record の構造の型は libvulkan が公開の header（zedBSD の拡張の header）に置き、compositor は標準の Vulkan の呼び出しに chain する
     だけにする（compositor は libvulkan だけを使う、Guardrail）。row pitch の比べは LINEAR の時だけ（LINEAR でない時は libvulkan の dedicated の確かめと nvrtx の bind の確かめ）。
   - libvulkan の dedicated の確かめ（`dedicated.c`）: LINEAR でない記述は image の tiling が OPTIMAL・record が記述と同じ・usage が記述と同じ事を確かめる。
   - nvrtx の側は済み（499f18262）: record の decode、記述のある backing への bind は同じ形だけ（`nvrtx_vk_memory_bind_image`）。
   - それまでは nvrtx の SCANOUT の block-linear は出ない（`nvrtx.disp.bl` 既定 0）ので、貸す image は LINEAR のまま（P2 と Q1 の合意どおり）。

## 10. 試験

### 10.1 host（build は warning 0）

- I1: §5.3。kernel の全 port の build（`$(BUILD)/vmunix` を各 `config/ci` の config で、warning 0）。
- I3: `nvrtx-draws-bench.sh` に (1) OPTIMAL・1 level の色 + 深度の pass で影の quad が 0、(2) 結果を GOB から戻した画素が今の kernel（影あり）の pitch の結果と全画素で一致、
  (3) present の copy（tiled → pitch）の一致、(4) tiled ↔ 幅の揃わない buffer の CPU の copy の一致、を足す。既存の nvrtx-vk・vk-d3 の host 試験 0 failure。
- I4: nvrtx の `image_layouts`・`image_layout`・私用の record の decoder（chain の順の 2 通り、未知の sType の拒否）・再 export の拒否の host 試験。
- I5: `nvrtx_disp_window_image` の push の語（block-linear の SET_STORAGE と pitch）の host 試験。

### 10.2 QEMU

- I1 の後退の確かめ（Venus、Q1 の割当で 1 本の script）: `plan/tools/boot-test.sh` の login の画面。今の driver は既定の答えで変わらない事。console・serial の log では判定しない。

### 10.3 実機（10.0.10.3 の RTX 2070、Q1 の割当、lock `/tmp/zedbsd-rdna.lock`）

- I2: probe の script（1 本: 配置 → 起動 → 交互の flip → 回収 → 要約）。
- I3〜I5 の A/B: `rtx-p013.sh OUT base=<main の vmunix> new=<tip の vmunix>`（`SHOT=1 ROUNDS=2 PROF=1`、LIST `rtx-ab.list`）。受け:
  - **構造**: PROF の perf の行で subs の 1 pass に影の quad が 0（主の受け。fps は揺れがある）。
  - fps: subs-1000 が base より速い（目安 約 16 fps、上限の目安 nc の 16.07）、subs-100 も同じ向き。big-100・big-1000・scene-1500・scene-5000 は p014 の表の揺れの内。
  - 画素: one・each・subs の still の差 0（base の one とも）。
- desktop: `rtx-desk-main.sh`（RUNS=1）の各段が 60 fps 前後（p014 の d1・desk2 と同じ幅）。加えて、固定の desktop の場面（glass・blur のある window、notes、browser）を
  keiland-shot で撮り、base と tip で画素差 0（指摘 6）。notes と browser の CPU の読み出しの時間が base から大きく伸びない事（PROF の CPU の copy の行）。
- block-linear の scanout（`nvrtx.disp.bl=1`）: game mode（今の V1 では client の buffer は LINEAR のまま、P2 の部分の後に block-linear）で flip が続く事、display の例外が無い事。画素は §8.1。
- 各 window の終わりに file と kernel を戻して cksum 一致、Debian に戻る（今の script の手順）。

## 11. Phase と受け入れ（p023 の実装、P3）

| 段 | 内容 | 受け入れ | 見積もり |
| --- | --- | --- | --- |
| I1 | UAPI・core・口・改名・host 試験・全 port の build | §5.3、全 port の kernel warning 0、別に commit して SHA を Q1 へ（早く merge して P2 が使う） | 3 LW |
| I2 | R3 の probe（診断の patch と実機の script） | probe の結果を記録（読める・kind が要る・読めない） | 2 LW |
| I3 | 段 A と CPU の copy の 16 B 化 | §10.1 I3、§10.3 の A/B（構造・fps・画素）と desktop | 実装 3 LW、実機 2 LW |
| I4 | nvrtx の口・私用の record・記録・import と present の規則・subresource layout | §10.1 I4、amd64 の vmunix warning 0 | 4 LW |
| I5 | block-linear の scanout と present（`nvrtx.disp.bl`、既定 0） | §10.1 I5、§10.3 の block-linear の scanout（P3 の確かめ）、画素はユーザーの目視（phase.md） | 3 LW |
| I6 | tip で回帰をまとめて 1 回 | §10.3 の全部 | 2 LW |
| I7（= ws291-p024、別の Phase） | subs の run ごとの固定費（約 56 µs）を 1 つずつ測って削る（2026-10-31 ユーザーの答え）: run の始めの texture pool と shader cache の無効化、最初の draw の全 state、pass の始めと終わりの barrier の WFI、run の終わりの fence の WFI、channel の切り替えの有無。各項を外す診断の kernel（`plan/ws291/tests/nvrtx-run-cost-diag.patch` と `rtx-runcost.sh` の変種 0〜5、p014 で用意済み・未計測）で 1 項ずつ測り、要らない物を外す | I3〜I6 | 各項の前後の subs-100・subs-1000 の GPU の 1 run の µs と fps（PROF）、画素差 0、big・scene・desktop の後退なし | 計測 3 LW、削る実装 3〜6 LW（何が削れるかで変わる） |

- I1〜I6 の合計 約 19 LW（8 LW ≈ 2〜3 時間の換算で約 5〜7 時間）、I7（= ws291-p024）は別の Phase で 6〜9 LW。I1 は済み（7b0a6644e）、I2 と I3 は I1 と独立に進められる。
- 各 Phase の C には `plan/coding-style.md` の全文を当て、source の comment に開発の管理情報を書かない。出典は該当の block の直前の複数行の comment（Guardrail）。
- WS291 の最後の「規約の全文の見直し」の Phase は WS の既存の計画に従う。

## 12. 失敗と回復

| 失敗 | 振る舞い |
| --- | --- |
| driver に口が無い | 既定の答え（LINEAR）、LINEAR でない export は `EOPNOTSUPP`（今と同じ） |
| driver の答えが壊れている | core が `EIO`（呼び手は LINEAR に戻る、P2 の約束） |
| LINEAR でない記述の別の device への import | 受ける device の確かめが通らなければ `EXDEV` |
| 再 export の食い違い・bind の並びの食い違い | nvrtx が `EINVAL` |
| block-linear の scanout が無効・読めない | `share_import`（SCANOUT）が `ENOTSUP`、compositor は合成で出す（今の意味） |
| present の stride 0・記録との食い違い | nvrtx の present が `EINVAL` |
| 私用の record の未知の値 | nvrtx が `VK_ERROR_FORMAT_NOT_SUPPORTED`（libvulkan は LINEAR に戻る） |
| GPU の reset | 今の扱い。並びの仕組みは新しい回復を持たない |

## 13. 他の GPU への広げ方

- driver は `drv_gpu_layout_ops` を実装し、自分の family の値（予約の範囲）を足すだけで参加する。core・libvulkan・compositor・protocol は変えない。
- 別の device の組（VF2・Pi・ODROID）は、render の driver が並びを書け、display の driver が同じ値を SCANOUT で答え `image_layout` で受ければ、foreign の scanout の import が通る。
- 各 GPU の速い並び（amdrdna の swizzle・DCC、i915 の Tile4・CCS、powervr の twiddled・圧縮、V3D の UIF、Mali の AFBC）は各 driver の WS の判断で、この設計の Phase には入れない。

## 14. 危険と未確認

- R1: 段 A の効果の上限は run の固定費（subs-1000 は約 16 fps まで）。
- R2: 段 A で色の attachment を CPU で読み出す app（notes・browser・libglesv2）が遅くなる。§6.2 で緩め、§10.3 で測る。
- R3: nvrtx の display が kind 0 の page の block-linear を読むか（I2 で最初に確かめる）。
- R4: p015 の貸し出しの経路（allocation か image の capability か、memory の付け替え）が nvrtx の 1 回だけの bind と合うか（§9 の 4、P2 の設計待ち）。
- R5: 私用の sType の値が将来の Khronos の値と衝突しない事（範囲の外の値を選んだ）。
- 未実施: 全部（設計だけ）。block-linear の scanout の値は MIT の header と GPL の nouveau の手順から読んだだけで、hardware での確かめは無い。

## 15. 出典の扱い

- code の出典は該当の block の直前の複数行の comment（「Value referenced:」「Procedure referenced:」）に書き、file の先頭には書かない。
  NVIDIA open-gpu-doc（MIT）は値、Linux v6.19 の nouveau（GPL-2.0、WS037 の規則で値と手順は可、名前・構造・comment は写さない）は手順。
  段 A の並びは nvrtx の既存の tiled の並び（Mesa 25.0.7 の nil の値を既に引用）。
- この文書は zedBSD の code の行と、外部の file の名前と field の名前だけを引き、外部の code の文は写していない。

## 16. 判断の記録

| 判断 | 選んだ物 | 根拠 | 選ばなかった物 |
| --- | --- | --- | --- |
| 影の copy の消し方 | nvrtx の中で OPTIMAL の色の attachment を block-linear（段 A） | 根は nvrtx の並びの規則で、app の image は既に OPTIMAL。protocol 無しで消える | 共有の image だけ block-linear（copy の経路の app の image が影を作り続ける）。私用の fence を wire で示す（0j の (a)） |
| 並びの値 | 明示の値（family 上位 16 bit）と `layout_parameter` | Q1 の決め。別の device の組で両側が名指せる | 第 1 版の不透明な `GPU_IMAGE_OPTIMAL`（同じ device の中だけ。別の device の組に広がらない）、64 bit の modifier（構造の ABI が変わる） |
| 好みの申告 | ioctl 46 `GPU_IMAGE_LAYOUTS`、口の無い driver は core の既定 | 今の driver を変えない、display だけの node も答える | share の table の口（display だけの node が持てない） |
| 段 A の対象 | 色の attachment の usage がある物（record の無い時） | 影はそこだけ、upload の CPU の copy を増やさない | OPTIMAL の 1 level の全部 |
| CPU の copy | 16 B の piece ごと | 実在の読み出し（notes・browser・libglesv2）の後退を抑える | copy engine（今は block-linear → pitch の方向だけ、`exec-copy.c`）、作業用の buffer（memory が増える） |
| 私用の record の decode | nvrtx の decoder（共有の i915 の decoder は変えない） | 他の driver に影響しない | 共有の decoder の chain の一般化（i915 の file は P3 の範囲の外） |
| block-linear の scanout の確かめ | probe を最初に（I2） | R3 が成否を決める（指摘 11） | 段 B の後に置く（第 1 版） |
