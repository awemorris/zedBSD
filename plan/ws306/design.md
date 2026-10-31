<!-- awesome-plan project=zedbsd record=ws306-design -->

# WS306 設計: GPU の「ゲーム最適化」の UAPI と driver の識別（p001、第 3 版）

> 番号の変更（2026-10-31 22:12、Q1）: WS291 p023 が 'G' 46（`GPU_IMAGE_LAYOUTS`）と `GPU_CAP_IMAGE_LAYOUTS` 131072 を main で使った（Q1 の割当の誤り、WS306 の予約を見落とした）。
> WS306 は 'G' 51〜53 と `GPU_CAP_GAME` 1048576 に移す（job-submit-design の 49・50 と 262144・524288 の後）。下の表の「46 から空き」「131072 は空き」は書いた時の事実。

Parent: [WS306](ws.md)。第 1 版 2026-10-29 P5（q1666）、第 2 版 2026-10-29 P5（[design-review.md](design-review.md) の第 1 回の反映）、
**第 3 版 2026-10-31 P5（q1746-i01）**: 照合した tree を main 9946e22c0 に更新し、第 2 版の改訂の表（X1〜X10）を本文に溶かし込み、
Q306-6 の回答・WS291 の今の状態・8 つの driver・docs/ の UAPI の文書・WS291 の job の案の番号の予約を反映した。
同じ日に design-reviewer の第 2 回（Blocker 0・Major 7・Minor 13）を反映した（§18。本文は反映の後の形）。下の行番号は 9946e22c0 の物。
source は変えない（設計だけ）。ユーザーの決定は ws.md（要点は §1）。ユーザーに聞く点は [user-questions.md](user-questions.md)。

## 0. 要点

- `/dev/gpuN` の open（open file description。dup・fork で共有。以下 session）ごとに、ゲーム最適化の ON/OFF と option の組を持つ。
  option の表は driver が持ち、文字列の解読・検査は共通の層（`src/drivers/gpu/gpu-game.c`、新しい純粋な module）が一様に行う。
- 新しい ioctl 3 つ（'G' 51〜53: `GPU_GAME_INFO`・`GPU_GAME_OPTION`・`GPU_GAME_SET`）と capability の bit 1 つ（`GPU_CAP_GAME` 1048576）。
  `GPU_ABI_VERSION`（1）と既存の struct・ioctl は変えない（追加だけ）。'G' 49・50 と 262144・524288 は WS291 の job の案が予約済み（§2）。
- 値は session の最初の「凍らせる request」（許可の表に無い ioctl）の入口で、多くても 1 回だけ driver に渡して凍らせる（専用の latch。`native_submitted` を借りない）。凍った後の SET は EBUSY。
- libvulkan が環境変数 `VULKAN_ZEDBSD_GAME`（`1` で ON）と `VULKAN_ZEDBSD_GAME_OPTIONS`（「,」区切りの `+name`／`-name`）を読み、
  render の context を開いた直後（`GPU_GET_CAPSET` の後、最初の投入の前）に `GPU_GAME_SET` を呼ぶ。Vulkan の独自の拡張は作らない（ユーザーの決定）。
- GPU の名前・driver の名前・driver の版・option の一覧は、libvulkan の zedBSD の C の関数 2 つ（`zedbsd_vulkan_game_info`・
  `zedbsd_vulkan_game_option`、header `<vulkan/vulkan_zedbsd.h>`）で取る。ゲームと、patch した vulkaninfo が呼ぶ。
- Vulkan の `deviceName`・`driverVersion` は今の値のまま変えない（§8.1・§9.5）。各 driver は今の `deviceName` の文字列と新しい `gpu_name` を同じ所に定義する。
- desktop（OFF の session）には描画の最適化の option を入れない。option の既定は「描画の内容を大きく変えない物は ON」（Q306-6 の回答）。
- 最初の option は amdrdna の `ngg` と `ngg_cull`（今は boot の行 `amdrdna.vk.ngg` で node 全体）。他の driver は option 0 個で始め、識別だけを出す。
- vulkaninfo は Vulkan-Tools の tarball と patch で `userland/packages/devel/vulkan-tools/` に置く（Vulkan-Headers は別の package `devel/vulkan-headers` として build の時だけ使う）。
- HAL は変えない。`drv_gpu_ops` は kernel の内部の driver の枠組みの口で HAL ではない（§7.5）。

## 1. ユーザーの決定（要点、詳細は ws.md）

| # | 決定 | 日付 |
| --- | --- | --- |
| D1 | GPU の driver の枠組みに「ゲーム最適化」の UAPI を足し、それが ON の時だけ NGG を mesh shader 以外にも使う | 2026-10-27 |
| D2 | ON/OFF に加え、細かい option の ON/OFF を明示する文字列も渡せる。GPU の名前と driver の版も取れる。ゲームが driver を見て option を選ぶ | 2026-10-27 |
| D3 | option の一覧（名前・説明・既定）も UAPI で取れる | 2026-10-27 |
| D4 | 指定は環境変数（libvulkan が読んで UAPI を叩く）。Vulkan の独自の拡張は作らない。一覧の表示は Khronos の `vulkaninfo` に出す（移植する） | 2026-10-27 |
| D5 | WS306 は WS291 で AMD RDNA の最適化をやりきった後に、その要素を option に移して実装し直す作業 | 2026-10-27・28 |
| D6 | Q306-1〜5: 変数は `VULKAN_ZEDBSD_GAME`・`VULKAN_ZEDBSD_GAME_OPTIONS`、書式は「,」区切りの `+name`／`-name`、表に無い名前は無視して数を返す、一覧は libvulkan の C の関数、変数が無い時は OFF | 2026-10-29 |
| D7 | Q306-6: desktop（OFF）には描画の最適化の option を入れない。ゲームの既定は描画の内容が大きく変わらない物を中心に推しで決める | 2026-10-29 |
| D8 | Q306-6 の確かめ: WS291 の案 4・5（描画の内容を変えない投入の経路の改善）は既定で全部に有効（option にしない）。NGG はゲーム用に作り替える（WS291 に依存） | 2026-10-29 |
| D9 | WS306 は検討済みなので進める。決定が不明なら cockpit で聞く | 2026-10-31 |
| D10 | task・mesh shader（VK_EXT_mesh_shader）はゲーム最適化と関係なく使える GPU で常に出す（WS226） | 2026-10-27 |

## 2. 今の code の事実（main 9946e22c0）

