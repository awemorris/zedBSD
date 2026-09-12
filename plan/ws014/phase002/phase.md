<!-- awesome-plan project=zedbsd record=ws014-p002 -->

# WS014 p002: GPUフレームワークのみの実装

<!-- awesome-plan-current:start -->
Status: planning
Phase disposition: normal
Parent: [WS014](https://github.com/awemorris/zedBSD/issues/15)
Queue: none
<!-- awesome-plan-current:end -->

Combined ID: `ws014-p002`
Primary Milestone: MG006

## 目標・範囲

GPUコア、struct drv_gpu_interface、device/session/handleの共通管理、/dev/gpuNの入口とcallback dispatch、PCI側の登録/解除連携を実装する。GPU driverはstatic callbackを持つinterfaceとinstance固有データを提供し、PCI側がattach成功後にGPUコアへ登録する。構造体版の44 callback案は検討材料で、必要な最小contractから具体化する。

## 前提・手順

p001から必要なinterface/PCI引渡し/所有権の設計判断を得る。p001の未決定を黙って確定扱いしない。PCI attachは現状int戻り値でGPU descriptor引渡しがないため、その連携方式を先に設計する。共通の権限・handle・参照寿命、必須/任意callback検証、公開/rollback/unregisterの順序を実装する。

## 非対象

virtio-gpuの実デバイス実装、Venus移植、Vulkan描画、i915実装は含めない。callbackとライフサイクルを検証する最小のテスト用backendは使用可能だが、GPU描画成功とは呼ばない。

## 受け入れ・検証

登録→device公開→session操作→callback dispatch→close/unregisterが成立する。未対応callback・不正handleを拒否し、登録失敗のrollback、使用中参照を持った解除で資源を早期解放しないことを限定テストで確認する。対象platform buildを通す。実GPUなしで達成できるframeworkの受け入れとする。

## 残件と再開条件

U/K表、callback/型/PCI連携資料と利用例を更新し、p003が接続可能なcontractを引き渡す。現在未着手。次はp001の必要判断と有限Queue選択。

## 適用規約・実行境界

[Guardrail](https://github.com/awemorris/zedBSD/issues/363)とローカルplan/coding-style.mdの全文を実装前に読む。HAL責務/hal.hの変更は別途適用承認が必要。既存PCI/VFS/VMの責務を確認し、大規模refactor前の配置を仮定しない。aggregate make checkは禁止。必要な対象buildはmake -j16と意味のある限定確認を用いる。無関係な変更を保護する。

ユーザーは計画・GitHub公開を指示した。まだ有限Queue、実行範囲と調査上限は選択していない。コード実装/build/QEMUは未実行。資料のgit add/commitはユーザーが行うためエージェントはadd/commit/pushしない。
