<!-- awesome-plan project=zedbsd record=ws141-p008 -->

# ws141-p008: GPU共通shader frontend

Status: cleared
Disposition: normal
Parent: [WS141](../ws.md)
Queue: [2026-10-10承認済みi17](../compiler-refactor-20261010.md)

## Scope / acceptance

統合済みVC4のsoftware出力を使い、SPIR-V parser/IR/診断/parse-free APIを`src/drivers/gpu/compiler/`と`drv_gpu_`名前空間へ移す。i915とVC4をそのfrontendへ移行し、vendor backendの境界を保持する。旧i915 frontendの実装/互換aliasは残さない。両target build・短いparser/EU/QPU host・IR numeric/ownership互換確認がsoftware criteria。実機Keilandは別Phase/ユーザー担当。

## 設計と規則

[設計/全文C/Guardrail/検証範囲](../compiler-refactor-20261010.md)。依存はp006 software出力のみ。i915・BCM caller、platform/arm64とamd64のbuild list、関連する現役host/testを対象とする。HAL/UAPI/toolchainは不変。Master/共有Queue/GitHub投影はQ1へ保留する。

## 追加の理由

2026-10-10ユーザーの共通化指示により追加。p006は共通frontendへの依存に置き換わり、p007を最終sourceで再確認する。WSの実機acceptanceは変更しない。

## software clearance（2026-10-10）

共通parser/IR/APIと両backend参照を移行し、IR token/layout互換、対象RPi4/amd64 build、parser/EU/QPU/actual client hostを確認してi17/software criteriaを満たした。heap stateの全拒否時cleanupを確認、実機未実施は他Phase/WSに保持。[最終source/commands/results](../compiler-refactor-20261010.md#i17-software-clearancei18最終検証)。main統合はi18で実施、remote closure/projectionはQ1。


## main統合（2026-10-10）

i18により共通compiler source `fca9cd37f` をmerge `ab268b857` でmainへ統合し、212path hash/対象入力一致を確認。p008のsoftware clearanceを保持する。[統合/終了](../compiler-refactor-20261010.md#i18-main統合今回software実行の終了2026-10-10)。WSの実機acceptanceは未確認。