| 物 | 事実 |
| --- | --- |
| driver | 8 つ: amdrdna・bcm2711・ghci・i915・mali・nvrtx・powervr・venus（[docs/reference/gpu-drivers.md](../../docs/reference/gpu-drivers.md) の表）。bcm2711・mali・powervr は display の node と render の node に分かれ、他は 1 node で render と display |
| session | `struct gpu_session`（`src/drivers/gpu/gpu.c` 222〜276）は open file description ごと（214 行付近の comment「shared by dup/fork until its final close」）。`writable`（228、open の mode から 1521〜1522）、`native_submitted`（232、「stop の証明が要らない」の印） |
| file の操作 | open・close・ioctl・poll・mmap だけ（1414〜1420）。ioctl は全部 `gpu_ioctl_body`（2110〜）を通る。mmap は backend の callback を呼ばない（3719〜）。poll は `gpu_events_snapshot` → `ops->display->events`（2452 付近）を呼ぶ |
| `native_submitted` を立てる所 | `gpu_command_ioctl`（3454〜、3515）、`gpu_job_admit`（5982〜、6033、`GPU_JOB_COMMIT` から）、`gpu_submit_ioctl`（6112〜、6232）。どれも `gpu_registry_lock` の下 |
| admission の外の経路 | `gpu_ioctl_body`: `GPU_JOB_CAPACITY`・`JOB_POLICY`・`JOB_RESERVE`・`JOB_COMMIT`/`CANCEL`・`COMMAND_WAIT`・`DISPLAY_EVENTS`・`DISPLAY_REFRESH`・`FENCE_*`・`COMMAND_SUBMIT_SYNC`/`DISPLAY_PRESENT_SYNC`・`DISPLAY_COMMIT`・ungated の display（2125〜2243）。残りは `gpu_session_enter`（1739〜）の admission の下の switch（2246〜2339、知らない request は 2339 で EOPNOTSUPP） |
| open の権限 | root か node の owner（1496〜1505）。read だけの open は変更の ioctl で EACCES（例 2690〜2691）。open の失敗は `gpu_session_drop`（1693〜）で session を free |
| user の pointer | `gpu_user_range`（3072〜）が address と長さを検査する（ILP32 の kernel で上位の bit を断る） |
| mutex | `include/kern/lock.h` の `mutex_init`（失敗は NULL の時だけ、`src/kern/lock.c` 207〜）・`mutex_lock`・`mutex_lock_interruptible`。壊す関数は無い。rank は実行時に検査されない |
| `GPU_GET_INFO` | `gpu_info_ioctl`（2635〜、size の検査 2649、capability を core が `ops->capabilities` で上書き 2665）。`struct gpu_info` は `driver_name[32]` だけで GPU の名前・版の欄は無い |
| `GPU_DEVICE_QUERY` | `gpu_device_query_ioctl`（5004〜）が `ops->scanout->query_device(…, session->backend)`（5031）を呼ぶ |
| `driver_name` | amdrdna `node.c` 421「amdrdna」、nvrtx `node.c` 590「nvrtx」、i915 `session.c` 270「i915」、powervr `render.c` 1051「powervr」と display、bcm2711 `render-device.c` 344「bcm2711-v3d42」（V3D 7.1 の Pi 5 でも同じ文字列）と display「bcm2711-display」、mali `render.c` 633「mali」と display、ghci `ghci.c` 827（device の名前）、venus `venus.c` 621「venus」 |
| COMMAND の bit を足す所 | executor を bind する関数: amdrdna `runtime.c` 204、nvrtx `runtime.c` 235、powervr `render-runtime.c` 97、mali `sched.c` 395、bcm2711 `render-runtime.c` 71、ghci `vk/vk-node.c` 129。i915（`I915_CAPABILITIES`）と venus（`VENUS_CAPABILITIES`）は定数。executor の無い構成（firmware が無い、self-test の失敗など）は資源だけの node として登録される |
| ops | `struct drv_gpu_ops`（`include/drivers/gpu/gpu.h` 124〜158、`DRV_GPU_INTERFACE_VERSION 9U`）。任意の表は pointer（display・share・commands・scanout・jobs・recovery）で最後に並ぶ。venus は位置で初期化（`venus.c` 357〜380）、他は designated。kernel は `-Wall -Wextra -Werror`（Makefile 1098〜1101）なので、位置の初期化で後ろの欄を書かないと `-Wmissing-field-initializers` で止まる。登録の検査 `gpu_ops_validate`（1104〜、許す bit の mask は 1127〜1134）。「No callback runs under a core spinlock」（gpu.h 108）、「a single session admits one ordinary ioctl at a time」（gpu.h 109〜111） |
| ioctl の番号 | 'G' 0〜45 を使用（抜け・重なり無し、全 header を数えた）。46 から空き。WS291 の [job-submit-design.md](../ws291/job-submit-design.md) が 'G' 49・50 と `GPU_CAP` 262144・524288 を予約し、46〜48・131072 を WS306 に残すと書いている（6・31〜32・90〜91 行） |
| capability の bit | 1〜65536 を使用（`GPU_CAP_HOST_BLOB 65536U` が最大）。131072 は空き |
| Vulkan の名前と版 | 各 driver の kernel の executor が `VkPhysicalDeviceProperties` を作る: amdrdna `vk/vk-device.c` 264〜284「zedBSD amdrdna (AMD RDNA1)」・`DEVICE_DRIVER_VERSION 1U`（53）、nvrtx `vk/vk-device.c` 362〜378「zedBSD nvrtx (NVIDIA Turing)」、powervr `vk/vk-device.c` 349〜363「zedBSD powervr (PowerVR BXE-4-32)」（`plan/tools/powervr/p009/frame-host.c` 934 が完全一致で照合）、i915 `vk/vk-instance.c` 294〜311「zedBSD i915 (Gen12 Xe)」ほか、bcm2711 `vk/vulkan-query.c` 412〜432「zedBSD V3D 4.2/7.1 (Keiland graphics)」、mali `vk/vk-device.c` 216〜227「Mali-G310 (zedBSD)」、ghci `vk/vk-device.c` 340〜360（node の名前）。driverVersion はどれも 1。venus は host の応答をそのまま通す |
| 製品名の表 | nvrtx `products.c` に製品名（例「GeForce RTX 2080 Ti」）。amdrdna `products.c` は device ID と chip（「navi10」など、出典は Linux v6.19 と pci.ids の block の comment）だけで製品名は無い |
| amdrdna の NGG | `amdrdna_vk_geometry_mode(owner)`（`vk-node.c` 437〜449）は node の session を受け、`node->vk_geometry`（boot の行 `amdrdna.vk.ngg`、149、既定 0 = legacy）を返す。読むのは 2 か所: pipeline の compile（`vk/vk-pipeline.c` 691〜695。0 以外で `primitive_shader`、2（`AMDRDNA_VK_GEOMETRY_CULLING`、`vk/amdrdna-vk.h` 203）で `primitive_culling`、primitive の番号を読む fragment の pipeline は従来の vertex のまま）と executor の quad の program（`vk/vk-execute.c` 624 `execute_programs`）。draw の state は program ごとの `primitive_shader` を見て（vk-execute.c 1802、vk-state.c 740）、pm4 の buffer は最初の draw で NGG・従来のどちらの向きにも geometry の flush を出す（vk-pm4.c 54、vk-state.c 815〜818・866〜869）ので、違う mode の session が同じ ring に混ざっても成り立つ。kernel に session をまたぐ shader の cache は無い（`vk-layout.c` 308〜339 の pipeline cache は何も持たない） |
| libvulkan の open | `vkCreateInstance`（`instance.c` 40〜）が `/dev` の GPU の node を全部開く → `instance_add_context`（704〜、`vulkan_context_open` の唯一の呼び手 721）→ `vulkan_context_open`（`context.c` 64〜）: `open(O_RDWR)`（104）→ `GPU_GET_INFO`（115）→ `GPU_GET_CAPSET` → 戻る → `instance_create_remote`（771、最初の blob・`GPU_COMMAND` 系）。display の session は別の open（`wsi-display.c` 1720、`wsi-display-nodes.c` 473）。instance を作る間に thread は作られない |
| libvulkan の物 | `VkPhysicalDevice_T` は `instance_context` を通して 1 つの context の fd に属する（`internal.h` 121〜124）。Venus では 1 つの context に複数の physical device がありうる。投入できる context は `GPU_CAP_JOB`・`JOB_CAPACITY` と strict queue・native quiescence を要る（`instance.c` 730〜740）。QEMU の Venus は対の renderer（`build/ws035-sq-venus`）の時だけ使える |
| libvulkan の版 | 公開の版は 1.0（`instance.c` 1073、`vkCreateInstance` は 1.0 以外の apiVersion を断る 64〜66）。`vkGetPhysicalDeviceProperties2KHR` はある（`api-commands.tsv` 160）。`VK_KHR_driver_properties` は無い |
| libvulkan の環境変数 | `VULKAN_ZEDBSD_SUBMIT_TIMING`（`sync.c` 27）、`VULKAN_ZEDBSD_CURSOR_LOG`（`wsi-swapchain.c` 43）、`VULKAN_ZEDBSD_OVERLAY_LOG`（`wsi-display.c` 795）。どれも診断 |
| libc の環境変数 | zedBSD の libc の `getenv`（`userland/base/libc/posix.c` 355〜）と `setenv`（409〜）は `environment_lock()`（実体 `pthread.c` 177 の `__libc_environment_lock`）の下で読み書きし、`getenv` は thread ごとの複写を返す。POSIX は保証しないが zedBSD では thread のある process でも安全 |
| libvulkan の export | `exports.map` は `global: vk*; local: *;`。`/lib/libvulkan.so`（Makefile 24） |
| Vulkan の header | `include/libc/vulkan/vulkan_core.h` は Khronos v1.3.269 から 1.0 の 137 command と WSI の 18 command を選んだ物（`API-PROVENANCE.md`）。1.1 以降の宣言は無い。WS226 が Khronos の header をそのまま使う形に替える予定（planning） |
| package | `ZEDBSD_EXTERNAL_SOURCE` は 1 つの名前に 1 つの archive（`userland/packages/external.mk` 291〜）。C++ の package の手本は harfbuzz（`ZEDBSD_EXT_harfbuzz_DEPENDS := glib freetype libcxx`、Makefile 47）。libcxx の build は AGENTS.md §7 の toolchain の扱いで Q1 の許可が要る |
| vulkaninfo | tree に無い（`grep -rli vulkaninfo userland` は 0 件） |
| 文書 | [docs/reference/gpu.md](../../docs/reference/gpu.md) ほか `gpu-*.md`（WS009 で完了）。WS306 の UAPI は無い |
| 試験の手本 | `userland/tests/gpu-admission/main.c`（1 つの fd を thread で共有する admission の guest 試験）。gpu.c の host の模型の前例 `plan/ws014/tests/gpu-framework.c`（runner は無くなっている） |

## 3. 責務と境界

| 層 | 責務 | しない事 |
| --- | --- | --- |
| UAPI（`include/uapi/gpu-game.h`） | 3 つの ioctl・struct・定数 | — |
| 共通の層の純粋な module（`src/drivers/gpu/gpu-game.c`、`include/drivers/gpu/gpu-game.h`） | option の文字列の解読（文法・長さ・名前の照合・依存の解決）、driver の option の表の登録の検査 | lock・copyin・driver の呼び出し |
| 共通の層（`gpu.c`） | ioctl の入出力の検査、session の game の状態、latch（凍らせて driver に多くても 1 回渡す）、登録の検査の呼び出し | option の意味の判断 |
| driver | 識別（GPU の名前・driver の版）、option の表（静的、device ごとに選んでよい）、凍った値を自分の判断（compile・状態）に使う | 文字列を見る、凍った後の値の変化を前提にする |
| libvulkan | 環境変数の読み、`GPU_GAME_SET`、識別と一覧の C の関数 | Vulkan の拡張・layer を作る、`deviceName` を上書きする |
| vulkaninfo（package） | 一覧と識別の表示 | 設定を変える |

範囲の外: Vulkan の拡張・layer、process の名前で driver が自動で profile を当てる仕組み（「互換をゲームの側で制御する」の意図と逆。§14）、
`VkPhysicalDeviceDriverProperties`（WS226 の Vulkan 1.2 以上の後。§9.5）、Keiland の「ゲームモード」（WS309、compositor の direct scanout。語が近いので文書で区別する）。

## 4. UAPI（`include/uapi/gpu-game.h`、新しい header、追加だけ）

