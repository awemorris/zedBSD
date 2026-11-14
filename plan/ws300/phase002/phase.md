<!-- awesome-plan project=zedbsd record=ws300p002 -->

# ws300-p002: VMD の suspend・resume（隠れた root port、VMCONFIG、suspend の順）

Phase ID: `ws300-p002`
Parent: [WS300](../ws.md)
Status: canceled の推し（2026-11-15 P5 q2114-i01、Q1 が確定: [BUG-424](../../bugs/BUG-424.md) の debec3781b・295eea4623 で main に入った形に置き換わった。この Phase の code は merge しない）
Phase disposition: replaced by BUG-424
Queue: q1683-i01（P6、branch `agent/p6-ws300`、merge されず）

## 範囲（第 2 版の設計）

[design.md](../design.md) §4.1（第 3 版では BUG-424 との差だけ）。VMD の `.suspend`・`.resume`、隠れた root port の save・D3hot・復元、VMCONFIG の保存と書き戻し、suspend の順の明示。

## 置き換え（2026-11-15 P5）

- BUG-424（2026-11-13 P2）が branch のこの Phase を読み、PCI の core が driver の無い function の config の保存・復元と順（upstream で数える深さ）を持ち、VMD の driver は VMCONFIG だけを持つ形で main に入れた。
  5330 の `sleepctl devices` の往復は BUG-424 の b424d で成功（VMD の設定は kept、root port の reset 無し）。
- BUG-424 に入っていない第 2 版の物: 隠れた root port の D3hot（No_Soft_Reset の port だけ、下の function が全部 D3hot の時だけ）、`pci.vmd.d3=force`。要否は p006 の 5330 の測り（S0 idle の PC の滞在）の後。
- 第 2 版の実装の記録（2026-10-29 P6、branch の d86fe0c5d・d503c39ea）は git の履歴（branch `agent/p6-ws300`、`plan/backup-20261114/agent_p6-ws300.diff`）にある。
