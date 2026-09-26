<!-- awesome-plan project=zedbsd record=ws035p067 -->

# ws035-p067: zdesktop で mview（Vulkan の client）の窓を i915 で合成する

Phase ID: `ws035-p067`
Parent: [WS035](../ws.md)
Status: cleared（q466-i01、2026-09-26。i915 実機の GPU（VFIO、capture）で mview の窓を合成）
Phase disposition: normal
Queue: q466-i01
承認: 2026-09-26 ユーザー「今回のzdesktopでmviewを動かしたいです。i915でです。」（その前に「i915でmviewが動いた証跡がないようですね？」）

## 対象

p066 と同じ輪: 5330（10.0.30.3）の iGPU を QEMU へ VFIO で渡し、capture 表示で判定する（`plan/ws031/tests/vkloop-hw.sh zdesktop`）。
Future Work の F-022（i915 で GPU の client の窓を合成）をこの Phase で扱う。

## 範囲

1. mview を zdesktop の中で動かすと device lost になる原因を切り分ける（guest の file log を `ufs-cat.py` で読む。serial は読まない）。
2. 疎通に要る範囲で、i915 の実行器（`src/drivers/gpu/i915/render`）と zwl・libvulkan を直す。
3. 実機の capture で mview の窓（Wiseman Mode の浮いたタイトルバーの下の 3D の絵）を確かめる。

HAL（`include/hal/hal.h`）は変えない。

## 受け入れ

1. 5330 の i915 で、zwl --glass の画面に mview の窓が描かれる（capture の画面）。
2. host の fixture（`plan/ws031/tests/run-vk-host-tests.sh`）が PASS、build は warning 0、変えた C の style-check 0。
3. Venus の回帰（p052・p059・p062）が通る。boot test PASS。
4. 実機の LCD・ベアメタルは範囲外（未実施と書く）。

## 経過

- 2026-09-26: guest の log（`/var/log/zwl.log`・`mview.log`）で、mview の最初の描画の直後に zwl の copy が
  `GPU copy 1920x1080 -> 1920x1080 at (0,0) of a 1024x1024 surface` で失敗し、zwl の submit が device lost、mview も errno 44。
  原因: 実行器の object 表が device に 1 つで、key が（種類、wire の id）だけ。wire の id は process ごとに 1 から数えるので、
  zwl と mview の object が同じ key で上書きし合う。blob の storage の対応（`drv_i915_render_blob_attach`）も同じ。
- 直し:
  - object 表の key に session を足す（`drv_i915_object_insert/lookup/remove` の第 1 引数を executor の session に。
    session の close で `drv_i915_object_forget`）。
  - allocation に node の open（`struct i915_session`）を持たせ、blob は同じ open の allocation だけの storage にする。
    close で session の残した allocation を解放する（`drv_i915_gfx_memory_forget`）。
  - allocation の import（`VkImportMemoryResourceInfoMESA`）: import した resource の object を storage にする。
- host の fixture PASS（object 表の 2 session の試験を追加）。
- 実機 run11: device lost は消えたが、zwl の import が `VK_ERROR_INVALID_EXTERNAL_HANDLE`。libvulkan の `memory_import_fd` が
  `GPU_CAP_ALLOCATION_SHARE` の無い node で即座に断り、画像の import（p066 で足した道）に届いていなかった。
  → allocation の共有が無い node では、fd を画像の capability として `memory_import_image_fd` へ。

## 結果（q466-i01、2026-09-26、5330 の i915 を QEMU へ VFIO で渡した実機の GPU、capture 表示。LCD・ベアメタルは未実施）

`CAPTURE=zdesktop I915_HOST=awe@10.0.30.3 plan/ws031/tests/vkloop-hw.sh zdesktop`（run12）が PASS（capture の 4 検査: desktop が描かれる、
ダブルクリックでドッキング、下端から Wiseview、背景の click で閉じる）。画面は `build/ws035-p067/run12/`（`desktop`・`docked`・`wiseview`・`closed` の png と `sheet.png`）:

- desktop: 2 つの wl_shm の窓の上に「Model viewer」の窓。浮いたタイトルバーの下に mview の 3D model（qs40、テクスチャ付き）が描かれる。
- docked: 一番上の mview がバーにドッキングし（バーに「Model viewer」と `— ⧉ ×`）、1920x1042 で描き直した model が画面を埋める
  （guest の log: zwl が 1920x1042 の buffer を 3 つ import）。
- wiseview: 3 つの窓の縮小に mview の model が見える。

guest の log（guest が `/var/log` に書いた file を `ufs-cat.py` で読んだ）: zwl が mview の 800x560 の buffer 3 つと 1920x1042 の 3 つを import、
Vulkan・import の ERROR 無し、device lost 無し。

受け入れ:

1. mview の窓が i915 の zwl --glass に描かれる: 達成（上の画面）。
2. host の fixture（`run-vk-host-tests.sh`）PASS。build は warning 0（Venus の image、i915 の image、`I915_TESTS=y` の kernel の compile）。
   変えた C の style-check は HEAD と同数（新しい指摘 0）。
3. Venus の回帰 p052・p059・p062 PASS（`build/ws035-p067/venus-p0NN.log`）。boot test PASS（`build/ws035-p067-boot/login.png`、QEMU、
   i915 の image。i915 が無いので zwl は終了するが login に届く）。
4. 実機の LCD・ベアメタル: 未実施。kernel の試験 scenario（`vke1`・`vke2` 等）の実機の実行: 未実施（compile のみ。判定が serial の log のため）。

制限・残り:

- session の close で、application が破棄しなかった allocation は解放するが、他の object（buffer・image・pipeline 等）は表から外すだけで
  解放しない（XXX、以前からの漏れ）。
- 実行器の不足（F-023）はそのまま。zwl は p066 の回避のまま。