```c
#include <stdint.h>
#include <uapi/ioctl.h>

#define GPU_CAP_GAME			1048576U

#define GPU_GAME_NAME_MAX		32U	/* NUL を含む */
#define GPU_GAME_DESCRIPTION_MAX	128U	/* NUL を含む */
#define GPU_GAME_OPTIONS_MAX		64U	/* driver の表の上限（mask が 64 bit） */
#define GPU_GAME_STRING_MAX		512U	/* option の文字列の上限（NUL を含まない） */

/* gpu_game_info.flags（出力） */
#define GPU_GAME_ENABLED		1U	/* この session でゲーム最適化が ON */
#define GPU_GAME_FROZEN			2U	/* 値が凍った: 以後 GPU_GAME_SET は EBUSY */

/* gpu_game_option.flags（出力） */
#define GPU_GAME_OPTION_DEFAULT		1U	/* ON の session で文字列が触れなければ有効 */

#define GPU_GAME_INFO			_IOWR('G', 51, struct gpu_game_info)
#define GPU_GAME_OPTION			_IOWR('G', 52, struct gpu_game_option)
#define GPU_GAME_SET			_IOWR('G', 53, struct gpu_game_set)

struct gpu_game_info {			/* 160 byte。入力は version と size だけ */
	uint32_t version;		/* GPU_ABI_VERSION */
	uint32_t size;
	uint32_t flags;			/* GPU_GAME_ENABLED・GPU_GAME_FROZEN */
	uint32_t option_count;		/* driver の表の行数（0〜64） */
	uint64_t active;		/* 効いている option の bit（index の順）。OFF なら 0 */
	uint32_t driver_version_number;	/* driver の版の数（§7.4）。単調に増える */
	uint32_t reserved;		/* 0 を返す */
	char gpu_name[64];		/* GPU の名前（§8.1）。分からなければ空 */
	char driver_name[32];		/* gpu_info.driver_name と同じ */
	char driver_version[32];	/* driver の版の文字列（§7.4） */
};

struct gpu_game_option {		/* 184 byte。入力は version・size・index */
	uint32_t version;
	uint32_t size;
	uint32_t index;			/* 0〜option_count-1 */
	uint32_t flags;			/* GPU_GAME_OPTION_DEFAULT */
	uint64_t requirements;		/* この option が効くのに要る他の option の bit */
	char name[GPU_GAME_NAME_MAX];
	char description[GPU_GAME_DESCRIPTION_MAX];
};

struct gpu_game_set {			/* 40 byte。入力は version・size・enable・bytes・address・reserved */
	uint32_t version;
	uint32_t size;
	uint32_t enable;		/* 0 OFF、1 ON。他は EINVAL */
	uint32_t bytes;			/* 文字列の長さ（0〜512、0 は既定のまま） */
	uint64_t address;		/* user の文字列（NUL は要らない） */
	uint64_t active;		/* 出力: 効く bit（依存を解いた後） */
	uint32_t unknown;		/* 出力: 表に無い名前と予約の書式の token の数 */
	uint32_t reserved;		/* 入力 0 */
};
```

- 大きさは x86_64 と `-m32` で 160・184・40 byte（2026-10-31 scratchpad で `sizeof`・`offsetof` を出し、第 2 回のレビューも確かめた）。全部の field が自然な境界にあり暗黙の padding が無い。
  `gpu.c` 124〜126 と同じ `_Static_assert` を 3 つに付ける。`_IOWR` の size の上限（8191）に収まる。
- field の名前は C と C++ の予約語を避ける（`requires` は C++20 の予約語なので `requirements`）。header は C++ から include できる形にする。
- 3 つとも `GPU_ABI_VERSION`（1）を version に要る。既存の struct は変えない（`gpu_info` に欄を足さないのは、size の検査で今の libvulkan が壊れるため）。

### 4.1 振る舞いと誤り

検査の順は表の上から。3 つとも admission（`gpu_session_enter`）の下の switch で処理する（INFO・OPTION は軽いので外に出す理由が無い）。

| ioctl | 条件 | 結果 |
| --- | --- | --- |
| 3 つとも | node に `GPU_CAP_GAME` が無い | EOPNOTSUPP（知らない request と同じ。今の kernel でも 'G' 51〜53 は EOPNOTSUPP） |
| 3 つとも | copyin の失敗 | EFAULT |
| 3 つとも | version・size の不一致 | EINVAL |
| INFO | — | read だけの open でも可。凍る前も後も今の値（凍る前は SET した値、SET が無ければ OFF）。`ops->game->identity` を呼ぶ |
| OPTION | `index >= option_count` | EINVAL |
| SET | read だけの open | EACCES（他の変更の ioctl と同じ） |
| SET | `reserved != 0`、`enable` が 0・1 以外 | EINVAL |
| SET | `enable = 0` | 文字列を読まない（`bytes`・`address` が何でも EINVAL にしない）。値を OFF（`active = 0`、`unknown = 0`）にして下の「凍った後」の検査へ |
| SET | `enable = 1` で `bytes > 512`、`gpu_user_range` の拒否 | EINVAL・EFAULT |
| SET | `enable = 1` で文字列の copyin の失敗 | EFAULT |
| SET | 文法の誤り（§5）・文字列の中の NUL | EINVAL、session の値は前のまま |
| SET | 凍った後（`game_lock` の下で確かめる） | EBUSY（値は変わらない） |
| SET | 成功 | 値は既定から作り直す（前の SET に積み重ねない）。`active`・`unknown` を返す。driver はまだ呼ばれない |

- 文字列の copyin と解読は `game_lock` の外（kernel の複写の上）で済ませ、lock の下では「凍っていないか」と値の書き込みだけを行う（凍らせの側を user の page fault で待たせない）。
- 入力でない field（INFO の flags 以下、OPTION の flags 以下、SET の active・unknown）は入力の値を見ない（`gpu_info` と同じ扱い）。
- `GPU_CAP_GAME` は「名前と版が取れる」の意味も持つ。option の無い driver も INFO に答える（`option_count = 0`）。

## 5. option の文字列の文法（D6）

```
string   := token ( "," token )*
token    := sign word
sign     := "+" | "-"
word     := name | reserved
name     := [a-z] [a-z0-9_]{0,30}
reserved := [a-z0-9_.:]+ で ":" か "." を含み 63 文字以下の物
```

- `+name` で有効、`-name` で無効。符号は必須（Q306-2 の答え「+name/-name」のとおり。符号の無い token は文法の誤り）。
  書かれない option は既定（`GPU_GAME_OPTION_DEFAULT`）のまま。同じ名前が 2 度出れば後が勝つ。
- 空白・空の token・末尾の「,」・`+-x`・符号の無い token・`[a-z0-9_.:]` の外の文字・NUL・32 文字以上の name は文法の誤り（EINVAL）。
  長さ 0（`bytes = 0`）は「既定のまま」で誤りではない。
- 表に無い名前は無視して数える（`unknown`）。理由: 1 つの環境変数が全 GPU に渡るので、他の driver の option の名前を含むのが普通（D6）。
- `reserved`（`:` か `.` を含む token）は今は意味を持たず、表に無い名前と同じく無視して数える。将来、driver の名前で限る形（例 `+amdrdna:ngg_cull`）を足した時に、
  その書式を使うゲームを古い kernel で動かしても文字列全体が拒否されないため。
- 依存の解決: 文字列を当てた後、`requirements` の bit が全部は効いていない option を落とす。落とした結果で別の option の依存が崩れれば、変化が無くなるまで繰り返す
  （1 回に少なくとも 1 bit 落ちるので 64 回以内に止まる）。落としたことは `active` に出る（誤りにしない）。表の検査（§7.2）で依存の循環と自分への依存を拒否する。
- 例: `VULKAN_ZEDBSD_GAME=1 VULKAN_ZEDBSD_GAME_OPTIONS=-ngg_cull`（NGG は使い culling だけ切る）。
- 手本の慣習（比較だけ、code は見ていない。記憶で未確認）: Mesa の RADV の `RADV_PERFTEST`・`RADV_DEBUG` は「,」区切りの名前の並び。

## 6. 状態遷移と latch

session の game の状態（`gpu.c` の `struct gpu_session` に足す）:

```c
	/* Game optimization: what SET asked for, and whether the backend may have seen it. */
	struct mutex game_lock;		/* SET と凍らせを順に並べる（LOCK_RANK_DEVICE） */
	unsigned game_enabled;		/* game_lock の下で書く */
	uint64_t game_active;		/* game_lock の下で書く */
	unsigned game_frozen;		/* game_lock の下で 1 にし、以後変わらない（acquire で読む） */
```

```
 OPEN（open の直後: enabled 0、active 0）
   │ GPU_GAME_SET 成功（何度でも、最後が勝つ）
   ▼
 OPEN（enabled・active を更新）
   │ 凍らせる request が来た
   ▼
 FROZEN（ops->game->apply を多くても 1 回呼んだ後）── SET は EBUSY、INFO は FROZEN を返す
   （凍らせないまま close されることもある。その時 apply は呼ばれない）
```

- **凍らせる request**: 次の許可の表に無い request は全部、`gpu_ioctl_body` の入口（session の NULL の確かめの直後、admission の分かれ道 2125 より前）で凍らせる。
  許可の表: `GPU_GET_INFO`・`GPU_GET_CAPSET`・`GPU_DEVICE_QUERY`・`GPU_GAME_INFO`・`GPU_GAME_OPTION`・`GPU_GAME_SET`。
  許可の表を「凍らせない物」の側に書くので、将来の ioctl（例 WS291 の 'G' 49 `GPU_JOB_SUBMIT`）は自動で凍らせる側に入る。
  libvulkan の順（GET_INFO → GET_CAPSET → SET → 投入）はこの表で足りる。知らない request も凍らせる（その後 EOPNOTSUPP。害は無い）。
