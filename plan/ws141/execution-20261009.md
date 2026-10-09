# WS141 独立Codexセッションの実行記録

- Cycle ID: ws141-codex-20261009
- Status: finished（最初の部分範囲の確認を終了。WS・whole Phaseは未完了）
- 承認: 2026-10-09、このchatのユーザーがWS141を担当に割当。原文と所有範囲は [ws.md](ws.md#独立セッションの担当2026-10-09)。共有Queueの採番・更新はQ1。
- 検証範囲: buildと短いhost試験。QEMUはQ1経由T1、実機はユーザー（後で実施）。
- 実装の判断・licenseの決定: [既存design](rpi4-gpu-design.md) §9、2026-10-04の項目1〜17の承認を保持。新しいHAL API差分は事前承認のまま。

| Attempt | Phase / 部分範囲 | 状態 | 条件・依存 |
| --- | --- | --- | --- |
| ws141-codex-20261009-i01 | p002骨格とp003/N0の現行ツリーとの整合・再build | cleared（部分範囲のみ） | rpi4 driver y/nのkernel build、stage/list host試験。既存の範囲内で必要な整合修正。whole Phaseの実機条件を免除しない |

## 後続の再開条件

- p003/N1: 実機N0の写真でHVSのlist位置・範囲・plane数を確認。
- p003/N2以降: framebufferの寿命・引き継ぎの観測に応じて進める。HAL APIが必要なら差分を用意して判断を求める。
- p004〜p007: 既存のPhase範囲と依存を保持。まだこの最初のattemptでは実行しない。

## 検証結果

- driver `y`: `make -j2 ZEDBSD_CONFIG=config/ci/config-rpi4.mk BUILD=build/ws141-rpi4-y CONFIG_DRIVER_BCM2711_GPU=y vmunix` → exit 0、warning/error 0、20.7秒。`build/ws141-rpi4-y/vmunix` SHA256 `0fb0caf5095188e3911aec3a5382a7339a21929f6e6fe67841308df43b4e6774`。
- driver `n`: `make -j2 ZEDBSD_CONFIG=config/ci/config-rpi4.mk BUILD=build/ws141-rpi4-n CONFIG_DRIVER_BCM2711_GPU=n vmunix` → exit 0、warning/error 0、18.6秒。`build/ws141-rpi4-n/vmunix` SHA256 `e7446d4f070cc09d1a41c79c3e8d00d0343d33290af8a3f759db8b94e9013e26`。
- `sh plan/ws141/tests/stage-host-test.sh build/ws141-host` → stage-host-test / list-host-test ともPASS（GCC 14.2.0、`-Wall -Wextra -Werror`）。
- target compiler: 共有のproject LLVM clang 23.1.0（d7f1bbaca898fb5f4cc373b082e915ec1a07310f）。kernel buildのみ。toolchainのsource・出力は変更なし。
- このattemptではsource修正なし。2026-10-04の骨格・N0の既存実装が開始treeでbuildできることを確認した。
- QEMU・実機・GPUの動作確認は未実施。T1-092の過去のPASSは骨格の版であり、N0の版の確認を代替しない。
- 以前の`plan/ws141/temp/`（GPLの作業の文書・630定数のrename-map）は開始treeに存在しない。現在のsourceを新たに書く前に、旧作業資料を回収するか、正本と既存の監査記録から再構成する必要がある。今回、旧名の照合は未実施。
- p001のQ1判定、p002の実機P0/V0、p003のN0回帰・実機、N1以降は残件。今回のbuild/host PASSだけでwhole Phaseをclearedにしない。


## ws141-codex-20261009-i02: 作業資料の復旧（2026-10-09）

- ユーザーが分担を再確認: この担当はWS141だけを専用ディレクトリで作業、ws.mdを排他的に更新、Masterを更新しない。パッチの提供先はQ1、統合もQ1。
- 範囲: p001/p002の既存作業資料と参照sourceの復旧・整合確認。新規GPU sourceの生成は無し。
- 状態: cleared（復旧という部分範囲のみ）。whole PhaseのQ1判定・実機条件は残る。
- 旧P2 worktreeの`plan/ws141/temp/`に資料が残っていた。元のcacheは読み取りだけにして、このworktreeのignored `plan/ws141/temp/`へ136 fileを複写。
- 3つのGPL由来の作業文書、独自名へ変換した3文書、630定数の対応表、変換用scriptを回収。Linux/Mesaの監査対象121 fileのSHA256は、既存の`rpi4-gpu-license-audit.md`の値と全て一致。
- 正本commitはLinux `05f7e89ab9731565d8a62e3b5d1ec206485eeb0b`、Mesa `06f9e28304d5d3f109c33535c1c25b9df5769af2`で一致。各cacheは監査対象fileのsnapshotであり、git cloneを新規に作ったものではない。
- 対応表の630のhardware定数は全て旧名と新名が異なる。現行`src/drivers/gpu/bcm2711/`のC/headerへ旧名を語単位で照合し0件。
- `git check-ignore`で原文・改名表がignoredであることを確認。GPL source・作業文書・旧名の対応表はコミットに含めない。
- 詳細な複写元・個別hashのreceiptは`temp/recovery-20261009.json`（ignored）。後日復旧するときは旧P2 cacheまたは監査記録の固定commit/hashを使う。元cacheは変更しない。
- 初回i01時点の「資料回収が必要」は、このi02で解消。実機確認は依然後で行う。N1は実機N0の観測後。
