# ws025-p028 q125 results

Status: uncleared
Date: 2026-09-09

## 確認した事実

HEAD 34a1f6dの現ソースとq122/p024の既存証拠を確認した。
vmspace_pin_user_pagesは存在するが、pinはDMAとの同時書換えを禁止するcontent leaseではない。file content leaseとuser aliasの寿命は別。p024 controlled比較はCPU改善を示していない。

## 未完了と再開

direct user I/Oを正当化する対象workloadのcopy律速測定がなく、既存pinだけではalias/COW整合を満たせない。p024のコピー削減結果をdirect I/OのCPU改善と読み替えない。

対象workloadのcopyコストを分離した測定、および共有aliasのfreeze/COW方式を検証するVM fixture。測定と契約が揃うまで既存copyで運用する。

production実装・新規性能測定を行ったとは主張しない。既存挙動と既定値を維持した。