- **凍らせの手順**（`gpu_game_freeze(session)`）:
  1. `__atomic_load_n(&session->game_frozen, __ATOMIC_ACQUIRE)` が 1 なら何もしない（凍った後の速い道。subs-N のような ioctl の多い負荷で lock を取らない）。
  2. `mutex_lock(&session->game_lock)`。もう凍っていれば unlock して戻る。
  3. `ops->game` と `apply` があり、device が online で session と device に誤りが無ければ、`ops->game->apply(private_data, backend, enabled, active)` を呼ぶ
     （OFF の時も呼ぶ。driver の入口を 1 つにする）。誤りのある session では呼ばない（その後の request は今どおり誤りで返る）。
  4. `__atomic_store_n(&session->game_frozen, 1, __ATOMIC_RELEASE)`、unlock。
- **SET**: admission の下の switch で処理し、`game_lock` を取って `game_frozen` が 0 なら値を書く、1 なら EBUSY。
- **lock の順**: admission → `game_lock`（SET）。凍らせは `game_lock` を admission の外で取り、持ったまま admission を取らない（admission は lock でなく busy の印。
  `busy_owner` の再入の道 1752〜 は凍った後の速い道を通るので影響しない）。だから 2 つの thread（SET と JOB_COMMIT・SUBMIT_SYNC・DISPLAY_COMMIT などの admission の外の経路）が並んでも、
  「session の仕事を GPU に出しうる callback より前に apply が多くても 1 回、凍った後は値が変わらない」が全部の経路で成り立つ。
  `game_lock` は spinlock でない（gpu.h 108 の「No callback runs under a core spinlock」を守る）。`apply` は眠らず、自分の leaf の lock 以外を取らない（§7.1 の契約）。
- `mutex_init` は NULL でなければ失敗しない（§2）。open で初期化し、壊す手順は無い（kernel に無い）。
- `native_submitted` と WS250 の stop の証明には触れない（意味を借りない）。
- dup・fork は同じ session を共有する: 子の SET は親が凍らせた後なら EBUSY。1 process の 2 つの VkInstance は別の open で、別の値を持てる。
- poll・mmap は ioctl でないので凍らせない。poll は `display->events` を呼びうるが、session の仕事を出す callback ではない（§7.1 の 2）。

## 7. kernel の内部の口（`include/drivers/gpu/gpu-game.h`、HAL ではない）

### 7.1 型と ops

```c
/* One option a backend offers; the table is immutable while the device is registered. */
struct drv_gpu_game_option {
	const char *name;		/* §5 の name の文法、表の中で重ならない */
	const char *description;	/* 127 文字以下、NUL 終端 */
	uint32_t flags;			/* GPU_GAME_OPTION_DEFAULT */
	uint64_t requirements;		/* 他の行の bit（自分を含まない、循環しない） */
};

struct drv_gpu_game_ops {
	int (*identity)(void *, void *, struct gpu_game_info *);
	void (*apply)(void *, void *, unsigned, uint64_t);
	const struct drv_gpu_game_option *options;
	unsigned option_count;
};
```

`apply` と `identity` の契約（header の comment に書き切る）:

1. `apply(private_data, backend_session, enabled, active)` は、session の仕事を GPU に出しうる・compile しうる callback（command・commands・jobs・
   resource と blob の作成・share・display の present 系・scanout の import）の前に、**多くても 1 回**呼ばれる。凍った後の値は変わらない。
2. `open`・`close`・`get_info`・`get_capset`・`identity`・`scanout->query_device`・`display->events`・close の時の `commands->drain`・`resource_destroy`・`recovery` は、
   `apply` の前にも、`apply` が一度も無いままでも呼ばれうる（凍らせないまま close する session がある）。
3. `apply` は admission の外で呼ばれ、同じ session の 2 の callback と並んで走りうる。driver は `apply` が書く値を、1 の callback だけが読むように作る。
4. backend の `open` は session を OFF（ゲーム最適化なし）の状態で初期化する。`apply` が来なければ OFF のまま。
5. `apply` は眠らず、core を呼び返さず、失敗しない。device が offline・session に誤りがある時は core が呼ばない。
6. `identity(private_data, backend_session, info)` は `gpu_name`・`driver_version`・`driver_version_number` を埋める（`driver_name` は core が `get_info` から写す）。GPU を待たない。

- `struct drv_gpu_ops` の最後（`recovery` の後）に `const struct drv_gpu_game_ops *game;` を足す。他の任意の表と同じ形。
  `DRV_GPU_INTERFACE_VERSION` を 9 → 10 に上げる。designated の初期化の driver は変更が要らないが、venus の位置の初期化（`venus.c` 357〜380）は
  `-Wmissing-field-initializers` で止まるので 1 行（venus の game の表）を足す（p004 で venus に identity を足すので同じ変更）。
  WS291 の job の案は `drv_gpu_job_ops`（`drv_gpu_ops` の中の別の struct）に欄を足す案で、版を上げるかは WS291 の文書に無い。先に入った方の後で、もう一方が版と venus の初期化を合わせる（Q1 が順を決める）。

### 7.2 登録の検査（`gpu_ops_validate` に足す）

- 許す bit の mask（1127〜1134）に `GPU_CAP_GAME` を足す。
- `GPU_CAP_GAME` ⇔ `ops->game != NULL` かつ `ops->game->identity != NULL`。
- `GPU_CAP_GAME` は `GPU_CAP_COMMAND` を要る（render の node だけ。display だけの node は出せない）。
- `option_count` は 0〜64。`option_count > 0` なら `options` と `apply` が要る（`option_count = 0` でも `apply` は任意）。
- 表の各行: 名前が §5 の name の文法、重なり無し、`description` が 127 文字以下で NUL 終端、`flags` は `GPU_GAME_OPTION_DEFAULT` だけ、
  `requirements` は表の中の bit だけで自分を含まない、依存の graph に循環が無い。この検査は `gpu-game.c` の純粋な関数（§7.3）。
- 誤りは今の検査と同じく EINVAL（bit の未知は EOPNOTSUPP）。driver は登録に失敗する（黙って option を落とさない）。
- **全 driver で、COMMAND の bit を足す所（§2 の 6 つの関数、i915・venus は定数）で、同時に `GPU_CAP_GAME` と `ops->game` を足す**。静的な初期化の方に置くと、
  executor の無い構成で登録の検査が EINVAL を返し、資源だけの node ごと消える。

### 7.3 純粋な module（`src/drivers/gpu/gpu-game.c`）

host で単体試験できるよう、`<stdint.h>`・`<stddef.h>` と `drivers/gpu/gpu-game.h`・`uapi/gpu-game.h` だけを include し、kernel の関数を呼ばない。

```c
/* Checks a backend's option table at registration (§7.2). 0 or EINVAL. */
int gpu_game_table_check(const struct drv_gpu_game_option *options, unsigned count);

/* The default bits of a checked table. */
uint64_t gpu_game_defaults(const struct drv_gpu_game_option *options, unsigned count);

/*
 * Applies an option string (§5) to the defaults of a checked table and
 * resolves requirements.  text need not be NUL-terminated; bytes ≤ 512.
 * On success *active and *unknown are written; on EINVAL neither is.
 */
int gpu_game_parse(const char *text, uint32_t bytes,
		   const struct drv_gpu_game_option *options, unsigned count,
		   uint64_t *active, uint32_t *unknown);
```

関数の正確な comment（入力・出力・誤り・信用できない入力）は p002 で header に書く。

### 7.4 driver の版

- 各 driver は 1 か所に版の数（例 `AMDRDNA_GAME_VERSION 1U`）を持ち、game の option の意味・既定・名前の変更、ゲームが避けたい不具合の修正のたびに上げる。
  変更の履歴は docs/ の reference（p010）に表で残す（source には日付・WS の番号を書かない）。
- `driver_version` の文字列は `"<driver_name> <数>"`（例 "amdrdna 1"）。OS の版は userland が `uname` で取れる（`include/libc/sys/utsname.h`）ので UAPI に入れない。
- libvulkan の版は info の関数が別の欄で返す（§9.3）。不具合が libvulkan にある時に区別できる。

### 7.5 HAL について

HAL の API（`include/hal/hal.h`）は変えない。この WS の口は全部、kernel の中の GPU の枠組み（`include/drivers/gpu/`）と UAPI と userland にある。
既存の driver と kernel の口（ioctl の分岐・session・登録・`kern_*` の mutex と atomic）で足りるので、HAL を変える理由が無い。
driver は HAL を include しない（Guardrail「driver は HAL を知らない」）。新しい code（`gpu-game.c`、各 driver の identity・apply）も HAL の header を include しない。

## 8. driver ごと

### 8.1 識別（全 8 driver、p005）

各 driver は、今の Vulkan の `deviceName` の文字列（変えない）と新しい `gpu_name` を **同じ所**（driver の識別の定数か関数）に定義し、
executor の `vkGetPhysicalDeviceProperties` と `identity` がそこを読む（出所を 1 つにする。文字列の見た目は変えない）。

