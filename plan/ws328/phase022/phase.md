<!-- awesome-plan project=zedbsd record=ws328-p022 -->

# ws328-p022 統合の試験（WPT・GPU と CPU の比べ・QEMU・実機の試験の一覧）

- WS: [WS328](../ws.md)。設計: [design.md](../design.md)。
- Status: planned（2026-11-14 p001 が作成、2026-11-15 design-reviewer の反映で改訂）
- 担当: Opus（phase-runner、統合なので推論が多い）。
- 依存: p024・p025・p028（依存の Phase が main に merge され cleared の後に始める）。

## 1. 範囲

- WPT の該当の reftest（design.md §15 の directory）を CPU で流し、p018 の基準の数と後の数を表に（`plan/ws328/results/` の text、件数は自分で数える）。
- `plan/ws328/tests/pages/` の全部を host の lavapipe と QEMU の Venus で GPU と CPU を比べる 1 本の script（配置 → 起動 → 実行 → 回収 → 要約）。倍率の transform と view transition の頁（角丸と SVG）を含む。
- 起動の確認（`plan/tools/boot-test.sh`）と、Keiland の browser で頁を開いた画面（QEMU の screendump）。
- 実機（5330 の i915、Pi 4、Pi 5、VF2）の試験の一覧を ws.md に（ユーザーが拾える形。自分では流さず、Q1 の割り当てがあれば流す）。
- 見つかった不具合は、小さな物はその場で直し、大きな物は Q1 に報告（Bug Board は Q1）。

## 2. 試験

- 上の script の要約、WPT の数の表、未実施の一覧（実機）。

## 3. 守る事

- AGENTS.md の §4・§7・§9・§10、plan/coding-style.md の全文、plan/guardrail.md の出典・開発の管理情報・「画像の処理は GPU で」。
- vector/・svg/ の header は変えない（基点の SHA、`BASE=<SHA> sh plan/ws328/tests/check-skeleton.sh`、check 6 の toolchain は module の phase.md §6 と同じ）。header の契約の誤りを見つけたら header を変えずに止めて Q1 に返す（design.md §11 の再凍結。p023 は書き方だけで契約の直しを持ち越さない）。
- Makefile・`mk/`・`sources.mk` の変更は Q1 経由でユーザーの承認の後（design.md §13 の差分、user-questions の U5）。HAL・toolchain は変えない。
- 削除（rm、`git rm`）は担当がしない。path と理由を Q1 に送る。
- 他の実装（Blink・WebKit・Gecko・Skia・resvg・librsvg など）の code・comment・構造を写さない。
- WS074 の host の試験の期待の値を直す事は Q1 の許可の範囲（design.md §12）。直した file を結果に全部書く。WS328 の新しい試験は `plan/ws328/tests/` に置く。
- QEMU の console・serial の log で判定しない（boot-test.sh、screendump、gdbstub・QMP）。
- frame ごとに CPU が画素を触らない（cache の miss の時の coverage と ramp の作成は内容が変わった時だけ、§4 の予算の中）。

## 結果

（担当が書く。）
