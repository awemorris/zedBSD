# WS141 独立Codexセッションの実行記録

- Cycle ID: ws141-codex-20261009
- Status: finished（i01〜i04の部分範囲の確認を終了。WS・whole Phaseは未完了）
- 承認: 2026-10-09、このchatのユーザーがWS141を担当に割当。原文と所有範囲は [ws.md](ws.md#独立セッションの担当2026-10-09)。共有Queueの採番・更新はQ1。
- 検証範囲: buildと短いhost試験。QEMUはQ1経由T1、実機はユーザー（後で実施）。
- 実装の判断・licenseの決定: [既存design](rpi4-gpu-design.md) §9、2026-10-04の項目1〜17の承認を保持。新しいHAL API差分は事前承認のまま。

| Attempt | Phase / 部分範囲 | 状態 | 条件・依存 |
| --- | --- | --- | --- |
| ws141-codex-20261009-i01 | p002骨格とp003/N0の現行ツリーとの整合・再build | cleared（部分範囲のみ） | rpi4 driver y/nのkernel build、stage/list host試験。既存の範囲内で必要な整合修正。whole Phaseの実機条件を免除しない |
| ws141-codex-20261009-i02 | p001/p002の作業資料の復旧 | cleared（部分範囲のみ） | 固定sourceのhash・改名表を確認。詳細は下記 |
| ws141-codex-20261009-i03 | p003/N1の配置計算とraw word複写の準備 | cleared（部分範囲のみ） | 起動経路へ組み込まない純粋な処理。snapshot・使用中list・filter等の予約範囲を受け取り、衝突しない連続領域と同一word列を生成。host PASS・y/n build warning/error 0。N1のhardware書き込み・切り替え・実機受け入れは対象外 |
| ws141-codex-20261009-i04 | p004/V5の4 KiBページ表の生成・解除 | cleared（部分範囲のみ） | V0骨格と固定sourceのPTE形式を依存出力として使う純粋な処理。予約VA page 0、VA/PA範囲、既存mappingを確認して全体を更新。host PASS・y/n build warning/error 0。電源・register・cache/TLB操作・起動への統合は対象外 |

## 継続の承認とi03の境界（2026-10-09）

- ユーザー「Q1でのマージは遅らせます。続きをお願いします。」に基づき、このworktree内で未マージの変更を積み上げる。統合は引き続きQ1。
- p002の骨格とp003/N0のbuild可能な既存source、回収済み改名資料を準備処理の依存出力として使う。実機P0/V0/N0のclearanceを成立したとは扱わない。
- 準備処理の呼び手は、全channelのcurrent/next listとfilter等の使用範囲を実機観測から供給する責務を持つ。空きを推測する起動コードは追加しない。
- Full standard: `plan/coding-style.md`全文、GuardrailのWS141 license/HAL/build/scanoutの規則、`plan/standards/automation.md`。全文review、clang-format-19とstyle-checkを補助にして確認する。HAL API変更なし。

## 後続の再開条件

- p003/N1: 実機N0の写真でHVSのlist位置・範囲・plane数を確認。
- p003/N2以降: framebufferの寿命・引き継ぎの観測に応じて進める。HAL APIが必要なら差分を用意して判断を求める。
- p004: V5のsoftwareページ表の準備はi04で実装。実機V0確認後にV1へ進み、表の確保・cache/TLB・hardwareへの設定とjobの寿命管理を統合する。
- p005〜p007: 既存のPhase範囲と依存を保持。今回の部分attemptでは実行しない。

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

## i03/i04の結果（2026-10-09）

- p003/N1準備: `bcm2711_list_copy_prepare`をlist.cへ追加。安定した全SRAM snapshotと全channelのcurrent/next list・filter等の予約範囲を呼び手から受け取り、source自身と終端を保護して最初の連続した空きを選ぶ。順不同・重複の予約も処理し、context・scaling word・8 planeを超える残りのplane・終端の全bitをそのままimageへ複写する。失敗時はimageを変更せずcopy.wordsを0にする。MMIO書き込みは無い。
- p004/V5準備: mmu.cのmap/unmapは4 KiBの連続buffer用PTEだけを編集。page 0を予約、32-bit VAと24-bit PFNの端・alignment・全slotの状態を事前確認し、範囲の一部だけ変更した失敗を残さない。実行中jobとの排他、物理bufferの所有、4 MiB表の確保、clean/barrier/TLB flushは後続の呼び手の責務。
- `sh plan/ws141/tests/stage-host-test.sh build/ws141-host-i04` → stage/list/list-copy/mmuの4試験PASS。GCC 14.2.0、`-Wall -Wextra -Werror`。list-copyはraw word一致、source SRAM不変、filterと他listの回避、exact fit・容量不足・不正範囲・SRAM枯渇。mmuはliteral PTE、最終VA/PA page、後方collision/holeの時の前方slot保持、不正span・larger-page解除の拒否を確認。
- rpi4 driver y/n: i01と同じ`make -j2 ZEDBSD_CONFIG=config/ci/config-rpi4.mk BUILD=build/ws141-rpi4-{y,n} CONFIG_DRIVER_BCM2711_GPU={y,n} vmunix` → i03/i04ともexit 0、warning/error 0。i04ではmmu.cのobjectをcompile・link対象に追加。ログは`build/ws141-baseline/kernel-{y,n}-i04.log`。
- 最終sourceのreturn経路review後にも同じdriver y buildを実行しexit 0・warning/error 0（`kernel-y-final.log`）。`sh plan/ws141/tests/stage-host-test.sh build/ws141-host-final`の4試験もPASS、style-check total 0、shell構文確認とdiff-checkも0。driver n側は当該sourceを含まず、i04のbuildで確認済み。
- 起動から新helperを呼んでいないため、linkerの未使用section除去後のvmunixのSHA256はi01と同じ（y `0fb0caf5095188e3911aec3a5382a7339a21929f6e6fe67841308df43b4e6774`、n `e7446d4f070cc09d1a41c79c3e8d00d0343d33290af8a3f759db8b94e9013e26`）。kernelの動作が変わったとの主張はしない。新helperのsoftware動作の証拠はhost試験。
- full C標準の目視review: 公開/static順・forward宣言・ANSI宣言・段落・条件/return・所有/寿命・半開区間/算術の境界を確認。clang-format-19 19.1.7で新Cとlist.cをformatし、定義引数のtab・split callの1引数1行を全文規約に戻し、無関係な旧macro整形を復元。`python3 plan/tools/style-check.py src/drivers/gpu/bcm2711/list.c src/drivers/gpu/bcm2711/mmu.c plan/ws141/tests/list-copy-host-test.c plan/ws141/tests/mmu-host-test.c --summary` → total 0。`git diff --check` → 0。
- 630定数の旧名の語単位照合とtempがignoredであることを再確認。新実装は作業文書の制御構造やcommentを転記せず、独立したrawコピー/区間探索と全slotの事前確認として記述。最終license/類似監査p007は未実施。
- 新host試験は未完了p003/p004の開発と次の統合に使う。WS完了時の恒久回帰化/削除の判断はQ1の試験整理規則に従う。Masterのtool登録は担当では変更しない。
- 未実施: N0のQEMU回帰、P0/V0/N0/N1等の実機観測、N1のwrite/readback/pollとN2通知、V1の電源とV5のhardware設定、job実行、p005〜p007。whole Phase/WSのclearance・完了は行わない。
- 統合・共有Master/Queue投影・T1への依頼・GitHub公開はQ1の担当として保留。今回のchatでQ1へのメッセージ送信やmerge/pushは行っていない。