| driver | `gpu_name`（案） | 出所 | option（初期） |
| --- | --- | --- | --- |
| amdrdna | chip と世代（例 "AMD Navi 10 (RDNA1)"） | `products.c` の chip の表と generation（既存の値、出典の comment あり）。製品名（RX 5600 XT など）は同じ device ID が複数の製品に当たる（0x731f）ので作らない | `ngg`・`ngg_cull`（§8.2） |
| nvrtx | 製品名（例 "GeForce RTX 2070"）、無ければ chip | `products.c`（既存） | 無し（WS291 の NVIDIA の段の変更は投入の経路で、D8 により常に入れる） |
| i915 | 世代（例 "Intel Gen12 Xe"） | `vk-instance.c` の今の名前と `gt.graphics_ver`・`product` | 無し |
| powervr | "PowerVR BXE-4-32" | 今の名前 | 無し |
| bcm2711 | "V3D 4.2" か "V3D 7.1"（hub の版から、`vulkan-query.c` 420〜432 と同じ判定） | 今の判定 | 無し。`driver_name` が 7.1 でも「bcm2711-v3d42」の件は §15 |
| mali | "Mali-G310" | 今の名前 | 無し |
| ghci | node の名前（"ghci:<vendor>:<device>"） | 今の名前 | 無し |
| venus | 空（host の名前は kernel が読まない） | — | 無し。Vulkan の名前と版は host の値のまま |

- `GPU_CAP_GAME` は render の node（COMMAND を持つ node）だけで、§7.2 の最後の規則のとおり COMMAND と同じ所で足す。

### 8.2 amdrdna の option（p007、WS291 の RDNA の完了の後）

| option | 既定 | requirements | 意味 |
| --- | --- | --- | --- |
| `ngg` | ON | — | mesh でない頂点の program を primitive shader（NGG）で走らせる（`options.primitive_shader`） |
| `ngg_cull` | ON | `ngg` | NGG の program で見えない primitive を落とす（`options.primitive_culling`。WS291 の compaction・属性の後回しを含む形） |

規則（第 2 版の X1 を Q306-6 の回答に合わせた物）:

- **ON の session**: option の bit だけで決める。`ngg` off → 従来の vertex、`ngg` on・`ngg_cull` off → passthrough の NGG、両方 on → culling の NGG。boot の行は見ない。
- **OFF の session**（desktop を含む）: boot の行 `amdrdna.vk.ngg` の値（既定 0 = 従来の vertex。D7「desktop には入れない」）。
  boot の行は「診断のための OFF の session の強制」として残す案を推す（→ Q306-7）。
- 実装: `apply` が `struct amdrdna_session` に `game_geometry`（0/1/2）と `game_set`（apply が来たか）を書き、`amdrdna_vk_geometry_mode(owner)` の中で
  「ON なら session の値、OFF か apply 無しなら node の boot の値」を返す。読む 2 か所（compile の `vk-pipeline.c` 691 と quad の program の `vk-execute.c` 624）が同時に従う。
  `vk-pipeline.c` だけを直さない。`open` は `game_set = 0` で始める（§7.1 の 4）。凍った後は変わらないので、compile と実行で mode が食い違わない。
- 既定を ON にする理由: NGG も culling も見えない primitive を落とすだけで、描画の内容を変えない（D7「描画の内容が大きく変わらない物は既定 ON」）。
- pipeline cache の data を将来出す時は、効いている `active` を `pipelineCacheUUID` か cache の key に入れる（今は何も持たない、§2）。p007 の phase.md の注意に書く。

### 8.3 WS291 の要素の分け方（p007 の最初に、WS291 の RDNA が終わった時の形で表を作る）

基準（D7・D8）:

| 分類 | 基準 | 扱い |
| --- | --- | --- |
| 常に入れる | 描画の内容を変えず、互換の危険が無い（投入の経路・CPU の側・memory の map・clock の制御） | option にしない。desktop も含む全部 |
| option（既定 ON） | 描画の内容は変えないが、shader の形・固定機能の使い方が変わり、driver の不具合を避けたいゲームがありうる | ON の session だけ |
| option（既定 OFF） | 描画の内容が変わりうる（精度・順序・近似） | ON の session で `+name` の時だけ |
| 作らない | session をまたぐ資源の形（export・import・scanout する画像の format・tiling・圧縮・metadata・同期）を変える、他の session の結果・隔離・回復（WS250）を弱める | — |

今の候補の見立て（WS291 の [ws.md](../ws291/ws.md)・[rdna-next.md](../ws291/rdna-next.md) から、p007 で確かめる）:

| 要素 | 見立て |
| --- | --- |
| NGG passthrough・culling・compaction・属性の後回し | `ngg`・`ngg_cull`（§8.2） |
| 非同期の submit・record の短縮・WC の BAR の map・PTE の fragment・UCLK の DPM・acquire の待ち（案 4・5 を含む） | 常に入れる（D8） |
| PA の small primitive filter・back-face cull・vertex reuse（p004、RADV の値に合わせた。driver の変更なし） | 常に入れる（今の main の挙動、描画の内容を変えない） |
| guard band・late alloc | p007 で確かめる（描画の内容を変えないなら常に入れる） |
| HTILE（p008〜p009 の予定） | depth の画像の圧縮。共有しない depth だけなら option か常に、共有しうる画像なら作らない側。p007 の時に WS291 の実装を見て決める |

- 「option にしない」と分けた要素も、WS291 の受け入れの数（scene-6500 など）を ON の session で取り直して、OFF の session が WS291 の最後の数から
  `ngg`・`ngg_cull` の分だけ落ちる事を確かめる（desktop の frame-ops は 60 fps を保つ）。

### 8.4 安全の規則（全 driver、phase の受け入れに入れる）

- option は自分の session の速さと互換だけを変える。
- session をまたいで共有する資源（allocation・image の export・import、scanout、fence）の形と意味を変えない。変える必要がある時は、その資源を共有しないと driver が判定できる時だけ効かせる。
- memory の隔離、GPU の取り消しと復帰（WS250）、他の session の結果を弱めない。
- option の表の各行に、上の確認を p007 の phase.md で 1 行ずつ書く。

## 9. libvulkan

### 9.1 環境変数（D6。coding-style §12 の「意図した設定の口」で、試験だけの切り替えではない）

| 変数 | 値 | 扱い |
| --- | --- | --- |
| `VULKAN_ZEDBSD_GAME` | `1` で ON、`0` か無しで OFF | 他の値（空・"yes"・"2"）は OFF にし、process で 1 回 stderr に 1 行 |
| `VULKAN_ZEDBSD_GAME_OPTIONS` | §5 の文字列 | OFF の時は読まない。512 byte を超えたら OFF にして stderr に 1 行 |

- 読むのは `vulkan_context_open` のたび（vkCreateInstance のたび）。探りの instance の後に変数を変えると次の instance に効く（§9.4）。
- 子の process に継がれる（ゲームが起こす helper にも効く）。session ごとなので他の process の害は無い。文書に書く。

### 9.2 `vulkan_context_open` の流れ（`context.c`）

1. 今の `GPU_GET_INFO`・`GPU_GET_CAPSET` の後、`information.capabilities` に `GPU_CAP_GAME` があり、変数が ON なら `GPU_GAME_SET`（enable 1、文字列）。
2. EINVAL（文法の誤り）: OFF のまま続け、stderr に 1 行（`libvulkan: VULKAN_ZEDBSD_GAME_OPTIONS ignored: invalid syntax`）。
   他の誤り（EBUSY は起きないはず）: OFF のまま続ける。context の open を失敗にしない。
3. 結果（`active`・`unknown`）を context に残す（info の関数が返す）。
4. `vkCreateInstance` の終わりに、ON で option の文字列があり、使える全部の context で `unknown` が token の数と同じ（どの GPU も名前を 1 つも知らない、綴りの誤りの疑い）なら stderr に 1 行。
   kernel の挙動（無視して数える、D6）は変えない。
- display の session（`wsi-display.c` 1720・`wsi-display-nodes.c` 473 の別の open）には SET しない（render の context だけ）。

### 9.3 識別と一覧の C の関数（D6 の Q306-4、`include/libc/vulkan/vulkan_zedbsd.h`）

```c
#include "vulkan_core.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZEDBSD_VULKAN_GAME_ENABLED		1U	/* GPU_GAME_ENABLED と同じ */
#define ZEDBSD_VULKAN_GAME_FROZEN		2U	/* GPU_GAME_FROZEN と同じ */
#define ZEDBSD_VULKAN_GAME_OPTION_DEFAULT	1U
#define ZEDBSD_VULKAN_GAME_OPTION_ACTIVE	2U	/* この instance で効いている */

struct zedbsd_vulkan_game_identity {	/* 352 byte（x86_64・-m32） */
	uint32_t size;			/* 呼び手が sizeof を入れる（後で欄を足すため） */
	uint32_t flags;			/* ENABLED・FROZEN */
	uint64_t active;
	uint32_t option_count;
	uint32_t unknown;		/* この instance の SET の unknown */
	uint32_t driver_version_number;
	uint32_t library_version;	/* libvulkan の版（変数の解読の版を含む） */
	char gpu_name[256];		/* kernel の gpu_name、空なら VkPhysicalDeviceProperties.deviceName */
	char driver_name[32];
	char driver_version[32];
};

struct zedbsd_vulkan_game_option_info {
	uint32_t size;
	uint32_t flags;			/* OPTION_DEFAULT・OPTION_ACTIVE */
	uint64_t requirements;
	char name[32];
	char description[128];
};

VKAPI_ATTR VkResult VKAPI_CALL zedbsd_vulkan_game_info(VkPhysicalDevice, struct zedbsd_vulkan_game_identity *);
VKAPI_ATTR VkResult VKAPI_CALL zedbsd_vulkan_game_option(VkPhysicalDevice, uint32_t index,
	struct zedbsd_vulkan_game_option_info *);

#ifdef __cplusplus
}
#endif
```

