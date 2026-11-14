<!-- awesome-plan project=zedbsd record=ws217-p009c -->

# ws217-p009c: libvulkan の display の transform 180° と vkdemo `--transform=180`

Parent: [WS217](../ws.md)
Status: in-progress（実装は branch `agent/p2-ws217` の `fb1b270d27` だけ、main に無い。試験は build だけ。2026-11-15 P5 q2110-i01: p009a だけを main に載せ直し、この段は載せていない。今の main への載せ直しの見込みは §5。cleared にしない）
Disposition: normal
Primary Milestone: MG006（WS から継承）
Queue / attempts: q1680-i01（P2、worktree `/home/awe/zedBSD-worktrees/p2-ws217`、branch `agent/p2-ws217`、p009a・p009b の上）。
Q217-2 の順（WS309 の WSI の段との順）は 2026-10-29 Q1「進めて良い。P1 は今 WS309 p011（compositor の手書き）で libvulkan の WSI は触っていない（WS309 の p015・V2 swapchain が後で WSI を触るので、変更は小さく 1 commit に）」。
Purpose / goal: p001 第 4 版 §5.3 と design-v5 §3・A8・A11（m5）。display の `supportedTransforms` に 180、primary の surface の `transform = ROTATE_180` を受け、その surface の present と commit に `ROTATE_180` の flag を付ける。surface の capabilities と swapchain は IDENTITY のまま（S11）。

## 1. 実装（libvulkan は 1 commit）

| file | 内容 |
| --- | --- |
| `userland/desktop/libvulkan/wsi-internal.h` | `VULKAN_WSI_OUTPUT_ROTATE_180 64U` |
| `userland/desktop/libvulkan/wsi-display.c` | query の写し: `GPU_DISPLAY_ROTATE_180` → `VULKAN_WSI_OUTPUT_ROTATE_180`。present の blob（`GPU_DISPLAY_PRESENT_FIFO \| BLOB`）と copied（`FIFO`）の 2 か所、commit の primary（`request.planes[0]`、`display_overlay_state` の中ではなく呼び手で、L28）に、`lease->surface->display_info.transform == ROTATE_180` なら `ROTATE_180`。cursor・overlay の present は元から primary の present に来ない |
| `userland/desktop/libvulkan/wsi.c` | `vkGetPhysicalDeviceDisplayPropertiesKHR` の `supportedTransforms` に、出力の印があれば `ROTATE_180`。`vkCreateDisplayPlaneSurfaceKHR` は IDENTITY か、印のある出力の primary（cursor でも overlay でもない）の `ROTATE_180` を受ける。他は `VK_ERROR_INITIALIZATION_FAILED` のまま |
| `userland/desktop/libvulkan/README.md` | 節「Scanout の 180° の回転」: surface の座標は scanout が回す前の画像の座標、capabilities は IDENTITY、`preTransform = IDENTITY` |
| `userland/tests/vkdemo/display.c`・`display.h` | `vkdemo_display_open_transform`・`vkdemo_display_open_on_transform`（`transform` を `supportedTransforms` で選び surface に渡す）。今の `vkdemo_display_open`・`_open_on` はそれを IDENTITY で呼ぶ（compositor の `compose.c`・`heads.c` は変えない、M25） |
| `userland/tests/vkdemo/renderer.c`・`renderer.h`・`main.c` | `vkdemo_initialize(..., rotate180)`、option `--transform=0\|180`（`--offscreen` とは組めない） |

変えていない: `wsi-display.c` の surface の capabilities（`supportedTransforms = currentTransform = IDENTITY`）、`wsi-swapchain.c` の `preTransform != IDENTITY` の断り、libvulkan-compat（Linux・FreeBSD）。

## 2. 確かめた事（2026-10-29、build だけ）

| 確かめ | コマンド | 結果 |
| --- | --- | --- |
| amd64 | `make ZEDBSD_CONFIG=config/ci/config-amd64.mk BUILD=build/p009c build/p009c/dynamic/libvulkan.so build/p009c/bin/vkdemo build/p009c/bin/wayland` | exit 0、warning 0 |
| arm64（rpi4）・riscv64（visionfive2）・Linux の Keiland | §2.1 | §2.1 |
| `git diff --check` | 変えた file | 出力なし |

### 2.1 他の platform の build

| build | コマンド | 結果 |
| --- | --- | --- |
| arm64（rpi4）の libvulkan と compositor | `make ZEDBSD_CONFIG=config/ci/config-rpi4.mk BUILD=build/p009c-rpi4 build/p009c-rpi4/dynamic/libvulkan.so build/p009c-rpi4/bin/wayland` | 1 回目は worktree の新しい arm64 の sysroot の生成と libc の object の `-j` の競合で `errno.h` が無い（変更と無関係）。sysroot ができた後の 2 回目は exit 0、warning 0 |
| riscv64（visionfive2）の libvulkan と vkdemo | `make ZEDBSD_CONFIG=config/ci/config-visionfive2.mk BUILD=build/p009c-vf2 build/p009c-vf2/dynamic/libvulkan.so build/p009c-vf2/bin/vkdemo` | exit 0、warning 0 |
| Linux の Keiland（compositor が vkdemo の `display.c` を含む） | `make keiland-linux KEILAND_ASK=n KEILAND_LINUX_BUILD=build/p009c-linux` | exit 0、warning 0（install はしていない） |
| FreeBSD の Keiland | `make keiland-freebsd KEILAND_ASK=n KEILAND_FREEBSD_BUILD=build/p009c-freebsd` | 未実施と同じ: この host に FreeBSD の `/usr/local/include/libdrm` が無く、header の段で止まる（FreeBSD の guest が要る） |
| amd64 vmunix（p009b の最後の直しの後） | `make ZEDBSD_CONFIG=config/ci/config-amd64.mk BUILD=build/p009a build/p009a/vmunix` | exit 0、warning 0、`amd64 vmunix check: PASS` |

