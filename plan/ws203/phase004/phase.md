<!-- awesome-plan project=zedbsd record=ws203-p004 -->
# ws203-p004: GENETの実機初期化失敗を修正

Parent: [WS203](../ws.md)
Status: cleared
Disposition: normal
Queue: [repair Queue](../codex-repair-20261011.md)

Authority: current userのRPi4 Ethernet実装・DHCP起動確認指示と2026-10-11の実機写真。GENET PHY ID 600d84a2の後でinitialization failed (21)、MMIO release failed (3)。既存p001/p002の当時のbuild/model証拠を保持、実機受入p003は未達。
Scope: 既存HAL APIのarm64 GIC trigger設定と共有direct MMIO aliasのrelease実装を補完。HAL API/責務変更なし。有限の調査はこの確認済み失敗と直結する呼出契約だけ。USB DMA修正はWS048 p010が所有する。
Criteria: level/high IRQ189の既存契約を実装、invalid/未対応設定の拒否、MMIO releaseで共有blockを破壊しない、短いhost確認、warning0の現在config arm64 kernel build、最終変更ソースのC全文review。実機DHCP/SSHはp003に残す。QEMU/toolchain変更/pushなし。
Standards: AGENTS.md、plan/guardrail.md、plan/coding-style.md全文、.clang-format、plan/standards/automation.md。共有Master/Queue/history/cacheはQ1投影待ち。
Design: GICv2 ICFGRのSPI trigger bitだけをmasked状態で更新しreadback、低極性は非対応。RAM/Device共通direct aliasはkernel存続中共有されるためreleaseでblockを剥がさず正しいrangeを検証する。
Reference: Arm GIC Architecture v2 IHI0048B Table4-18 https://documentation-service.arm.com/static/5f8ff21df86e16515cdbfafe （register仕様のみ確認、ソース複製なし）。

## Terminal scoped result / 2026-10-11

限定source/build criteria cleared。[最終差分・commands・versions・旧/新対照・全文規約・limitations](../../ws203/tests/initialization-repair-20261011.md)。現在config warning0 arm64 kernel build、DMA/GENET既存host checksとGIC/MMIO限定check PASS。実機は未実施、親WSの受入未達を保持。先行Phaseの過去outcomeは書き換えない。main統合承認待ち、共有投影/GitHubはQ1 pending。