- C++ で関数の名前が型の名前を隠さないよう、struct の tag（`zedbsd_vulkan_game_identity`・`zedbsd_vulkan_game_option_info`）と関数の名前（`zedbsd_vulkan_game_info`・`zedbsd_vulkan_game_option`）を重ねない。
- 一覧は 1 個ずつ取る（`index` は 0〜`option_count`-1、範囲の外は `VK_ERROR_INITIALIZATION_FAILED`）。配列で返さないので、呼び手と libvulkan の struct の大きさが違っても歩幅がずれない。
- `size` が知っている大きさより小さい・0 なら `VK_ERROR_INITIALIZATION_FAILED`（書かない）。大きければ知っている所まで書き、残りを 0 にする。
- `GPU_CAP_GAME` の無い node（今の kernel・古い driver）では info は `VK_ERROR_FEATURE_NOT_PRESENT`、option は `VK_ERROR_FEATURE_NOT_PRESENT`。
- Vulkan の拡張ではない（`vkGetInstanceProcAddr` で引けない、`VkExtensionProperties` に出さない）。名前は `vk` の外（Khronos の名前空間を避ける）。
  `exports.map` に `zedbsd_vulkan_*;` を足す（libvulkan の ABI の追加として p006 に書く）。
- 実装は physical device の context の fd に `GPU_GAME_INFO`・`GPU_GAME_OPTION`（context の mutex の下）。Venus で 1 つの context に複数の physical device がある時は、全部が同じ session の値を返す。
- header は zedBSD の物（`SPDX-License-Identifier: Zlib` だけ）。Khronos の派生の header と同じ directory に置くので、`API-PROVENANCE.md` に「`vulkan_zedbsd.h` は zedBSD の独自の宣言で Khronos の派生でない」と 1 行足す。
  `vulkan.h` からは include しない（Khronos の header だけを期待する program に見せない）。WS226 が `include/libc/vulkan/` を替える時にこの file と行を残すよう、Q1 から WS226 に伝える（§15）。

### 9.4 ゲームの手順（文書と試験に入れる）

1. 探りの `vkCreateInstance`（変数は無し＝OFF）→ physical device ごとに `zedbsd_vulkan_game_info`・`zedbsd_vulkan_game_option` → `vkDestroyInstance`。
2. driver の名前と版を見て option を決め、`setenv("VULKAN_ZEDBSD_GAME", "1", 1)`・`setenv("VULKAN_ZEDBSD_GAME_OPTIONS", ..., 1)`。
3. 本番の `vkCreateInstance`。`zedbsd_vulkan_game_info` の `active` で効いた事を確かめる。

- zedBSD の libc は `getenv`・`setenv` を lock で守る（§2）ので、thread のある process でも 2 は安全。POSIX は保証しないので、他の OS へ移す code では
  thread を作る前に行う（文書に書く）。だから設定の口は環境変数だけにする（D4。第 2 回のレビュー M5 で前提を直し、関数での設定の問いは取り下げた）。

### 9.5 Vulkan の properties

- `deviceName`: 今の文字列のまま（§8.1）。libvulkan は今どおり通す（上書きしない）。
- `driverVersion`: 変えない（今の 1 のまま）。理由: 数の読み方は vendor ごとに違い（vulkaninfo も vendorID で読み方を変える。外部の慣習で未確認）、
  NVIDIA の vendorID のまま zedBSD の版を入れると、vendorID と driverVersion で回避策を選ぶゲームが古い NVIDIA の driver と読み違えうる。
  zedBSD の driver の版は §9.3 の関数で取る。
- `VkPhysicalDeviceDriverProperties`（`VK_KHR_driver_properties`）: `vkGetPhysicalDeviceProperties2KHR` の上で 1.0 でも出せるが、zedBSD に登録された `driverID` が無い。
  WS226（Vulkan 1.3 相当）で扱い、その時に driverName・driverInfo を §8.1 の値で埋める（WS226 への引き継ぎは Q1 が WS226 の ws.md に書く）。

## 10. vulkaninfo の移植

### 10.1 版と依存（p008 の下見で確かめる。下の物は未確認）

- 版: Vulkan-Tools と Vulkan-Headers の同じ tag。tree の header が v1.3.269 なので v1.3.269 を第一の候補にする（無ければ近い tag）。
- vulkaninfo の build は Vulkan-Headers の全部の宣言（1.1 以降の struct）を要る見込み。tree の `include/libc/vulkan/` は 1.0 だけなので、
  Vulkan-Headers を別の package `userland/packages/devel/vulkan-headers/`（`ZEDBSD_EXTERNAL_SOURCE` は 1 つの名前に 1 つの archive）として build の時だけ使う（image に入れない）。
  WS226 で tree の header が Khronos のままになったら、それに替える。
- C++（C++17 の見込み）と CMake。cross の C++ の build は harfbuzz の package で通っている。`DEPENDS` に `libcxx` が要り、**libcxx の build は AGENTS.md §7 で Q1 の許可が要る**（p008 の phase.md に書き、始める前に Q1 に確かめる）。libc++ の shared library が image にあるかも確かめる。
- CMake の option の見込み: WSI の XCB・Xlib・DirectFB を切る。Wayland は libwayland の有無で決める（無くても vulkaninfo は表示の無い形で動く見込み）。cube・mock ICD は作らない。
- loader の探し方: vulkaninfo が loader を link するか `dlopen`（`libvulkan.so.1` か `libvulkan.so`）するかを source で確かめる。zedBSD は `/lib/libvulkan.so`（soname の有無も確かめる）。合わなければ patch。
- libvulkan の制限への合わせ: instance の apiVersion は 1.0 だけ（`vkEnumerateInstanceVersion` は無い）。vulkaninfo が 1.0 の instance と 1.0 の device で動くかを見る。
- GitHub が tag から作る archive は byte が変わった事例があるという（推測・未確認）。release の asset か、tag の archive の SHA-256 の再現を下見で確かめる。

### 10.2 package の形（既存の手本 `userland/packages/libs/libjpeg-turbo/Makefile`・`libs/harfbuzz/Makefile`）

- `devel/vulkan-headers` と `devel/vulkan-tools` の 2 つの package。`ZEDBSD_EXTERNAL_SOURCE` で各 tarball（URL・size・SHA-256）を宣言し、`build/distfiles` に取得して照合する。source tree に取り込まない（AGENTS.md §7）。
- patch は `devel/vulkan-tools/patches/` に。zedBSD の時だけ（`__zedbsd__` か CMake の option）§9.3 の関数を `dlsym`（libvulkan の handle から）で引いて呼ぶ。引けない時は節を出さない。
- image に入れる物: `/bin/vulkaninfo` と `/usr/share/licenses/vulkan-tools/LICENSE.txt`（名前は tarball の実物に合わせる）。desktop の構成（libvulkan を入れる config）に入れる。

### 10.3 ライセンスの監査の手順（p008）

1. tarball を取得し SHA-256 を記録する（package の Makefile と `plan/ws306/vulkan-tools-provenance.md`）。
2. 展開した tree の `LICENSE*`・`COPYING*`・各 file の SPDX の行を一覧にする（Vulkan-Tools は Apache-2.0、Vulkan-Headers は Apache-2.0 か MIT の見込み、未確認）。
   vulkaninfo の build に入る file（`vulkaninfo/` と生成物、使う header）に Apache-2.0・MIT 以外が無いことを確かめる。
3. `plan/tools/packages/audit-licenses.sh`（GPL・LGPL の文言の機械の監査）を流し、増えた件を判定する。既知の表に足す件は Q1 に頼む（tool は WS306 の所有でない）。
4. [WS032 provenance](../ws032/provenance.md) の形で、版・入手元・SHA-256・license・patch の license（zedBSD の patch は Apache-2.0 の file への変更なので、patch も Apache-2.0 の条件で配る）を記録する。Apache-2.0 の §4（変更の印・NOTICE の継承）を満たす事を確かめる。
5. 記録は `plan/ws306/vulkan-tools-provenance.md`、image の license の file の配置は package の Makefile。

### 10.4 patch の表示（p009）

- 各 device の節に「Game Optimization」の節を足す: driver の名前・版の文字列と数、GPU の名前、今の session の ON/OFF と効いている option、option ごとに名前・既定・依存・説明。
- `--summary` には driver の名前と版、option の数と効いている数の 1 行。`--json`（出すなら）は vulkaninfo の JSON の形を壊さない別の key（p009 で形を決める）。
- vulkaninfo 自身は変数を変えない（`VULKAN_ZEDBSD_GAME=1 vulkaninfo` で ON の時の効き方も見られる）。

## 11. 失敗と回復

| 失敗 | 振る舞い |
| --- | --- |
| driver の表の不正、GAME と COMMAND の組の誤り | 登録の失敗（§7.2）。device は出ない。host の模型の試験で見つける |
| SET の誤り | session は前の値（既定は OFF）。libvulkan は OFF で続ける |
| 凍った後の SET | EBUSY。libvulkan は凍る前にしか呼ばないので起きない（起きれば OFF のまま） |
| 凍らせの時に device が offline か session に誤り | `apply` を呼ばず凍った印だけを立てる。その後の request は今どおり誤りで返る |
| `apply` の後の device の誤り・session の回復（WS250） | game の値は driver の session に残る。回復の後も同じ値。新しい session は既定から |
| option が GPU を hang させる | 普通の GPU の hang と同じ回復（WS250・各 driver）。ゲームは `-name` で避けられる（D2 の目的） |

