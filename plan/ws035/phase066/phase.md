<!-- awesome-plan project=zedbsd record=ws035p066 -->

# ws035-p066: zdesktop（Wiseman Mode）を Intel GPU（i915 のネイティブ実行器）で動かす疎通確認

Phase ID: `ws035-p066`
Parent: [WS035](../ws.md)
Status: cleared（q465-i01、2026-09-26。wl_shm の窓で。Vulkan の client の窓は F-022）
Phase disposition: normal
Queue: q465-i01
承認: 2026-09-26 ユーザー「10.0.30.2に Intel Iris Xe (Core i5 1245U)のマシンがあるので、ここでIntel GPUでzdesktopが実行できるように疎通確認的に作業してください。」「10.0.30.3でした」

## 対象

Dell Latitude 5330（hostname `chaos`、i5-1245U、iGPU `8086:46a8`）。以前の 10.0.10.25 から 10.0.30.3 へ移った同じ機械（WS031 の試験機）。
WS031 の実機の輪（`plan/ws031/tests/vkloop-hw.sh`）: centris で image を作り、5330 の QEMU に iGPU を VFIO で渡して起動し、
capture 表示（guest RAM へ写した画面を QMP で読む）で判定する。LCD とカメラは使わない。

## 範囲

1. zwl の shader を i915 のネイティブの compiler（`src/drivers/gpu/i915/compiler`）が通す形に（標準の Vulkan のまま、Venus でも同じに動く）:
   `gl_VertexIndex` → 角の vertex buffer、`gl_FragCoord` → vertex shader からの画素位置の varying、
   整数の mode の比較の連鎖（OpSwitch になる）と早い return（merge-return が OpSwitch を作る）→ float の範囲比較と 1 つの出口、
   成分の代入（OpCompositeInsert）→ vector の組み直し。host の `i915-vk-eudump` で 4 本とも compile を確認。
2. 実機の輪に `zdesktop` の scenario: `plan/ws031/tests/zdesktop/`（zwl --glass 1920x1080 と wallpaper、wltest・wlshm・mview の窓）、
   `plan/ws031/tests/config-zdesktop-hw.mk`、`i915-capture.py zdesktop`（desktop・ダブルクリックでドッキング・下端から Wiseview・閉じる の 4 画面）。
   `I915_HOST` で 5330 の名を変えられるように。wlshm に `--display=`。
3. 実機で起動し、画面で確かめる。動かない所は原因を切り分け、疎通の範囲で直す。

判定は capture の画面で行う（AGENTS.md: serial log で判定しない。serial は capture 領域の base と viewer の開始の同期にだけ使う）。

## 受け入れ

1. 5330 の i915（VFIO）で zwl --glass の画面（壁紙・浮いたタイトルバー・窓）が capture に出る。ドッキングと Wiseview の画面が変わる。
2. Venus の回帰（p052・p059・p062・p063）が通る。build は warning 0、変えた C の style-check 0。
3. 実機の LCD・ベアメタルは範囲外（未実施と書く）。

## 結果（q465-i01、2026-09-26、5330 の i915 を QEMU へ VFIO で渡した実機の GPU、capture 表示。LCD・ベアメタルは未実施）

`CAPTURE=zdesktop I915_HOST=awe@10.0.30.3 plan/ws031/tests/vkloop-hw.sh zdesktop` が PASS（capture の画面での 4 検査: desktop が描かれる、ダブルクリックでドッキング、下端から Wiseview、背景の click で閉じる）。
`build/ws035-p066/run10/`（`desktop`・`docked`・`wiseview`・`closed` と `sheet.png`）: 1920x1080 の壁紙（ユーザーの絵）、すりガラスの上部のバーと文字（Inter）、3 つの wl_shm の窓の浮いたタイトルバーと cascade、一番上の窓のドッキング（バーに題名と `— ⧉ ×`）、Wiseview の 3 つのタイトル。

直したもの（順に見つかった順）:

| 症状（画面の読み取り: VGA の screendump と capture） | 直し |
| --- | --- |
| build が kernel の include の検査で失敗（`I915_TEST_VBT=y` の VBT の表が `vendor/intel-vbt/`） | `check-kernel-includes.noct` に `vendor/intel-vbt/` を許す（試験 build だけが include する data。製品の kernel は含まない。今日の検査の追加で WS031 の実機の輪全体が壊れていた） |
| zwl の shader が native compiler を通らない | `gl_VertexIndex` → 角の vertex buffer、`gl_FragCoord` → 画素位置の varying、OpSwitch になる整数の比較と早い return → float の範囲比較と 1 つの出口、OpCompositeInsert → vector の組み直し（host の `i915-vk-eudump` で 4 本を確認） |
| `ZWL VULKAN_ERROR operation=device result=-7`（拡張が無い） | i915 は `GPU_CAP_SHARE`（画像の共有）だけで allocation の共有と kernel の fence が無い。libvulkan は `GPU_CAP_SHARE` でも外部 memory（画像の import だけ）を出し、allocation の import が `EOPNOTSUPP` なら画像の import へ。zwl は外部 fence の拡張が無ければ fence を fd にせず、event loop の各回で `vkGetFenceStatus`（その間の poll は 2 ms） |
| swapchain で `intel_context_create failed rc=8`（ENOSPC） | GT の GGTT の窓（1 MiB）が context 5 つ程で尽きた。ユーザーの指示（2026-09-26「Maxまでマップできるのがいいです…このOSはクライアントOSなので」）で GT の窓 64 MiB、display の窓 1 GiB（以前 32 MiB = full HD 2 枚）に。GGTT は Gen12 で 4 GiB |
| `unimplemented path: primitive topology 4` | 四角を triangle strip から triangle list（6 頂点）に |
| client が去ると `opcode 78`（vkFreeDescriptorSets）が未移植で device lost | zwl は descriptor set を個別に解放せず、予備の列に戻して次の画像で使う |
| Vulkan の client（mview・wltest）の窓を合成すると別の surface へ copy して device lost | 実行器が context の間の共有を持たない（F-022）。この Phase は wl_shm の窓で確かめた |

試験の道具: `vkloop-hw.sh` に `zdesktop` と `I915_HOST`（5330 の ssh の名。`~/.ssh/config` の `solaris10-man` は旧 IP のまま、変えていない）、`i915-capture.py zdesktop`（capture 領域を serial log でなく guest の memory の `I915CAP1` の走査で見つける。判定は画面だけ）、`plan/ws031/tests/zdesktop/`（zwl と 3 つの 800x560 の wl_shm の窓。同じ大きさなので一番上の窓の位置が決まる）、`config-zdesktop-hw.mk`、wlshm に `--display=`。

回帰（Venus）: p052・p053・p054・p059・p062・p063 PASS。build は warning 0（外部 package の既存を除く）、変えた C の style-check 0。boot test PASS（`build/ws035-p066-boot/login.png`、QEMU、i915 無しのため zwl は終了するが login に届く）。

VBT（2026-09-26 ユーザーの確認）: VBT は Dell の firmware の表で、実機では OS が起動時に OpRegion から読む。QEMU の passthrough では guest に OpRegion が見えないため、試験 build（`I915_TEST_VBT=y`）だけが 5330 から写した表を持つ。製品の kernel は含まない。
