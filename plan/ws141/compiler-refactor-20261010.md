# WS141 shader共通化 / 2026-10-10

Status: active
Owner: WS141独立session、専用worktree `ws141-codex`
Parent: [WS141](ws.md)

## 承認と有限scope

ユーザー2026-10-10:「vc4の実装が一通り終わってからでいいのですが、i915のshaderをvc4ドライバでも使っていますが、これはdrivers/gpu/compiler/ 以下に drv_gpu_ プレフィックスで共通化して、i915とvc4をリファクタできませんか？」「私は寝るので、起きたらRPi4実機でテストします。静的レビューと改善、それから上記リファクタを進めて、目標を達成してください。」

前実行はsoftware/main統合までfinished、実機は未確認。この追加指示を今回の有限実行の承認とし、既存のself-merge承認を維持する。共有compiler・i915の参照・両platform source list・対応する現役試験の変更はこのrefactorの範囲。Master/共有Queue/Guardrail/GitHubはQ1担当。HAL/UAPI/toolchainの変更は対象外。

| Attempt | Phase / scope | Status | 依存・criteria |
| --- | --- | --- | --- |
| ws141-codex-20261010-i16 | p006 software静的レビュー・具体的欠陥の改善 | cleared（software静的レビュー部分） | 統合済みsource e100aa1ce。compiler/ownership/初期化/scanoutのsoftware契約を照合、見つけた機能欠陥を修正し短い対象確認を行う |
| ws141-codex-20261010-i17 | p008共通compiler抽出・i915/VC4参照の移行 | cleared | i16出力。SPIR-V parser/IRをGPU共通へ移しdrv_gpu_型/関数・DRV_GPU_定数に改名。Gen12 EU/V3D QPU backendは各driver。IR数値/配置/compile動作とdiagnostic/ownershipを保持 |
| ws141-codex-20261010-i18 | p007再監査・target build/host・main統合 | in-progress | i16/i17最終source。全文C/境界/licenseレビュー、RPi4 y/n・amd64 i915有効build warning0、parser/EU/QPUの短いhost確認、final ARM64 stack再評価、最新main統合/readback |

依存: i16 → i17 → i18。実機は外部contextでユーザー担当、whole WSのcleared/completed条件を満たす証拠ではない。今回source変更で旧p007 final検証の適用は失効し、p007をunclearedから今回attemptへ再開する。終了時は実機用image/source/hashと未確認項目を記録し、WS incompleteを保持する。

## 設計

`src/drivers/gpu/compiler/{ir.h,spirv.h,spirv.c,spirv-compute.inc,spirv-geometry.inc}` を共通frontendとする。`spirv.h`は診断・parse/free宣言を所有し、i915 compiler.hはこれをincludeしてEU binary/compile APIだけを所有する。BCM shader/private/pipelineは共通headerを直接includeする。共通IRのoperation/stage/resource/builtin定数の数値は変更せず、backend側で現在のhardware encodingを解釈する。Compute/geometry parserの既存範囲・128 invocation上限も維持する。Vulkan wire codecは今回のshader共通化範囲外。

## 規則・検証境界

[全文C](../coding-style.md)・[Guardrail](../guardrail.md)を読了。簡約版なし。移動でも全文規約をレビューし、新実装には全規則を適用。既存Zlib sourceの移動なので外部code導入なし。clang-format19は変更範囲だけに適用し、meaningful host確認とnamed target buildを行う。共有LLVMはread-only symlinkを使用。QEMU/実機は走らせず、T1/Q1回帰とremote公開は担当へ残す。旧実行・監査の履歴は保持する。

## 結果・handoff

進行中。

## i16静的レビューと改善

BCMのscanout準備→firmware停止→一回限りrestart、unique boot出力先/登録拒否、V3D launch前span確認・DMA未retire時のquarantineを再読し、既存software契約の矛盾は見つからなかった。common parserは大きなdecode stateをkernel stackに置いているため今回heapへ移す。stateの全ENOMEM/parse拒否/成功の解放と、APIのNULL stream/32bit word-count表現を確認し短いhostへ含める。実機register/cache/画面は未確認。

## i17中間確認

共通frontendへ4 fileを移しspirv.hを追加。全backend/caller/現役hostを新型/定数/APIへ移行、旧aliasは無し。parser stateをheapへ移し全部分allocation/refusal/freeを確認。i915 spirv/lower/compile host（ordinary/ASan/UBSan）PASS、VC4 actual Keiland/native scalar/深度/全allocation refusal host PASS。sandbox内LSanはptrace制約で未判定だったため、同じ対象を通常環境で再実行してPASS。移動したparserの既存条件内call/直接call return/Boolean/ternary/不足paragraphとvoid終端を全文Cに合わせて修正、IR値/ABIは保持。これは中間source、final build/stack/i18は未実施。

## i17 software clearance・i18最終検証

p008/i17はsoftware criteriaを満たしてcleared。共通frontend5 file、i915 EU/VC4 QPU backend caller、両platform source list、現役WS031/068/075/101/141試験の参照を移行した。旧i915 parser/IR/API/型/定数alias・VC4のi915 compiler header includeは検索0。共通sourceにGen12/V3D hardware依存includeなし。Vulkan wire codecのi915配置は今回shader scope外で保持。IR全tokenを旧定義と比較しnamespace/guard/comment以外は一致、全enum/macro数値・field型/順序を保持。Compute/geometry profileと128 invocation admissionも維持する。