## 12. 試験の計画

試験の image は `plan/ws306/tests/config.mk`（amd64、Venus、`userland/tests/gpu-game/` を入れる）で作る（AGENTS.md §9）。
QEMU の console・serial の log では判定しない。guest の dmesg（kernel の warning の有無）は `plan/tools/guest/serial.py` か SSH で対話して読む。

| 性質 | 確かめる試験 | 形 |
| --- | --- | --- |
| 解読と表の検査 | host の単体試験（`plan/ws306/tests/run-unit.sh`、ASan・UBSan） | 文法の受け・拒否（空の token、末尾の「,」、`+-x`、符号の無い token、NUL、空白、許されない文字、`reserved` の token が unknown、31 文字の name は受け 32 文字は拒否、`bytes` 0・512・513）、重複の後勝ち、unknown の数、依存の解決（連鎖の落ち）、64 行の境、表の検査（重複・文法・循環・自分への依存・description の長さ・未知の flag）、乱数の入力で範囲の外を読まない |
| latch: apply が多くても 1 回、仕事の callback より前、凍った後は不変、admission の外の経路と並んでも | host の模型（`plan/ws306/tests/gpu-game-model.c`: gpu.c を偽の backend とつなぐ。偽の backend は option の表を持ち、callback の順と apply の回数を記録して assert。WS014 の `gpu-framework.c` を手本にし、WS306 の tests に自前で作る） | pthread で SET と COMMAND・JOB_COMMIT・SUBMIT_SYNC・DISPLAY_COMMIT を同時に。各回は新しい open（open → 競合 → 確かめ → close を 1000 回）。凍らせないまま close した session で apply が 0 回 |
| 登録の検査 | 同じ host の模型 | GAME と COMMAND の組、`ops->game` の有無、executor の無い（COMMAND の無い）node が GAME 無しで登録される、表の誤りで EINVAL |
| UAPI の大きさ | build の `_Static_assert`（3 つ）と host の x86_64・`-m32` | 160・184・40 byte |
| UAPI の誤りと権限 | QEMU Venus（対の renderer）の guest の `userland/tests/gpu-game/`（`gpu-admission` の形） | `GPU_CAP_GAME`、INFO（option 0、driver の名前、FROZEN の前後）、OPTION の範囲外 EINVAL、SET の誤りの表（§4.1）、read だけの open で EACCES、凍った後の EBUSY、dup・fork の後の共有、2 つの open が別の値。Venus は option 0 個なので `active` は常に 0（option の効きは下の 2 行で見る） |
| 実際の kernel での競合 | 同じ guest の program（各回は新しい open、1000 回） | SET が成功して FROZEN の値が SET の値（Venus では `enabled` の 1 bit）か、SET が EBUSY のどちらか。guest の dmesg に warning が無い（serial.py・SSH で読む） |
| libvulkan の解読と SET | host の試験（ioctl を差し替えて `vulkan_context_open` の SET の部と §9.1 の扱いを流す）と QEMU の guest の Vulkan の部 | 変数なし → SET 無し、`=1` → SET、誤った文字列 → OFF と stderr の 1 行、`=yes` → OFF と 1 行、探りの instance → setenv → 本番で SET の文字列が変わる（`active` の変化は host の差し替えで見る）、display の session に SET が飛ばない、venus の `deviceName`・`driverVersion` が変わらない |
| driver の build | 全構成の vmunix（warning 0）: `config-amd64.mk`（amdrdna・i915・nvrtx・venus）、`config-rpi4.mk`・`config-rpi5.mk`（bcm2711）、`config-visionfive2.mk`（powervr）、`config-odroid-c5-keiland.mk`（mali）、`CONFIG_DRIVER_PCI_GHCI=y` の amd64（ghci、`plan/ws306/tests/` の config） | 登録の検査の型が通る |
| 識別 | 実機（Q1 の割当の範囲で）: amdrdna（10.0.10.3）、nvrtx（同じ機械で GPU を入れ替えた時）、i915（5330）、powervr（VF2）、bcm2711（Pi 4/5）、mali（ODROID-C5） | `zedbsd_vulkan_game_info` が §8.1、`deviceName` が今と同じ。割当の無い機械は未実施と書き、ユーザーが拾える試験の一覧に置く |
| amdrdna の option の効き | 実機 10.0.10.3、`plan/tools/gpu-perf/round.sh` と WS291 の測りの script | boot の行 0・1・2 × OFF・ON × `±ngg`・`±ngg_cull` の表で、compile と quad の program の mode（driver の counter と `active`）が §8.2 のとおり。ON の既定で scene-6500 が WS291 の最後の数、OFF で desktop の frame-ops 60 fps、hang の試験 |
| vulkaninfo | QEMU Venus と実機 1 台 | 下見: patch なしで動く。patch の後: 節が出る、`VULKAN_ZEDBSD_GAME=1` で効く option が出る（option の効きは amdrdna の実機）、license の file が image にある |

## 13. Phase の分け方と受け入れ（LW は ±50%、1 LW ≈ 実時間 15〜25 分）

**標準廉価実装手順の当てはめ**: [spec-module-delegation.md](../../docs/agent/spec-module-delegation.md) §1 の条件 1（外部の標準仕様に基づく）を満たす部分は WS306 に無い
（UAPI・文法・latch はどれも zedBSD の独自の物。vulkaninfo は移植で、仕様からの実装ではない）。手順は条件を満たさない時は使わない決まりなので、
WS306 では使わない。候補だった `gpu-game.c`（解読と表の検査）は、純粋で host で試験できるが数百行で、手順の段取り（骨格・基点の SHA・照合の script・別の worktree）が
実装より大きい（第 2 回のレビュー M6）。p004 の中で host の単体試験とともに作る。

| Phase | 内容 | 担当の定義 | 依存 | LW | 受け入れ |
| --- | --- | --- | --- | --- | --- |
| p002 | `include/uapi/gpu-game.h`、`include/drivers/gpu/gpu-game.h`（§7.1 の契約を comment に）、`gpu-game.c`（解読・表の検査）と host の単体試験（`plan/ws306/tests/run-unit.sh`） | phase-runner | p001 | 5 | header が単独で C と C++ で compile、`run-unit.sh` PASS（ASan・UBSan、わざと壊す確かめ）、amd64 の vmunix が warning 0 |
| p003 | 核: `gpu.c` の 3 つの ioctl・latch・`game_lock`・登録の検査、`DRV_GPU_INTERFACE_VERSION` 10、venus の identity と初期化の 1 行、host の模型（§12）、guest の `userland/tests/gpu-game/`、`plan/ws306/tests/config.mk`、QEMU | phase-runner-high | p002 | 10 | §12 の latch・登録・UAPI の誤り・実際の kernel での競合が PASS、全構成の build が warning 0 |
| p004 | 全 driver の識別（`identity`、今の `deviceName` と `gpu_name` を同じ所に、COMMAND と同じ所で GAME の bit）、各 driver の版の数 | phase-runner | p003 | 6 | 全構成の build が warning 0、host の模型の登録の試験、QEMU の venus が回帰しない、割当のある実機で §8.1（無い機械は未実施と明記） |
| p005 | libvulkan: 変数・SET・2 つの関数・`vulkan_zedbsd.h`・`exports.map`・`API-PROVENANCE.md` の 1 行、host の差し替えの試験、guest の試験の Vulkan の部 | phase-runner | p003 | 6 | §12 の libvulkan の行が PASS、`nm -D` で `zedbsd_vulkan_*` だけが増えた、header が C++20 で compile |
| p006 | vulkaninfo の下見: `devel/vulkan-headers`・`devel/vulkan-tools` の tarball・SHA-256・license の監査（§10.3）・patch なしの build と QEMU での実行。libcxx の build は始める前に Q1 の許可 | phase-runner | p002（関数は要らない） | 5 | 監査の記録、QEMU で `vulkaninfo --summary` が動く（動かない時は理由と案を Q1 へ） |
| p007 | amdrdna の option（`ngg`・`ngg_cull`、§8.3 の表を作り WS291 の要素を分ける）、実機の測り | phase-runner-high | p003、p004、WS291 の RDNA の完了（D5）、Q306-7 | 10（WS291 の後に見積もり直す） | §12 の amdrdna の行、§8.4 の各行の確認 |
| p008 | vulkaninfo の patch（§10.4）と image への組み込み | phase-runner | p005、p006 | 5 | §12 の vulkaninfo の行（option の効きの確かめは p007 の後の amdrdna の実機で追加） |
| p009 | 文書（枠組み）: `docs/reference/gpu-game.md`（新）、`gpu-requests.md`・`gpu-capabilities.md`・`gpu-drivers.md` の行、libvulkan の変数と関数、ゲームの手順（§9.4）、driver の版の履歴の表、WS309 の「ゲームモード」との区別 | document-writer | p005、p008 | 3 | docs/ から plan/ への link が無い、header と文書の値が一致 |
| p010 | 文書（amdrdna の option）: gpu-game.md の driver の option の表 | document-writer | p007、p009 | 1 | 同上 |
| p011 | 規約の全文の見直し（WS306 が変えた C） | conformance-reviewer | 全部 | 3 | coding-style の全文、出典の block の comment |

計 54 LW。p002〜p006・p008・p009 は WS291 の成果に依存しない。D5（WS291 の後に始める）を「option に移して作り直す作業」（p007）だけを指すと読み、
枠組みの Phase を先に進めるかは → Q306-9（gpu.c を同時に変える WS291 の job の案・WS250 との merge の衝突の費用を問いに書いた）。

