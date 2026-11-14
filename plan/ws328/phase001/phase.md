<!-- awesome-plan project=zedbsd record=ws328-p001 -->

# ws328-p001 設計（module の分け方・header・骨格・試験の道具・実装の Phase）

- WS: [WS328](../ws.md)。Queue: q2091-i01（P4、worktree `/home/awe/zedBSD-worktrees/p4-ws328`、branch `agent/p4-ws328`、main c5b8a10d24 から）、q2098-i01（P4、worktree `/home/awe/zedBSD-worktrees/p4-ws328b`、branch `agent/p4-ws328b`、main f6d4437bc2 から: design-reviewer の反映）。
- Status: in-progress（2026-11-14 P4、2026-11-15 P4 が q2098-i01 で design-reviewer の反映を終えた。cleared は Q1）
- 手順: 標準廉価実装手順（[docs/agent/spec-module-delegation.md](../../../docs/agent/spec-module-delegation.md) §4）。

## 成果

- 設計: [design.md](../design.md)（乖離の確かめ §2、全体の形 §3、GPU と CPU の分担 §4、module の表 §5、display list の追加の案 §7、Vulkan §8、CSS §9、SVG の統合 §10、Phase の表 §11、make の変更の案 §13、範囲の外 §14、試験の道具 §15、資料 §16、技術の判断 §17、design-reviewer §18）。
- header（仕様）と .c の骨格（全関数が ENOSYS か何もしない）: `userland/desktop/libbrowser/vector/vec-{base,path,flatten,stroke,raster,gradient,rrect}.{h,c}`、`userland/desktop/libbrowser/svg/svg-tree.h`、`svg/svg-{values,pathdata,style,shape,pserver,picture,clip,draw,bitmap}.{h,c}`。
- 試験の道具: `plan/ws328/tests/run-unit.sh`・`check-skeleton.sh`・`unit/check.h`・`unit/raster-reference.h`・`unit/svg-test-tree.h`・`unit/test-harness.c`。
- 実装の Phase: `plan/ws328/phase002/` 〜 `phase029/` の phase.md（p024〜p029 はレビューで分けた物）。
- ユーザーへの問い: [user-questions.md](../user-questions.md)（U1〜U4、推し付き）。

## 結果

2026-11-15 P4（q2098-i01）。日付は host の `date`。

- design-reviewer: 2 回。1 回目（前の担当 q2091 が素案 7cc9eca796 に起動、Q1 の要約で受けた）High 7・Medium 15・Low 11。2 回目（q2098 で起動）重大 6・中 21・軽 23。全部を [design.md](../design.md) §18 に表で記録し、全部を採った（R12 の影の積分は 1 回目の案と違う形、R27 は U1 に案 C を足して推しは A のまま）。採らなかった物は無し。
- 主な反映: CSS の倍率と view transition の snapshot（§7）、shader に atan・exp を使わない（`vec_atan2`・`vec_exp`、§8.4）、GPU の Phase を CSS・SVG の切り替えより先に（§11）、SVG の clip の表（§7）、継承の口（`svg_picture_build` の outer、`svg_style_of`、`svg_tree.parent`）、pathLength の向き、hint の entry、影の積分の形、side_share の定義、作業量と memory の上限、Phase の分割（p024〜p029）、header の誤りの再凍結の手順。
- commit: 3ffc49526d（header・骨格・試験の道具）、**57bf94e62f（design.md・phase.md・user-questions を足した反映の全体、基点の案）**、この結果と ws.md の commit。
- 確かめ（自分の worktree、x86_64 の toolchain と sysroot を `make CONFIGMK=config/config.x86_64.mk llvm-toolchain-cached`・`sysroot` で自分の build/ に入れた）:
  - `BASE=57bf94e62f sh plan/ws328/tests/check-skeleton.sh` → `check-skeleton: PASS`（cc・clang の header と .c、engine の include 無し、管理情報無し、header の不変、zedBSD の clang と sysroot の compile を含む）。`BASE=f6d4437bc2` では「headers changed」で FAIL になる事（照合が効く事）、sysroot が無い時に `INCOMPLETE` になる事も確かめた。
  - `sh plan/ws328/tests/run-unit.sh harness` → cc・clang とも `test-harness: PASS (1127 checks)`、`run-unit: PASS`。
  - `git diff --check` 無し。sources.mk の挿入の案は context 付きの diff にして `git apply --check` が通る事を scratchpad で確かめた（当てていない）。
  - 影の積分: header の新しい算法を倍精度の Python で書き、密な数値積分と比べた最大の差 0.0085（σ 0.5〜3、角 8〜100 px、楕円の角）、σ 10 で 0.0053、σ 30 で 0.0000。旧の算法（y の 8 点）は 0.059。float と A&S の erf での値は p007 が確かめる。
- 未実施: 新しい shader の式を zedBSD の shader compiler に通す事（p020）、各 driver の頂点の入力と flat の入力の上限（p020）、§4 の予算の値（p026）。arm64・riscv64 の sysroot での check 6（script は x86_64 だけ）。
- sources.mk の差分は案のまま（design.md §13、user-questions の U5）。

p001 の cleared の推し: 受け入れの条件（design-reviewer の反映、試験の道具の PASS、基点の SHA、Phase の表、ユーザーへの問い）は満たした。Q1 が 57bf94e62f を確かめて基点に決め、U1〜U5 の答えを待たずに p002・p017 から始めてよい（U の答えで変わるのは p018 以後）。
