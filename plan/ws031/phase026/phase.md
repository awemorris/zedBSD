<!-- awesome-plan project=zedbsd record=ws031p026 -->

# ws031-p026: 小さな欠落（uint8 index、非整列の copy、viewport、pipeline の漏れ、discard の HALT）

Phase ID: `ws031-p026`
Parent: [WS031](../ws.md)
Status: blocked（UAT 待ち）（2026-10-09 P1: host の分は実装と host 試験まで済み。i915 は QEMU に無く、実機 5330 の確認が要る。HALT は未着手）
Phase disposition: normal
Queue: Q1 の dispatch（P1、2026-10-09、ベータ3 の合間の仕事。ユーザーの順: WS031 から）
元: [phase015](../phase015/phase.md) の「小さな欠落」の項目

## 範囲

phase015 の 28〜31 行: uint8 index（`VK_INDEX_TYPE_UINT8_EXT` を拒否するか対応するか）、4 byte に揃わない `vkCmdCopyBuffer`、viewport・scissor の index > 0、負の viewport の高さ（`VK_KHR_maintenance1` の flip）、shader の compile の失敗で公開されない pipeline が漏れる件、discard の早い終わり（HALT）。ws.md の表にあった「host 試験（ws031 の display の 6 件と ws029）が perf.c を link しない」件は、その試験が 2026-10-03 の repository の作り直しで tree に無いので対象が無い（ws031-p049 と同じ）。

## 実装（2026-10-09 P1）

- uint8 index: `render/state.c` の `drv_i915_gfx_emit_index_buffer` で `VK_INDEX_TYPE_UINT8_EXT` を `GEN12_INDEX_BYTE`（1 byte、どの offset でも可）にした。primitive restart はこの executor にまだ無いので、cut index は関係しない。libvulkan は `VK_EXT_index_type_uint8` を広告していない（executor が受けられるようになっただけ。広告は libvulkan の範囲で、この Phase の外）。
- 非整列の copy: `render/command.c` の `i915_execute_buffer_copy` は、offset と size が 4 の倍数なら R8G8B8A8_UNORM、偶数なら R8G8_UNORM、それ以外は R8_UNORM の texel で copy する（vkCmdCopyBuffer は揃えを求めない。linear の surface の address と pitch は texel 単位で足りる、PRM の RENDER_SURFACE_STATE の Surface Base Address の「element-size aligned」）。ENOTSUP の拒否をやめた。
- 負の viewport の高さ: `render/state.c` の `i915_state_write_viewport` で、高さの符号が負なら viewport の矩形の y- を y + height、y+ を y - 1 にした（anv の y_min・y_max と同じ）。変換（m11 = height / 2、m31 = y + height / 2）はそのまま。`I915_FLOAT_SIGN` を `math.h` に移した。
- viewport・scissor の index > 0: 今のとおり読んで捨てる（pipeline は 1 つの viewport、multiViewport を広告していない）。cmdbuf の host 試験が既に確かめている。決め: 拒否しない。
- pipeline の漏れ: `render/pipeline.c` の `i915_gfx_undo_pipelines` が、作成の失敗で、その作成が公開した物を外し、全部の kernel を release して free する（graphics と compute の両方。前は graphics の失敗で record が残り、publish の途中の失敗も残っていた）。
- HALT: 未着手。EU の model に HALT が無く、実機の確かめが要る（残り）。

## 確認

| command | 結果 |
| --- | --- |
| `sh plan/ws031/tests/run-vk-host-tests.sh "pipe cmdbuf res resdispatch sync"`（ordinary と ASan・UBSan、leak の検出あり） | PASS |
| 足した host 試験 | cmdbuf: 6 byte（2→10）は R8G8 の 3 texel、7 byte（1→3）は R8 の 7 texel。index の 1 byte の format と offset 3。viewport の upright と flip（m11 -8、矩形 0〜15）。pipe: 拒否された pipeline の後に何も残らない（前の試験は 1 block の漏れを確かめていた） |
| `make ZEDBSD_CONFIG=plan/ws035/tests/config-amd64-zdesktop.mk BUILD=build/p1-ws177 build/p1-ws177/vmunix`（-Werror） | 成功、warning 0 |
| 実機（5330 の i915） | 未実施（blocked、UAT 待ち）: 非整列の copy の結果の byte、uint8 index の draw、負の高さの viewport の上下 |

## 残り

- HALT（discard で全部の channel が消えた thread を早く終える）。
- 実機の確かめ（上の 3 つ）。vke2 か vkx に step を足すのは、試験の kernel の大きさの余裕を見てから。