## 14. 選ばなかった案

| 案 | 選ばなかった理由 |
| --- | --- |
| Vulkan の拡張（`VK_ZEDBSD_game_options`）、`VK_EXT_layer_settings` | D4（環境変数、独自の拡張を作らない） |
| `gpu_info` に欄を足す | size の検査（2649）で今の libvulkan・試験が壊れる。版を上げるより別の ioctl が安い |
| option の一覧を 1 回の ioctl で全部 | 64 × 184 byte が `_IOWR` の size の上限（8191）を超える |
| libvulkan の一覧を配列（enumerate の形）で返す | 呼び手と libvulkan の struct の大きさが違うと歩幅がずれる。1 個ずつ取る |
| 文字列を driver に渡して driver が解読 | 文法と誤りの扱いが driver ごとに分かれる |
| 知らない名前を EINVAL | D6。1 つの変数が複数の GPU に渡る |
| `:`・`.` を EINVAL | 将来の書式を古い kernel が丸ごと拒否する。unknown として数える |
| `native_submitted` で凍らせる | 意味（stop の証明）を借りると WS250 の変更で黙って変わる。admission の外の経路（JOB_COMMIT・SUBMIT_SYNC）で競合する |
| SET の時に driver の `apply` を呼ぶ | admission の外の経路と並ぶと compile と実行が違う値を見うる。凍らせの 1 回に寄せる |
| registry の spinlock の下で `apply` | gpu.h 108「No callback runs under a core spinlock」 |
| `deviceName` を `"zedBSD <driver> (<gpu>)"` に揃える | 利用者に見える名前が変わる（bcm2711・mali・ghci・amdrdna）。今の文字列と `gpu_name` を同じ所に置いて出所を 1 つにすれば足りる |
| `driverVersion` に zedBSD の版 | §9.5（vendor の読み方との食い違い） |
| amdrdna の製品名の表 | device ID が製品に 1 対 1 でない。chip の名前で足りる（driver の不具合は chip と版で決まる） |
| 関数で option を設定する口 | zedBSD の libc で setenv は thread の中でも安全（§9.4）。D4 のとおり環境変数だけ |
| 標準廉価実装手順で `gpu-game.c` を委ねる | 手順の条件 1 を満たさない（§13） |
| process の名前で自動の profile | 「互換をゲームの側で制御」の意図と逆 |
| boot の行だけで option | session ごとに選べない |
| `libvulkan` が `deviceName` を上書き | 出所が 2 つになる（executor と UAPI）。venus で host の名前を壊す |

## 15. 危険と未決

- WS291 の RDNA は止まっている（RX 5600 XT が抜かれている、2026-10-30）。p007 の始まりはその後。WS291 が既定を変える（例 `ngg=2` を既定にする）と §8.2 の OFF の規則の前提が変わる → p007 の最初に main の既定を確かめる。
- gpu.c を WS291 の job の案（`gpu_ioctl_body` の分岐と `drv_gpu_job_ops`）と WS250（gpu.c の監視、incomplete）も変える。p003 の merge で競合しうる（§7.1、Q306-9）。
- bcm2711 の render の `driver_name` が V3D 7.1 でも「bcm2711-v3d42」。WS306 は `driver_name` を変えない（既存の UAPI の値。変えるなら別の判断）。`gpu_name` で区別できる。
- vulkaninfo の依存（C++・libcxx・header・loader の探し方・WSI）は未確認。p006 の下見で合わなければ Q1 に返す。
- WS226 が `include/libc/vulkan/` を Khronos の header に替える時、`vulkan_zedbsd.h` と `API-PROVENANCE.md` の行を残す必要がある。`VkPhysicalDeviceDriverProperties` の引き継ぎと合わせ、WS226 の ws.md への記録を Q1 に頼む。
- 環境変数は子の process に継がれる。compositor・helper にも効くが、各 process の session だけに効く。
- option の数が 64 を超える driver は mask の形を変える要がある（見込みは数個）。

## 16. 出典と引用

- この設計は zedBSD の code（main 9946e22c0）と zedBSD の文書だけを読んで書いた。外部の code・文書の写しは無い。
- §5 の RADV の慣習、§9.5 の vendor ごとの `driverVersion` の読み方、§10.1 の vulkaninfo の build の見込みは記憶による比較で、未確認と書いた。実装の段で使う時は出典（版・path）を確かめ、
  source に入れる値・手順は Guardrail の「Value(s) referenced:」「Procedure referenced:」の block の comment で示す（file の先頭に書かない）。
- Vulkan-Tools・Vulkan-Headers は source tree に取り込まず、tarball と patch（§10）。license の監査は §10.3。
- source の comment に WS・Phase の番号・plan/ への参照を書かない（Guardrail）。

## 17. 第 1 回のレビュー（2026-10-29）の反映

| 指摘 | 反映した所 |
| --- | --- |
| B1 `ngg_cull` と boot の行の混ざり | §8.2（`ngg`・`ngg_cull` に分け、ON は option だけ、OFF は boot の行）、§12 の表の試験、読む 2 か所（第 2 回の m2） |
| M1 latch | §6（専用の latch、許可の表、`game_lock`、lock の順）、§7.1 の契約（第 2 回の M1） |
| M2 名前と版の写し | §8.1・§9.5（今の名前と版を保ち、`gpu_name` と同じ所に定義、libvulkan は上書きしない、venus は host の値） |
| M3 ゲームの手順と関数 | §9.3・§9.4（2 つの関数、`vk` の外の名前、`exports.map`、`size` の欄、header の license） |
| M4 共有の資源 | §8.4 |
| M5 WS291 の要素と OFF の速さ | §8.3（Q306-6 の回答 D7・D8 で基準が決まった） |
| M6 版の上げ方 | §7.4 |
| M7 文書の依存 | §13 p009・p010（WS009 は完了、docs/reference/gpu*.md がある） |
| M8 試験 | §12 |
| m1〜m13 | m1 §7.1・§7.2、m2 §4.1、m3 §8.1、m4 §4.1（SET は writable）、m5 §5（`:`・`.` の予約、依存の解決）、m6 §9.1・§9.2、m7 §9.5、m8 §13（venus の識別は p003）、m9 §10.1・p006、m10 §8.1（製品名を作らない）、m11 §8.2 の最後、m12 §0・ws.md の目標、m13 冒頭（照合の SHA） |

## 18. 第 2 回のレビュー（第 3 版、2026-10-31）の反映

指摘の全文は [design-review.md](design-review.md) の第 2 回。全部を反映した（見送り 0）。

| 指摘 | 反映 |
| --- | --- |
| M1 `apply` の契約が実際の callback の順と合わない | §7.1 の契約 1〜6（多くても 1 回、前・無しに呼ばれうる callback の列挙、並行、open は OFF で初期化、offline・誤りでは呼ばない）、§6 の手順 3、§8.2 の `game_set` |
| M2 試験が latch と option の核心を確かめられない | §12 を性質ごとの表に: host の模型（偽の backend、順と回数の assert）、各回新しい open の競合、libvulkan の host の差し替え、option の効きは amdrdna の実機、dmesg は対話で読む |
| M3 `requires` が C++20 の予約語、C++ の header | `requirements` に改名（§4・§7.1・§9.3）、`extern "C"`・`VKAPI_ATTR`、flag の値を header に、struct と関数の名前の分け方 |
| M4 配列の歩幅 | 1 個ずつ取る `zedbsd_vulkan_game_option(physical, index, *out)`（§9.3） |
| M5 setenv の前提の誤り | §2 の libc の行、§9.4 を直し、Q306-8 を取り下げた（D4 のとおり環境変数だけ） |
| M6 標準廉価実装手順を条件の外で使う案 | 推しを取り下げ、`gpu-game.c` は p002 で普通の Phase（§13）。Phase を 1 つ減らした |
| M7 GAME の bit を足す所 | §2 の COMMAND の行、§7.2 の最後の規則、§8.1、§12 の登録の試験 |
| m1 venus の初期化 | §2 の ops の行、§7.1（venus.c に 1 行要る） |
| m2 NGG を読む 2 か所 | §2、§8.2 |
| m3 mutex | §2・§6（壊す手順は無い、失敗は NULL の時だけ、rank、copyin と解読は lock の外） |
| m4 INFO・OPTION の置き場所、`gpu_user_range` | §4.1 |
| m5 §4.1・§5 の細目 | §4.1 の検査の順、§5（依存は不動点まで、符号は必須、31・32 の境、`reserved` は unknown） |
| m6 Phase の依存と package | §13（libvulkan は p003 だけに依存、文書を 2 つに、libcxx の許可）、§10（vulkan-headers を別の package） |
| m7 `deviceName` の変更 | 名前を変えない（§8.1・§9.5・§14） |
| m8 Q306-9 の背景、版の上げの文 | user-questions.md の Q306-9、§7.1、§15 |
| m9 試験の image | §12 の冒頭（`plan/ws306/tests/config.mk`） |
| m10 記録の不整合 | ws.md の Phase の表と p001 の行、目標の語、`phase001/phase.md` を作った、§8.2 の pipeline cache の注意。ws.md の既存の原文は Q1 の指示（2026-10-31）で残す |
| m11 誤りのある session の凍らせ | §6 の手順 3、§11 |
| m12 build の一覧 | §12 の driver の build の行 |
| m13 WS226 との境 | §9.3・§9.5・§15（WS226 の ws.md への記録は Q1 に頼む） |