最終規約: 全文C/Guardrailを参照して共通parserのpublic/static順、167 static prototype、ANSI宣言、条件内call/複合条件/Boolean/ternary、split call/body、段落/return、全allocation/unwindをレビュー。直接call return309・条件内call11・Boolean/ternary・不足段落99・void終端を修正した。i915のliveness source/result countは純粋な分類をloop前に読み、blend/format照合は元の短絡順を保持して分解。formatter19を共通moduleと変更caller関数に適用し、定義引数/一行prototype/comment/clauseの全文規約を再調整した。style checkerは共通Cと変更callerで0、include fragment単体の27 forward-declaration指摘は親spirv.cに実宣言があり、結合167 functionとの照合PASS（未修正違反として隠さない）。その他vendor実装は共通名への機械置換をレビューし、hardware encodings/compile API/数値を変更していない。新external code/firmware/BLOB導入なし、既存Zlib frontend/IRを移動して同licenseを維持。旧i15のGPL/hardware/license監査は履歴として保持する。

Commands/results（own worktree、共有LLVM23.1.0とNoctはread-only symlink）:

- `make -j16 ZEDBSD_CONFIG=config/ci/config-rpi4.mk BUILD=build/ws141-rpi4-y CONFIG_DRIVER_BCM2711_GPU=y vmunix`: exit0、warning/error0、ARM64 checks3。log `build/ws141-refactor-rpi4-y.log`。
- 同config `BUILD=build/ws141-rpi4-n CONFIG_DRIVER_BCM2711_GPU=n vmunix`: exit0/up-to-date。旧n image hash不変、今回共通parserはn targetのcompile/link入力に含まれない。
- `make -j16 ZEDBSD_CONFIG=config/ci/config-amd64.mk BUILD=build/ws141-refactor-amd64 CONFIG_DRIVER_PCI_I915=y vmunix`: 最終exit0、warning/error0、include/vmunix checks2。log `build/ws141-refactor-amd64-final2.log`。最初のlink後checkはown Noct参照が無く停止し、共有Noct read-only symlink準備後再build PASS。toolchainをbuild/install/改変していない。
- `sh plan/ws031/tests/run-vk-host-tests.sh 'spirv lower compile'`: final log `build/ws141-refactor-i915-final.log`、ordinary/ASan/UBSan全3群PASS。i915 callerの最後の規約修正後 `... 'compile pipe'`: log `build/ws141-refactor-i915-caller-final.log`、ordinary/ASan/UBSan全2群PASS。新NULL/負stage/32bit長overflowのguardもactual parserで確認。sandbox LSanのptrace制約は通常実行環境で解消しPASS。
- `sh plan/ws141/tests/shader-host-test.sh build/ws141-refactor-shader`: fixed MIT Mesa oracle、actual Keiland/native scalar/深度/再定義/全frontend-native allocation refusal、3 PASS。log `build/ws141-refactor-shader.log`。
- `sh plan/ws141/tests/vulkan-device-host-test.sh build/ws141-refactor-vulkan`: actual client wire/pipeline/primary/record/ownership、26 PASS。log `build/ws141-refactor-vulkan.log`。
- `python3 plan/ws141/tests/filter-coefficients-check.py`: 独自rational coefficient確認PASS。途中の誤ったsh呼び出しは確認結果に含めず、正しいPython起動で再確認した。
- `build/llvm/bin/llvm-objdump --no-show-raw-insn -d build/ws141-rpi4-y/kernel.elf > build/ws141-refactor-disassembly.txt`; `python3 plan/ws141/tests/stack-audit.py build/ws141-refactor-disassembly.txt build/ws141-refactor-stack.json`: PASS。ordinary worker12656 / COMMAND15680 / SUBMIT12432 / capacity16384、margin704。旧15984/margin400から改善。callback/finite recursion/outer ioctl/one IRQという既存モデルの範囲を維持、terminal invariant診断/別config/実機の証明ではない。[最終stack](compiler-refactor-stack.json)。
- 対象Pythonのpy_compile、移行したshellの構文確認、IR token互換、旧名前/依存検索、`git diff --check` PASS。旧source inventory183pathはi15当時の履歴、新sourceは[refactor台帳](compiler-refactor-source-sha256.tsv)。

Artifact SHA-256:

| artifact | SHA-256 |
| --- | --- |
| RPi4 y vmunix | d6a96a28130a171e6f3371f32a3e24da5134a6c6c95e82264bbd02279c2125bd |
| RPi4 y ELF | a983a401b7c63384e507875e9d1e182a2e716663bd06a23baebcd034c5c22670 |
| RPi4 n vmunix | d20ca5d4b340b2e37ee7c4353df0a5c8f1ce054c8567761dbe8a51461595ab10 |
| amd64 i915 y vmunix | a0da49693a08a690fbb8279e3623a746cb75df052cb5f56aa8e668ae01132c4e |

i18はmain統合待ち。今回hostのMMIO/scheduler/native-runner fixture境界は既存どおり、actual ioctl/実GPU/Keiland画面のPASSを主張しない。ユーザー担当のRPi4 P0/N0/R0/P1/P2/P3/P5/V1〜V10・console RAM寿命・Keiland画面/操作、およびQ1/T1のkernel/desktop回帰は未実施。WS incompleteを維持。共有Master/Queue/Guardrail/GitHub投影はQ1へpending、外部公開/pushなし。
