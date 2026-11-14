<!-- awesome-plan project=zedbsd record=ws328-p001 -->

# ws328-p001 設計（module の分け方・header・骨格・試験の道具・実装の Phase）

- WS: [WS328](../ws.md)。Queue: q2091-i01（P4、worktree `/home/awe/zedBSD-worktrees/p4-ws328`、branch `agent/p4-ws328`、main c5b8a10d24 から）。
- Status: in-progress（2026-11-14 P4）
- 手順: 標準廉価実装手順（[docs/agent/spec-module-delegation.md](../../../docs/agent/spec-module-delegation.md) §4）。

## 成果

- 設計: [design.md](../design.md)（乖離の確かめ §2、全体の形 §3、GPU と CPU の分担 §4、module の表 §5、display list の追加の案 §7、Vulkan §8、CSS §9、SVG の統合 §10、Phase の表 §11、make の変更の案 §13、範囲の外 §14、試験の道具 §15、資料 §16、技術の判断 §17、design-reviewer §18）。
- header（仕様）と .c の骨格（全関数が ENOSYS か何もしない）: `userland/desktop/libbrowser/vector/vec-{base,path,flatten,stroke,raster,gradient,rrect}.{h,c}`、`userland/desktop/libbrowser/svg/svg-tree.h`、`svg/svg-{values,pathdata,style,shape,pserver,picture,clip,draw,bitmap}.{h,c}`。
- 試験の道具: `plan/ws328/tests/run-unit.sh`・`check-skeleton.sh`・`unit/check.h`・`unit/raster-reference.h`・`unit/svg-test-tree.h`・`unit/test-harness.c`。
- 実装の Phase: `plan/ws328/phase002/` 〜 `phase023/` の phase.md。
- ユーザーへの問い: [user-questions.md](../user-questions.md)（U1〜U4、推し付き）。

## 結果

（design-reviewer の反映の後に書く。）
