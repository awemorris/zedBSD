<!-- awesome-plan project=zedbsd record=ws334-p004 -->

# ws334-p004 設計の確定: design-reviewer と基点の SHA

- 親: [WS334](../ws.md)。手順 docs/agent/spec-module-delegation.md の §4.7・§4.8。
- 状態: cleared（2026-11-01 02:43 Q1 の判定、main 5ea437eb7）
- 依存: p003。

## 目的

p002・p003 の header・design.md・全部の phase.md を design-reviewer 1 本に敵対的に読ませ、指摘を全部 design.md の表に記録し、反映するか理由を付けて見送る。header を確定させた commit を基点の SHA として ws.md の Resume point と全部の phase.md に書き、`BASE=<基点> sh plan/ws334/tests/check-skeleton.sh` が PASS。Q1 に、起動できる実装の Phase の一覧（順と並べ方）を返す。

## 結果（2026-11-01 P7）

- design-reviewer 1 本（HEAD 34aed7a3a を読んだ）: 指摘 34 件（高 7・中 17・低 10）。全部を反映した（見送り 0）。表は [design.md](../design.md) §16。#2 のうち engine の今の穴（file: の下位の資源を http の頁からも読める、POSIX でも同じ）は WS334 の範囲の外なので Bug の候補として Q1 に送った。
- 基点（header と試験の道具を確定させた commit）: **d393d3267**。全部の phase.md の `BASE=` をこの SHA にした。
- 確かめ: `check-skeleton.sh`（header 19、source 22、cc・clang・x86_64 と aarch64 の llvm-mingw で warning 0、`make browser-win64`・`browser-winarm64`）PASS。`run-unit.sh tools` PASS（posix と win32 の Wine）。`plan/tools/keiland-os-boundary/check.sh` で WS334 の file に失敗が無い（main の既の失敗だけ）。
- 確かめた事実（p020・p022 に反映）: llvm-mingw の `--delayload` で Vulkan の遅延の import が Wine で働く。llvm-mingw は `-lpthread` を明示しないと winpthreads を link しない。
- 起動の順（Q1 が 4〜6 本ずつ）:
  1. 1 組目: p005（os-win32・os-system）、p009（時刻帯の表）、p010（records）、p013（media-os）、p020（glue、Opus）。
  2. 2 組目（p005・p010 の後）: p006（os-file）、p007（os-net）、p008（os-time、p009 の後）、p011（files、p006 の後）、p012（queue）、p014（鍵の表）。
  3. 3 組目（いつでも。libkeiland と apps の module）: p015（frame）、p016（platform-win32）、p017（wide・input）、p018（CF_HTML）、p019（ICO）。
  4. 統合（Opus、順に）: p021 → p022 → p023 → p027。p025・p026 は p022 の後に並べてよい。p030 は p022 の後。p028 → p029 → p031 → p032。p024・p033・p034・p035 はその後（p033 は前に設計の小さな Phase）。
