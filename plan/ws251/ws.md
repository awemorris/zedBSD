<!-- awesome-plan project=zedbsd record=ws251 -->

# WS251: source の tree 全体から開発の管理情報を削除する

<!-- awesome-plan-current:start -->
Status: incomplete・保留（作業対象になった時）（2026-10-27 P3 の精査 q1608、ユーザー「…masterとWSを修正して、completedやcanceledにしてください。」: p002（GPU ドライバ）cleared。2026-10-15 ユーザー「開発管理はすべてのソースから削除しますが、今は作業対象になったときに削除するだけでいいです。…独立したWSを作って、あとで…」。src/drivers の gpu/ 以外は WS296 に移した（2026-10-27 00:22））。旧: incomplete（2026-10-15 作成。2026-10-17 ユーザーの指示で GPU ドライバから着手: p002、P1）。残り・制限: p001 の棚卸し、drivers の外の群
Primary Milestone: MG001
Parent: [Master](../master.md)
Related: [Guardrail](../guardrail.md)「source の中の開発の管理情報」、[WS247](../ws247/ws.md)（nvrtx で済み）、[WS248](../ws248/ws.md)（AX211、管理番号が残る）、[WS249](../ws249/ws.md)（BCM43455、削除を含む）
Resume point: p002（GPU ドライバ、i915 から）を P1 が q1240-i01 で。p001 の tree 全体の棚卸しは後。
<!-- awesome-plan-current:end -->

## ユーザーの指示（原文、2026-10-15）

「開発管理はすべてのソースから削除しますが、今は作業対象になったときに削除するだけでいいです。WS248でも消してほしかったですが、また今度でいいです。独立したWSを作って、あとでソースツリー全体を見直しましょう。WS249では削除を行いましょう。」

## 範囲と完了の条件

- 対象: tree の source（src/・include/・userland/・bootloader/・platform/ など、plan/ と docs/ を除く）の comment・log の文字列。
- 削除する物: WS・Phase の番号（wsNNN、pNNN、wsNNN-pNNN）、Queue（qNNN）、Bug の番号（BUG-NNN）、試験の回（T1-NNN、HW-T37 など）、plan/ への参照、日付つきの決定の記録、開発の段の符号（nvrtx の N0・P4m の類）。
- log の文字列を変える時は、それを照合する試験・道具（plan/tools・plan/ws*/tests・tests/ の scenario）を追従させる。
- 完了: 機械の検査（例: `git grep -nE '\bws[0-9]{3}\b|\bq[0-9]{3,4}\b|BUG-[0-9]{3}|\bT[0-9]-[0-9]+|plan/'` を source の範囲で）が 0 件、build warning 0、変えた log の照合先の試験が通る。
- それまでの規則（Guardrail）: 作業の対象になった file では、その作業の中で削除する。

## Phase

| Phase | 内容 | 状態 |
| --- | --- | --- |
| p001 | 棚卸し: 機械の検査で件数と file の一覧、log の文字列の照合先の一覧、群ごとの分け方 | planned |
| [p002](phase002/phase.md) | GPU ドライバ（src/drivers/gpu/）の comment と log。開発・試験の運用の話も消す（2026-10-17 ユーザー） | cleared（2026-10-19 Q1、全 file の通読はしていない） |
| p003〜 | 残りの群ごとの削除（p001 で分割）。src/drivers の gpu/ 以外は [WS296](../ws296/ws.md) に移した（2026-10-27 00:22） | planned |
- 2026-10-30 Q1: userland/base/net/main.c・networkd/main.c の comment に管理情報（ws005-p024・BUG-187・q631 など）が残る（WS009 p025 の P1 の所見）。この WS の範囲で消す。
