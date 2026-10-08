<!-- awesome-plan project=zedbsd record=ws083-p001 -->

# ws083-p001: Vulkan Video（H.264 decode）と i915 VCS・MFX の設計

Status: in-progress → Q1 の判定待ち（2026-10-08 q902 P1 の照合: 第 3.1 版、H1〜H5・HD1〜HD6 は 2026-10-07 に決定、q897 の設計と実装の照合の review は blocking 無し）（旧: in-progress（q833、P1））
Disposition: normal
Parent: [WS083](../ws.md)

## 範囲

[design.md](../design.md) の設計（API の面、protocol、libvulkan・i915 の構造、MFX の command 列、memory、試験、Phase）。code は書かない。UAPI の差分は [proposed/](../proposed/README.md) に置くだけ。

## 受け入れ

design-reviewer の review の反映、人の判断（design.md §10）の提示、Q1 の ACK。

## 記録

- 2026-10-07 第 1 版（kernel-and-driver-designer）と design-reviewer の review（blocking 4・should-fix 17、design.md §15）。
- 2026-10-07 夕 P1（新しい世代）: Q1 の ACK（範囲 1〜5、ws.md は直してよい、Mesa は事実の確認だけ）。第 2 版: review の全指摘を code・vk.xml・Mesa 25.0.7・genxml で確かめて本文に反映（対応は design.md §14）。主な変更: Begin の slot の意味を規格どおり（B1）、scaling list は scan → raster（B2）、kernel の検べと picture の飛ばし（D17、B3）、capset は 176 byte の native の語で Venus の fork の flags に触れない（D2・HD1、B4）、engine record の class・hang の封じ込め・遅延の VCS context（S1・S6・S17）、submit の error の写像（D18）、Frame Size の上限（D20）、header は disk にある 1.4.309 から（D14）、stream は小さい合成を tree に（D16・HD4）、`i915.debug=video` の門（D19）、genxml の独立の decoder（D24）。
- `proposed/gpu-op-video.diff`（`git apply --check` 済み）と `proposed/README.md`（capset の native の語）。
- 2026-10-07 夕 第 2 版への再 review（blocking 2・should-fix 12・minor 12）→ 第 3 版（386e9cd16、§16）。第 3 版への最後の review（blocking 1: video の queue の timeline を VCS0 に写すと fence の marker が VCS0 で走る、should-fix 7）→ 第 3.1 版（517767d86、§17、再 review はしない）。人の判断の一覧（H1〜H5・HD1〜HD6、既定の案つき）は Q1 に送った（ユーザーへ）。
- 次: p003a（i915 の VCS の土台、wire は不要）を Q1 の ACK の後に。p002・p003b 以降は H1〜H3・HD1・HD6 の判断の後。
