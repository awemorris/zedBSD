<!-- awesome-plan project=zedbsd record=ws328-p022 -->

# ws328-p022 統合の試験（WPT・GPU と CPU の比べ・QEMU・実機の試験の一覧）

- WS: [WS328](../ws.md)。設計: [design.md](../design.md)。
- Status: planned（2026-11-14 p001 が作成）
- 担当: Opus（phase-runner）。
- 依存: p019・p020・p021（依存の Phase が main に merge され cleared の後に始める）。

## 1. 範囲

- WPT の該当の reftest（design.md §15 の directory）を CPU で流し、WS328 の前（main の p018 の前の commit）と後の数を表に（`plan/ws328/results/` の text、件数は自分で数える）。
- `plan/ws328/tests/pages/` の全部を host の lavapipe と QEMU の Venus で GPU と CPU を比べる 1 本の script（配置 → 起動 → 実行 → 回収 → 要約）。
- 起動の確認（`plan/tools/boot-test.sh`）と、Keiland の browser で頁を開いた画面（QEMU の screendump）。
- 実機（5330 の i915、Pi 5、VF2）の試験の一覧を ws.md に（ユーザーが拾える形。自分では流さず、Q1 の割り当てがあれば流す）。
- 見つかった不具合は、小さな物はその場で直し、大きな物は Q1 に報告（Bug Board は Q1）。

## 2. 守る事

- AGENTS.md の §4・§7・§9・§10、plan/coding-style.md の全文、plan/guardrail.md の出典・開発の管理情報・「画像の処理は GPU で」。
- vector/・svg/ の header は変えない（基点の SHA、`BASE=<SHA> sh plan/ws328/tests/check-skeleton.sh`）。header の誤りは規約の見直しの Phase（p023）へ持ち越し、Q1 に報告する。
- Makefile・`mk/`・`sources.mk` の変更は Q1 経由でユーザーの承認の後（design.md §13 の差分）。HAL・toolchain は変えない。
- 削除（rm、`git rm`）は担当がしない。path と理由を Q1 に送る。
- 他の実装（Blink・WebKit・Gecko・Skia・resvg・librsvg など）の code・comment・構造を写さない。
- QEMU の console・serial の log で判定しない（boot-test.sh、screendump、gdbstub・QMP）。

## 結果

（担当が書く。）
