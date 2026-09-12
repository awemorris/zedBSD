<!-- awesome-plan project=zedbsd record=ws029 -->

# WS029: i915ネイティブGPU実装

<!-- awesome-plan-current:start -->
Status: planning
Primary Milestone: MG006
Related Milestones: MG003
Objectives: O2, O4
Parent: [Master](https://github.com/awemorris/zedBSD/issues/1)
Queue: none
<!-- awesome-plan-current:end -->

## 単一目標

WS014で検証・整理したGPUフレームワークを使い、選定したIntel実機でzedBSDのi915 driverによる描画・表示を成立させる。ホストLinuxのi915を使うだけのQEMU/Venus実行とは別の成果。終了したWSを再利用せず、新しいWSとして持つ。

## 順序・依存

ユーザー指示の順序はGPUフレームワークのみ→QEMU＋Venusでのデバッグ/API改善→i915実装。前段は[WS014](https://github.com/awemorris/zedBSD/issues/15)が所有する。WS014最終contract・規約/検証結果を受け取り、i915で判明した不足も記録して整理する。

## 範囲・受け入れの具体化

struct drv_gpu_interfaceのstatic callback実装とPCI経由のGPU登録を用いる。対象GPUは従来のLatitude 5320を候補とし、PCI ID/世代、firmware、memory/submit/display/resetの要件、ユーザー空間driverとの分担、ライセンス境界を実装前に確定する。Linux i915コードの全面移植を今回決定したとは扱わない。

実機で合意した描画・表示テスト、console fallback、必要な同期/資源回収を確認することを完了方向とする。正確なAPI profileと受け入れは前段成果・実機情報で具体化する。PPCや他GPUを同居させない。

## Phase registry

前段成果と対象実機を確認してから有限Phaseを作成する。今はplanningで実行Phase/Queueなし。コード実装の終盤には適用規約全文と最終ソースの整合確認Phaseを必ず含める。

## 適用規約・実行境界

[Guardrail](https://github.com/awemorris/zedBSD/issues/363)とローカルplan/coding-style.mdの全文を実装前に読む。HAL責務/hal.hの変更は別途適用承認が必要。既存PCI/VFS/VMの責務を確認し、大規模refactor前の配置を仮定しない。aggregate make checkは禁止。必要な対象buildはmake -j16と意味のある限定確認を用いる。無関係な変更を保護する。

ユーザーは計画・GitHub公開を指示した。まだ有限Queue、実行範囲と調査上限は選択していない。コード実装/build/QEMUは未実行。資料のgit add/commitはユーザーが行うためエージェントはadd/commit/pushしない。