## 3. 未実施

- libvulkan の host 試験: 作っていない。既存の fixture（`plan/ws214/tests/vulkan-overlay-host.c`）は他の WS の物で、present の flags を記録しない。推し: p007 の実機（5330 で vkdemo `--transform=180` の間の PLANE_CTL）で見る。要るなら次の Phase で `plan/ws217/tests/` に fixture を作る（Q1 の判断）。
- venus-reg（`plan/ws291/tests/venus-reg.sh`、QEMU の Venus guest）: この Queue は QEMU を使わない約束なので流していない。venus の driver は `GPU_DISPLAY_ROTATE_180` を出さないので、libvulkan の変わる所は通らない（読みの判断）。merge の前に Q1 が QEMU を割り当てて流す。
- 実機（5330・SV7・Pi 4）での vkdemo `--transform=180`。

## 4. 残り

compositor の段（p010 以降）は WS309 の後（WS309 は 2026-11-15 に completed）。

## 5. 今の main への載せ直しの見込み（2026-11-15 P5、q2110-i01）

前提の変化: Q217-2（WS309 の WSI の段との順）は、WS309 が 2026-11-15 に completed になったので待つ物が無い。WS353（compositor・libvulkan・kernel の待ち方の作り直し）も main に入っている。この段（`fb1b270d27`）は p009a（main では agent/p5-ws217 の `ca9f3d491e`）の上に積む。p009b（i915）とは file が重ならないので、p009b を待たずに載せられる（ただし印を立てる driver が p009b の i915 だけなので、180 の経路を動かして確かめられるのは p009b の後の実機）。

衝突の見込み（`git merge-tree --write-tree --merge-base=fb1b270d27^ <p009a を載せた HEAD> fb1b270d27` で確かめた）:
- `plan/ws217/ws.md`: 文の衝突（今の ws.md を正として行を足す）。
- `userland/desktop/libvulkan/README.md`: main で 2026-11-07（`7bf6a9255a`）に消えた（modify/delete）。§1 の「Scanout の 180° の回転」の節は、今の libvulkan の説明の置き場（`docs/architecture/graphics-and-wayland.md` など、VK_KHR_display を書く文書）を探してそこへ移す。README を戻さない。
- `wsi.c`（+141 行）・`wsi-display.c`（+55）・`wsi-internal.h`（+77）・vkdemo の `display.c`・`display.h` は基点から変わったが自動で当たる。`VULKAN_WSI_OUTPUT_ROTATE_180 64U` は今の `VULKAN_WSI_OUTPUT_*`（1〜32）と重ならない。
- 意味の上で見る所: (1) `display_present_image_sync` の `GPU_DISPLAY_PRESENT_SYNC` の経路: 今の main では flags を立てた後に `synchronized.present = request` で写す（`wsi-display.c` の 1431 行で flags、1467 行で写し、2026-11-15 に読んだ）ので、branch の版のとおり flags を立てた直後に `ROTATE_180` を足せば SYNC にも届く。(2) 今の main で present・commit の経路が増えていないか（WS353 の後の `GPU_DISPLAY_PRESENT` と `GPU_DISPLAY_COMMIT` の呼びは、`display_present_image_sync`・`display_present_native`・`display_primary_commit` と overlay・cursor の物。primary の物だけに付ける）。(3) vkdemo の `display.c` の `open_on` を compositor（`compose.c`・`heads.c`）が含むので、Linux・FreeBSD の Keiland の build（`make keiland-linux …`）も通す。

手順:
1. p009a が main に入った後、新しい worktree で `git cherry-pick fb1b270d27`。README は消したまま（`git rm` の要否は Q1 に一覧を送る）、節を今の文書へ移す。
2. (1)〜(3) を読み、`make -j4 CONFIGMK=config/config.<x86_64|rpi5|vf2>.mk sysroot` の後に libvulkan・vkdemo・wayland の個別の target（warning 0）。
3. QEMU の Venus guest の回帰（`plan/ws353/tests/qemu-render-wait.sh` と、libvulkan の display の経路を通る `plan/ws291/tests/venus-reg.sh`）。venus は `GPU_DISPLAY_ROTATE_180` を出さないので、変わる所は通らず、既存の経路が壊れていない事の確かめになる。
4. 180 の経路そのもの（vkdemo `--transform=180` の間の PLANE_CTL）は p009b の後の 5330 で。
