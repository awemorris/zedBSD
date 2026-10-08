<!-- awesome-plan project=zedbsd record=ws031p025 -->

# ws031-p025: 異常系（範囲外の index・offset、65536 を超える command buffer、descriptor の上限、終わらない loop）

Phase ID: `ws031-p025`
Parent: [WS031](../ws.md)
Status: blocked（UAT 待ち）（2026-10-09 P1: host の読みと既存の host 試験の照合は済み、新しい不具合は無し。終わらない loop の GPU の hang の扱いは実機の i915 が要る）
Phase disposition: normal
Queue: Q1 の dispatch（P1、2026-10-09、ベータ3 の合間の仕事）
元: [phase015](../phase015/phase.md) の「異常系」

## 読みと照合（2026-10-09 P1）

| 項目 | 今の code | host 試験 |
| --- | --- | --- |
| 65536 を超える command buffer | `render/command.c` の `i915_command_op`: 伸ばせない list は overflow の印を付け、後の操作を捨てて、vkEndCommandBuffer が `VK_ERROR_OUT_OF_HOST_MEMORY` を返す（`I915_GFX_MAX_OPS` 65536） | cmdbuf の試験が 65537 個の操作で `VK_ERROR_OUT_OF_HOST_MEMORY` と log「needs more than 65536 operations」を確かめている |
| 範囲外の index・vertex の offset | `render/state.c`: index buffer は offset が buffer の末尾以上か index の大きさに揃わなければ EINVAL。vertex buffer は offset が buffer の大きさを超えれば EINVAL。どちらも 3DSTATE の大きさの欄に「buffer の末尾まで」を書くので、末尾を超えて読む index・vertex は hardware が 0 を返す（PRM の VERTEX_BUFFER_STATE・3DSTATE_INDEX_BUFFER の Buffer Size） | cmdbuf の試験が offset の拒否を確かめている（2026-10-09 に uint8 の場合も足した、ws031-p026） |
| descriptor の上限・範囲外 | `render/descriptor.c`: set の binding の数を超える layout、binding・配列の要素の範囲外の書き込みは EINVAL。UBO・SSBO の offset が buffer の末尾以上なら range 0、範囲は末尾で切る（`state.c` の push の block）。texel buffer の view の offset・range が buffer を超えれば EINVAL | 読みだけ（各拒否をどの試験が確かめるかは照合していない） |
| 終わらない loop | GPU の hang の扱い（engine の reset・request の timeout）は WS029・WS075 の engine の側。compiler は loop の回数を数えない（Vulkan は終わらない shader を未定義とし、driver に検出を求めない） | 無し（実機が要る） |

新しい不具合は見つからなかった。code の変更は無い。

## 残り（blocked、UAT 待ち）

- 実機（5330）で、終わらない loop の fragment shader の draw の後に GPU が回復するか（その request の timeout と、後の draw）を記録する。回復の経路が WS031 の範囲の外なら、その理由を書く。
