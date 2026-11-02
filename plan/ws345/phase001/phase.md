<!-- awesome-plan project=zedbsd record=ws345-p001 -->

# ws345-p001 調べと設計

- WS: [ws.md](../ws.md)。Queue: q1959-i01（P8、worktree `/home/awe/zedBSD-worktrees/p8-ws345`、branch `agent/p8-ws345`、基点 main 4249e9a927）。
- 状態: cleared（2026-11-02 P8）

## 目的

今の thread と event loop を調べ、窓の event が止まる所を host で測り、thread の分け方を設計して、実装を Phase に分ける。product の code は書かない（計測の probe だけ）。

## 成果

- [design.md](../design.md): 調べ（§1）、計測（§1.3）、thread の表（§3）、持ち主の模型と DOM の貸し（§4）、libbrowser の口と shell の thread の表（§5）、描画（§6）、通信（§7）、frame の間（§8）、打ち切りと閉じ方（§9）、判断の点（§10）、危険（§11）、敵対的レビュー（§12）。
- 計測の probe: [../tests/stall-probe.c](../tests/stall-probe.c)、[../tests/stall-probe.sh](../tests/stall-probe.sh)（`sh plan/ws345/tests/stall-probe.sh [SCENARIO...]`、engine は `BROWSER_HOST_BUILD=build/ws345-host sh plan/ws074/tests/host-build.sh plain`）。
- p002〜p013 の phase.md（p013 は design-reviewer の指摘 4 で足した）。

## 確かめ（2026-11-02 P8、host）

- `BROWSER_HOST_BUILD=build/ws345-host sh plan/ws074/tests/host-build.sh plain`: exit 0（warning 0、`-Werror`）。
- `sh plan/ws345/tests/stall-probe.sh`（各 6 s）と、`stall-probe sync-xhr-read|sync-xhr-connect|docwrite-connect 40`（各 40 s）: 結果は design.md §1.3。
- QEMU・実機・Windows の計測は未実施。

## 結果

- 設計 [design.md](../design.md) を書き、design-reviewer（subagent）を 1 回流して、指摘 19 件（high 5、medium-high 3、medium 8、low 3）を全部 design.md と phase.md に反映した（design.md §12 の表）。p013（libkeiland の resize の口）を足した。
- Q1 に返した判断の点: J2（応答しない頁の問い、製品）、J6（窓の thread の iframe の枠の表の引き）、J7（D5 の paint の分け方）。P8 が決めた技術の点: J1・J3・J4・J5。
- 変えた試験の file: 無し（WS074・WS334 の試験は変えていない）。product の code は変えていない。
