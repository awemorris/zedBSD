<!-- awesome-plan project=zedbsd record=ws031p050 -->

# ws031-p050: session が残した Vulkan の object を close で解放する

Phase ID: `ws031-p050`
Parent: [WS031](../ws.md)
Status: cleared（q467-i01、2026-09-26）
Phase disposition: normal
Queue: q467-i01
承認: 2026-09-26 ユーザー「残っている解放漏れを対処してください。」

## 背景

ws035-p067 で実行器の object 表を session ごとの key にし、close で表から session の entry を外すようにした。
そのとき application が破棄しなかった allocation は解放するが、他の object（buffer・image・view・sampler・pipeline・
command pool と buffer・fence・semaphore・descriptor の layout・pool・set・render pass・framebuffer 等）は表から外すだけで
解放していなかった（XXX）。また descriptor pool の破棄はその pool の set を解放しない（vkDestroyDescriptorPool は set を暗黙に解放する）。

## 範囲

1. `render/object.c` に session の object を 1 つずつ取り出す関数（種類と条件で）。
2. executor の session の close で、残った object を種類ごとの解放の仕方で解放する（command pool はその buffer と、
   pipeline は kernel の解放と、fence は `drv_i915_fence_free`、allocation は list から外して、その他は単一の確保）。
   instance・physical device・device・queue は token なので表から外すだけ。
3. descriptor set に割り当て元の pool を持たせ、pool の破棄でその set を解放する。

HAL は変えない。

## 受け入れ

1. host の fixture（`plan/ws031/tests/run-vk-host-tests.sh`）が PASS。close で残った object が解放されること（fixture の
   確保数が close の後に戻る）と、pool の破棄で set が解放されることを試験で確かめる。
2. build は warning 0（i915 の image と `I915_TESTS=y` の compile）、変えた C の style-check で新しい指摘 0。
3. 実機（5330 の i915、capture）の zdesktop（p067 の scenario）が PASS（mview の窓が出て、mview の終了後も zwl が描き続ける）。
4. 実機の LCD・ベアメタルは範囲外。

## 結果（q467-i01、2026-09-26）

実装:

- `render/object.c` に `drv_i915_object_take`（session と種類と条件で 1 つ取り出す）。
- `render/objects.c` の `drv_i915_gfx_objects_release`: session の close（`drv_i915_render_close`）で、command pool（buffer ごと）、
  descriptor pool（set ごと）、pipeline（kernel を解放して）、fence、allocation（blob の list から外して）、その他の単一の確保を解放する。
  token（instance・device・queue）は `drv_i915_object_forget` が表から外す。submit は返事までに完了するので GPU は使っていない。
- descriptor set が割り当て元の pool を持ち、`vkDestroyDescriptorPool`（`drv_i915_gfx_destroy_dpool`）でその set を解放する。
- `drv_i915_gfx_memory_release`（allocation の解放を 1 か所に）、`drv_i915_gfx_command_pool_free`（command pool の解放を切り出し）。

試験:

- host の fixture（`run-vk-host-tests.sh`）PASS。追加・変更: descriptor の試験を 2 通り（wire で破棄し pool の破棄で set が消える／
  何も破棄せず allocation も残して close し `stub_live == 0`）、cmdbuf の 1 試験は pool・buffer・資源・allocation を残して close、
  sync の 1 試験は fence を残して close、pipe の 1 試験は pipeline（kernel）と module を残して close。いずれも確保数が 0 に戻る。
- 実機（5330 の i915、VFIO、capture。LCD・ベアメタルは未実施）: zdesktop の scenario に、描画中の Vulkan の窓（wltest）を
  `timeout -s KILL 6` で殺す service（`wlkill`、`WLKILL status=124`）を mview の前に足し、capture の最後に docked の mview を
  バーの × で閉じる段（`ended`）を足した。run17 で 6 検査 PASS（`desktop_drawn`・`dock_changes_view`・`wiseview_changes_view`・
  `wiseview_closes`・`close_ends_viewer`・`desktop_after_close`）、guest の log に `MVIEW DONE run=zd1 frames=2 reason=closed`、
  zwl は close の後も合成を続けた（`ZWL CLEANUP client=4 objects=13`、以後の frame の log）。Vulkan・import の ERROR 無し。
  画面は `build/ws031-p050/run17/`（`ended.png`: mview が閉じた後の 2 つの wl_shm の窓）。
- build は warning 0（i915 の image）。変えた C の style-check は HEAD と同数か減（新しい指摘 0）。

mview の終了（2026-09-26 ユーザー「mviewの終了の問題は修正をお願いします。」）: 以前の run で mview の log に DONE が無かったのは、
5330 側の watcher が stop の行を見つけられず QEMU の 360 秒の timeout で guest ごと止めていたため（mview の 250 秒の期限より前に）。
scenario で mview を × で閉じるようにし、mview は `reason=closed` で終わり、その後 guest が poweroff する（`vkwait2` を 15 秒に）。
capture の mview の位置は 4 番目の窓（cascade の step は 32 px）に直した。
