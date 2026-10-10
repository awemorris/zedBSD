# ws193-p004: arm64選択でamd64へ戻るバグ

Parent: [../ws.md](../ws.md)
Status: cleared
Disposition: normal
Queue: [menuconfig-arm64-20261010](../codex-queue.md)
Approval: 最新ユーザーのRPi4 UAT前のmenuconfig arm64選択修正指示。
Scope / Criteria: CPUメニューでarm64を選ぶとtargetがrpi4へ更新され、CPU/Board/ヘッダーもarm64/RPi4、再度CPUメニューを開くとarm64を選択済み、保存したconfigのtarget階層がmakeで検証される。逆方向とcancelも既存仕様を保持。Python変更範囲をレビューし、短いhost確認、syntax/diff-check、WIP commit。C変更無し。
Finding: MENU_CPUS[index][0]はarchitecture名arm64で、[2]がplatform名rpi4。旧codeは[0]をplatformへ入れ、normalize_targetが未知platform arm64をamd64へ戻す。実select_cpu_board()の旧codeで再現済み。
Design: current lookup、CPU chooserのselected index、valuesへのplatform代入の3箇所を[2]で統一。menu entryとconfig契約を変えない。
Standards / bounds: AGENTS/Guardrailと既存Pythonの規約。全変更review、短いhostのselection/snapshot/save/make validate、curses操作の有限確認。新suite/全回帰/image/QEMU/toolchain変更無し。
Integration: CI修正のmain/push承認はCI taskのもの。今回の具体的commitを準備してmain統合を最後に確認、pushはしない。shared Master/Queue/cacheはQ1。

## menuconfig-arm64-i01 outcome

cleared（修正/有限host/WIP成果のscope）。tools/menuconfig.pyの3箇所をplatform field [2]へ統一。旧関数でarm64選択→amd64戻りを再現し、修正後の同じ実関数でRPi4更新、CPU/Board/ヘッダー、再選択index1、cancel、save/load、逆方向amd64/nativeをPASS。実 `make menuconfig` のPTY操作でもCPU arm64/Board Raspberry Pi 4/Current target Raspberry Pi 4 Arm64と、再CPUメニューのarm64 highlightを確認。保存cfgのmake validate-image-configとPython syntax/diff-check PASS。[証拠](../tests/arm64-selection-20261010.md)。main統合承認を残し、他source/driver/toolchain/user config.mkは変更無し。
