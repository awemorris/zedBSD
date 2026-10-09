# WS141 独立Codexセッションの実行記録

- Cycle ID: ws141-codex-20261009
- Status: active（2026-10-09ユーザーが完成までの自走を指示、Vulkan実行器/compilerもWS141へ追加）
- 承認: 2026-10-09、このchatのユーザーがWS141を担当に割当。原文と所有範囲は [ws.md](ws.md#独立セッションの担当2026-10-09)。共有Queueの採番・更新はQ1。
- 検証範囲: buildと短いhost試験。QEMUはQ1経由T1、実機はユーザー（後で実施）。
- 実装の判断・licenseの決定: [既存design](rpi4-gpu-design.md) §9、2026-10-04の項目1〜17の承認を保持。新しいHAL API差分は事前承認のまま。

| Attempt | Phase / 部分範囲 | 状態 | 条件・依存 |
| --- | --- | --- | --- |
| ws141-codex-20261009-i01 | p002骨格とp003/N0の現行ツリーとの整合・再build | cleared（部分範囲のみ） | rpi4 driver y/nのkernel build、stage/list host試験。既存の範囲内で必要な整合修正。whole Phaseの実機条件を免除しない |
| ws141-codex-20261009-i02 | p001/p002の作業資料の復旧 | cleared（部分範囲のみ） | 固定sourceのhash・改名表を確認。詳細は下記 |
| ws141-codex-20261009-i03 | p003/N1の配置計算とraw word複写の準備 | cleared（部分範囲のみ） | 起動経路へ組み込まない純粋な処理。snapshot・使用中list・filter等の予約範囲を受け取り、衝突しない連続領域と同一word列を生成。host PASS・y/n build warning/error 0。N1のhardware書き込み・切り替え・実機受け入れは対象外 |
| ws141-codex-20261009-i04 | p004/V5の4 KiBページ表の生成・解除 | cleared（部分範囲のみ） | V0骨格と固定sourceのPTE形式を依存出力として使う純粋な処理。予約VA page 0、VA/PA範囲、既存mappingを確認して全体を更新。host PASS・y/n build warning/error 0。電源・register・cache/TLB操作・起動への統合は対象外 |
| ws141-codex-20261009-i05 | ユーザー承認による最新mainとの統合 | cleared（統合範囲のみ） | 最新main基点の専用worktreeでmerge、host4試験PASS・rpi4 y/n build warning/error 0。その後共有mainへ取り込み。whole Phase/実機の条件は保持 |
| ws141-codex-20261009-i06 | p004/V7の1×1 noop command list生成 | cleared（software生成のみ） | BCL/RCL/tile sub-listを固定4.2 XMLと独立に照合しPASS。容量・VA・領域の重なり・失敗時のbyte保持を確認。rpi4 y/n build warning/error 0。GPUへの投入・起動への追加・実機clearanceは対象外 |
| ws141-codex-20261009-i07 | V7生成の最新mainとの統合 | cleared（統合のみ） | 最新mainとの統合版でnoop/XMLと既存4host試験PASS、rpi4 y/n build warning/error 0。merge 16024f1b9を共有mainへ取り込み済み。Master更新・実機clearance・pushは対象外 |
| ws141-codex-20261009-i08 | p003: Linuxと同じ再初期化への設計変更・VC4初期化〜初回scanoutの実装照合/修正 | uncleared（WS048限定修正の適用判断待ち） | 今回のユーザー指示と追加回答を下に保存。出力先はboot framebufferを実際に表示するHDMI、範囲は既存firmware mode。build/hostで確認、実機受け入れは後で実施 |
| ws141-codex-20261009-i09 | p003: 承認済みmailbox容量0修正の適用と初期scanout成果の最新main統合 | cleared（software/統合範囲のみ） | 2026-10-09ユーザー「mainにマージしてOKです。mailbox修正も承認します。」。提案の3 pathを適用、実mailbox hostとrpi4 y/n build、独立統合版確認後にmainへmerge。実機は後で実施 |
| ws141-codex-20261009-i10 | p003/P1/P2部分: vblank sequence・inactive SRAMへの同期flip・console復帰/timeout時のbuffer保持 | cleared（software/統合部分範囲のみ） | 2026-10-09ユーザー「では続けてください。」。R0のbuild/host検証済み出力を使うsoftware/runtime部品。caller所有の連続RGB32 bufferを受け取る。allocator・P3合成・device登録・起動からのflipは対象外、実機受け入れは保持 |
| ws141-codex-20261009-i11 | p003/P2/P3/P5: buffer owner・合成・display ops/登録の完成 | cleared（software/統合部分） | 最新継続指示。既存R0/flipの実sourceとhost/build出力を使い、実機の受け入れを別に保持 |
| ws141-codex-20261009-i12 | p004/V1〜V10: 電源/MMU/cache/IRQ/job/reset統合とhost/build | cleared（software部分） | V0の発見骨格・MMU/noop generatorを使用。実機未実施を保持 |
| ws141-codex-20261009-i13 | p005: 二deviceのresource共有・GPU API統合 | in-progress | i11/i12の必要な実source出力を確認後 |
| ws141-codex-20261009-i14 | p006拡張: kernel Vulkan実行器とV3D SPIR-V compiler・Keiland描画経路 | in-progress（検証済みscoped owner/worker出力を使用） | ユーザーが本WSへ含めると明示。i12/i13のjob/resourceを使う |
| ws141-codex-20261009-i15 | p007: 最終changed source全規約/license/類似監査とbuild・統合 | pending | i11〜i14の最終成果、公開HAL API具体差分の事前承認を維持 |

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

## i06の継続範囲（2026-10-09）

- ユーザー「では、続きをお願いします。」に基づき、統合済み6f912a7a4からp004/V7のsoftware生成を進める。p002/V0の実機結果はまだ無いため、V1のhardware操作やV7の実投入を起動経路へ追加しない。
- 固定Mesa 25.3.6のMIT `v3dvx_queue.c`/`v3dvx_cmd_buffer.c`からnoopに必要なpacketを確認し、承認済み`cle/v3d_packet.xml`の4.2形式で独立したbyte serializerを記述。GPL原文/旧名の構造は使わない。XML hashを確認するhost oracleでpacketごとのfieldと長さを照合する。
- 出力は1 layer、1 render target、1×1、32 bpp、MSAA無し。framebufferへのstoreは無し。tile list poolと3つのCLは別のGPU VA区間とし、bufferの確保・MMU mapping・cache操作・bin→render順の実投入は後続のjob所有者の責務。
- Full C標準・Guardrail・既存の最終監査p007を保持。source追加は当該driverのcl.cとarm64の当該source列のみ。Master・公開API・HAL・toolchainの変更は無い。

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

## i05: mainへの統合（2026-10-09）

- 追加承認: ユーザー「パッチの影響範囲が狭いので、あなたがマージしてOKです。」。i01〜i04の成果について、当初のQ1のみのmerge担当とマージ延期をこの指示で置き換える。Master/共有Queue等の投影・GitHub公開・T1依頼の担当範囲は変更なし。
- main開始点`3cca433174fbed822013a5f32f5cfb63c5352fe0`はclean。WS141以外の人間・他セッションのcommitを保持し、`codex/ws141-integrate`の専用worktreeへWS141 branch `25150d4c7`をmerge。競合なし、merge commit `a326c5e246a12e2f6963b89f61d3e76b7cdc42a6`（message WIP）。
- 専用worktree: `/home/awe/zedBSD-claude1/.claude/worktrees/ws141-integrate`。共有LLVMを読み取り専用symlinkで参照。共有buildは変更せず、このworktreeのbuildへ出力。
- `sh plan/ws141/tests/stage-host-test.sh build/ws141-host-integration` → stage/list/list-copy/mmuの4試験PASS。
- `make -j2 ZEDBSD_CONFIG=config/ci/config-rpi4.mk BUILD=build/ws141-rpi4-y CONFIG_DRIVER_BCM2711_GPU=y vmunix`と、BUILD末尾n・driver=nの同target → 両方exit 0・warning/error 0。ログは専用worktreeの`build/ws141-integration/kernel-{y,n}.log`、summary.jsonへhashも保存。vmunixのhashはi04と同一。
- `git diff --check HEAD^1 HEAD` → 0。統合の変更はWS141記録/host試験、当該driver、arm64の当該source列の13 pathのみ。HAL API・Master・Queueは変更なし。
- 検証後、共有mainで`git merge --ff-only codex/ws141-integrate`を実行し、HEADが上記merge commitであることを読み返して確認。ローカル統合完了。push/GitHub公開・QEMU/実機試験は未実施。N1/V5のhardware統合やwhole Phase clearanceを意味しない。
- 以前の累積パッチは統合済み成果の保存用となり、共有mainへ再適用しない。以後の変更はこのmergeを含むmainから積み上げる。

## i06の結果（2026-10-09）

- `cl.c`とprivate型/宣言、arm64の当該source列を追加。1×1、1 layer、1 target、MSAA無しのbin 14 byte・render 56 byte・generic tile list 19 byteを生成。renderのpool/start/endを明示のlittle-endian byte storeで埋める。shader無し、NONE storeなのでoutput framebufferは無い。MMIOアクセス・メモリ確保・起動への追加は無し。
- 3つのCLのCPU storageは独立したcaller-owned bufferで、GPU VAは容量全体とtile poolの予約を半開区間で確認する。GPU page 0、32-bit packet終端、64-byte pool alignment、1 tileのpool最小0x83000 byteを検査。全ての検査の後に生成し、失敗時は全used lengthを0にしてbyteを保持する。buffer保持、MMU mapping、cache clean、tile state 256 byteの確保、bin→renderの投入/IRQは後続job所有者の責務。
- `sh plan/ws141/tests/noop-host-test.sh build/ws141-noop-i06-final` → noop-host-test PASS・noop-packet-check PASS。Oracleは監査済みXMLのSHA256 `b13f995b1a4b606a1adcb2c048ff9bca067106be14e22ce1ee86e01055094e1b`を確認し、4.2のcode/field位置/default/minus-oneから期待バイト列を独立に生成。3つの列全体と一致。C試験は容量・終端overflow・零page・pool alignment/大きさ/overflow・未使用capacityでの重なり・poolとCLの衝突・CPU storage無し・隣接区間を確認し、全拒否で3つのbuffer不変とused=0を確認。
- 既存stage/list/list-copy/mmuの4試験PASS。host GCC 14.2.0、`-Wall -Wextra -Werror`。
- rpi4 kernelは同じdriver y/nのnamed `vmunix` targetでexit 0・warning/error 0。共有LLVMは読み取りだけ。ログ`build/ws141-baseline/kernel-{y,n}-i06.log`。起動から新helperを呼ばないためvmunix hashはi05と同じ。hardware jobが動く証拠にはしない。
- C全文の目視review（declaration/公開-static順/forward/段落/条件/return/byteの境界/所有・寿命）、clang-format-19 19.1.7と定義引数tab/packet表の復元、補助style-check total 0、shell/Python構文確認、git diff --check 0。630定数の旧名の語単位一致0。原文・XML・改名表はignored temp、参照codeのcopy・外部objectのlinkは無し。license/全source類似の最終監査はp007で残る。
- OracleのXMLはignored cacheにある。別のworktreeで確認する場合はrunnerの第2引数へこの担当の固定XMLの絶対pathを渡せる。hashの違うXMLは拒否する。再cloneの入手先/commit/hashは既存license監査の表を使用し、GPL対応表をパッチへ入れない。
- 次のsoftware段はV8のclear/store。実投入の前にはV0の実機観測とV1〜V6（電源・識別・表/cache/TLB・IRQ）、buffer所有と完了の順の統合が必要。p003/N1の実機N0観測も引き続き待つ。whole Phase/WSは未完了。
- 最終のcomment/format復元後にnoop-host/oracle/style-checkとdriver y buildを再確認し全てPASS・warning/error 0（kernel-y-i06-final.log）。driver nはcl.cを含まずi06の確認を保持。

## i07の統合結果（2026-10-09）

- 前turnの「パッチの影響範囲が狭いので、あなたがマージしてOKです。」と今回の継続指示を保持し、同じWS141のdriver・private header・当該build source列・WS記録/host試験の10 pathだけを統合。
- 実装commit `d93d4507e`を最新main `d0be6aa267ea3d74f8aa5847f6d5152e7c996e4c`基点へmergeし、競合無し。merge commit `16024f1b967402e1e020f097c8be3563e5c89059`（WIP）。専用統合worktreeで再確認後、共有mainへfast-forwardしHEADを読み返して確認。
- 統合版: noop-host/oracleと既存stage/list/list-copy/mmuの確認が全てPASS。XMLはrunner第2引数から担当worktreeの固定snapshotを読み取る。rpi4 driver y/nのkernel buildもexit 0・warning/error 0。ログ`build/ws141-integration/kernel-{y,n}-i07.log`と`i07-summary.json`、diff-check 0。
- Master・共有Queue・HAL API・toolchainは変更無し。push/GitHub公開/QEMU/実機は未実施。whole Phase/WSは未完了のまま。
- V8の次の実装で注意する点も固定MIT sourceを確認: clear値の設定だけでなく、初期tile bufferを準備する2回のdummy tile（NONE store、最初にCLEAR、最後にVCD cache flush）の段がある。V7のnoopをそのままcolor storeへ置換してclear済みとは扱わない。詳細は次のp004 software準備で展開し、実機のbuffer観測を受け入れに残す。

## i08の承認・設計変更（2026-10-09）

- ユーザー原文:「初期化の手順について、Linuxドライバと寸分違わず同じ手順になっているか、チェックして修正してください。VC4の初期化と、scanoutの開始まで。」
- 旧方針との差を説明し、ユーザー回答「Linuxと同じ再初期化へ変更する」を取得。途中の画面消失を許容し、旧designの項目7（画面を消さないコピー引き継ぎ）、8（コピー後の通知）、9（P4後回し）、15（underrun後のclock引上げ）を今回の表示再初期化について置換。旧履歴は保存する。
- 新しい処理はLinux v6.19固定commitの実際のhardware side effect、clock/reset provider、commit/encoder hookの呼び出し順で照合する。OSのDRM登録・allocator/clock参照管理をコピーせず、zedBSDの所有と独立した実装に対応させる。GPL由来の詳細trace/作業文書はignored tempだけ、hardware値の独自命名とZlib/最終監査の既存条件を保持。
- 元のソースは発見/P0・readout/N0のみで、自前のscanout開始は未実装。N1コピーhelperも未使用。このattemptの目的は初期化開始から最初のscanoutまでの差を修正することで、既存helperのPASSをhardware達成と読み替えない。
- 出力先はboot framebufferとの一致で選択する。単なるHDMI0優先では別の出力を選びうる。firmware mode以外の選択、EDID/DDC拡張、他ポートの点灯、V3D/V8、resident displayの後続APIはこのattemptに含めない。
- 最初の対象は既存designのfirmware mode（1920x1080@60、RGB8 progressive）。他のfirmware modeを受け付けるには各形式/fieldの検証を通す必要があり、未対応の入力では破壊的な再初期化を始めない。
- WS048のmailboxはcapacity=0を拒否するため、Linuxと同じ値なしのdisplay終了通知が不可能。限定差分をproposed/firmware-empty-tag.diffへ用意し、この担当範囲外の修正適用をユーザーへ確認中。回答前に当該sourceを編集しない。
- 実機確認「後で行う」の決定を保持。コード実装/buildと実機での動作確認を区別し、p003/WS全体は未完了。Master/共有Queue/Guardrail/standard投影はQ1担当、今回編集しない。


## i08の結果と再開条件（2026-10-09）

- 起点: `40a3541356b1f2610ba691cf98b2fbc3cc1f23cd`。このattemptは実装・build・host確認まで保存したが、必須のmailbox依存が未適用のため**uncleared**。scanout成功・whole Phase clearance・WS完了・mainへの統合を主張しない。
- 差の確認: 旧N0はHDMI0を優先し、自前のscanout開始がなかった。boot framebufferと一致する稼働中list/PVを両portで調べ、唯一の出力を選択する。曖昧なmirrorはEBUSYで止める。core/HSMのclockが0なら対応registerへ進まない。
- 実装: `display-program.c/.h`が検証済みmode/planeから完全な操作列を作り、`display-start.c`が既存mode/AVI/PHYのrateを通知前に取得、`display-execute.c`が既存mailboxとordered MMIO/cacheを実行、`display-irq.c`がsourceをEOI前に退役させてlist43を採用したvblankで完了を確定。登録済みIRQ sourceが無いなら通知前に拒否し、実行中の失敗は後続操作を止め、lineをmaskする。起動経路へR0を接続した。`rpi4gpu.stop=R0`と旧`stop=N1`は通知前の停止として有効。
- 固定Linux v6.19 `05f7e89ab9731565d8a62e3b5d1ec206485eeb0b`の実際の呼び手・clock/reset providerまで照合。表示終了通知の位置、HVS shared buffer/IRQ、初回commitのcore増減、channel/PHY/PV/HDMI video/FIFO/vblankの前後関係を対応させた。PLL固定小数計算の係数2、HDMI1の物理lane map、PV FIFO閾値（HDMI0=238、HDMI1=32）、PV水平timingの2画素clock換算を確認。固定DVP reset providerはreset_usが無くENOTSUPPになるため、存在しないSW_RESET pulseは追加しない。詳細traceとGPL由来資料はignored tempのみ。
- 成功時のhardware処理順を対応させた範囲は既存firmware modeのprogressive RGB8、25 MHz以上340 MHz未満。interlace・deep colour・pixel repetition・YUV・SCDC高rate・新mode/EDID/DDCは未対応で、通知前に拒否。zedBSD登録/所有/clock providerのglue、既存sink modeのAVI再利用、失敗時の停止方針はLinuxの全driverと同一ではない。「寸分違わず同じ」や全mode対応は主張しない。
- framebufferは既存HAL consoleが保持するものを使う。通知後にwidth/height/pitchを照合するが、同値でもRAMの寿命やfirmwareによる再使用を証明しない。実機でのRAM/画面/IRQの観測が残る。寿命の契約/API追加が必要なら具体HAL差分を先に提示する。
- `sh plan/ws141/tests/display-host-test.sh build/ws141-display-i08-release` → generator/実executorと実IRQ source serviceの2試験PASS。literal1080pの値/PLL ratio、両portの配線/FIFO/timing、VIDEOより先のVIDEN、setup core保持と採用後のrate低下、旧listによる誤完了拒否、HVS underrunのmask/W1C、通知拒否/geometry変化/clock failure/zero HSM/frame timeoutの停止境界を確認。modelは実firmware/GPU/電気的timingの成功証拠ではない。
- `sh plan/ws141/tests/stage-host-test.sh build/ws141-host-i08-final` → stage/list/list-copy/mmuの4試験PASS。新しいframebuffer一致判定は先頭以外のplaneも確認し、反転や異なるbufferを拒否。
- `make -j2 ZEDBSD_CONFIG=config/ci/config-rpi4.mk BUILD=build/ws141-rpi4-y CONFIG_DRIVER_BCM2711_GPU=y vmunix` → exit 0、warning/error 0、arm64 ELF/image checker PASS。同targetのBUILD=n・driver=nもexit 0、warning/error 0。共有LLVM 23.1.0はreadonly、GCC 14.2.0でhostを確認。ログ`build/ws141-init-i08/kernel-y-final-irq.log`・`kernel-n-final.log`と`summary.json`。
- vmunix SHA256: y `0adc41dfe97254a15852be7944401151055102a0832a0bb0dc458a02750a506f`、n `e7446d4f070cc09d1a41c79c3e8d00d0343d33290af8a3f759db8b94e9013e26`。今回は新起動処理がlinkされるためyは旧版と異なる。nはdriverを含まない。
- 全文C reviewは`plan/coding-style.md`を使用（簡約版無し）。宣言/公開-static順/forward/段落/条件/return/fieldの境界/所有・IRQ owner寿命を確認。clang-format-19 19.1.7（ColumnLimit 0）後、定義引数tabを復元。新source・list/fdt-util・新host2試験の補助style-check total 0。旧display/readout/v3dの短い対称なformat文字列の三項13件は既存のままで、全文規約の短い対称選択として目視確認。source/記録の`git diff --check -- . ":!*.diff"`、runnerの`sh -n`は0。提案diffのcontext行はpatch構文として空白+tab/空行prefixを保持するためwhitespace検査から除外し、`git apply --check plan/ws141/proposed/firmware-empty-tag.diff`を別に確認。630 hardware旧名の語単位一致0。原文/改名表はignored、BLOBの追加無し。全WSの最終license/設計類似/全文準拠p007は未実施。
- 必須依存: `drv_rpi4_firmware_property`は容量0をEINVALとして拒否する。**現在の実kernelではR0 op0でEINVALとなり、scanout再初期化のMMIO writeは0**。新host executor modelは通知を受理するmodelなので、この依存を検証済みとは扱わない。
- [proposed/firmware-empty-tag.diff](proposed/firmware-empty-tag.diff)はWS048 source/headerと既存host試験の必要な期待値/model修正を含む3 pathの限定提案。HAL APIは変更しない。ユーザーへ適用またはQ1への引渡しを確認済み、回答待ち。WS048の実source/header/testには未適用。
- 提案のみの確認: `build/ws141-init-i08/empty-tag-proposal/`にsource/testの作業コピーを置き、既存WS048 modelをコンパイルして通常/USB notificationと値なしの32 byte tag/answered=0を確認。`ASAN_OPTIONS=detect_leaks=0 .../firmware-host-test /home/awe/zedBSD-claude1/vendor/raspberrypi-firmware/boot/bcm2711-rpi-4-b.dtb` → 200163 checks PASS、ASan/UBSan。最初の実行はsandboxのptrace下でLeakSanitizerが終了時に使えず失敗したため、leak検出のみ無効化。これは提案の確認で、実source適用済みという証拠ではない。
- 再開: 限定修正の承認ならそのexact scopeを適用し、既存mailbox hostと対象buildを確認して新attemptへ保存。その後最新mainとの独立統合と検証。Q1への引渡しの回答なら依存をQ1へ残し、当該依存の統合を確認するまで表示開始成功扱いにしない。回答前のmain mergeは行わない。
- 実機はユーザーが後で実施、QEMUはQ1/T1経由。途中の画面消失は承認済みだが、実機IRQ・scanout・console buffer寿命・T1回帰、flip/合成/resident登録・V3D投入・p007は残る。Master/共有Queue/Guardrail/standards/他WS投影はQ1担当で未更新。push/外部連絡は無し。


## i09の追加承認（2026-10-09）

- 承認者/出典: このchatのユーザー、原文「mainにマージしてOKです。mailbox修正も承認します。」。
- 承認範囲: `proposed/firmware-empty-tag.diff`の実mailbox source/header/既存host試験の3 pathと、i08のWS141初期scanout成果のmain統合。HAL APIは変更しない。i08のuncleared履歴は保存する。
- 実sourceのmailbox host、display hostとrpi4 driver y/nのnamed kernel buildを確認し、最新mainとの専用worktreeで統合検証してから共有mainへ取り込む。実機/whole Phase・WS acceptanceは未達のまま。Master/共有Queue/Guardrail/他WSの記録投影とpushは対象外。


## i09: mailbox実sourceの確認（2026-10-09）

- 承認済み提案をそのまま`src/drivers/platform/rpi4/rpi4-firmware.c/.h`・`plan/ws048/tests/firmware-host-test.c`へ適用。capacity0/request_count0のtagはvalues=NULLを許可し、非emptyのstorage・上限・request_countの検査は保持。空tagのmodelがend markerを値で上書きしないよう既存host modelを修正。HAL API変更無し。
- `make -f plan/ws048/tests/host-test.mk OUT=build/ws141-init-i09/mailbox-host build/ws141-init-i09/mailbox-host/firmware-host-test` → 実mailbox clientのhost executableをbuild（ASan/UBSan）。`ASAN_OPTIONS=detect_leaks=0 build/ws141-init-i09/mailbox-host/firmware-host-test /home/awe/zedBSD-claude1/vendor/raspberrypi-firmware/boot/bcm2711-rpi-4-b.dtb` → 200163 checks PASS。値なし32 byte通知/answered0と通常tag/USB通知を確認。LSanは既知のsandbox ptrace制約により無効、実firmwareではない。
- `sh plan/ws141/tests/display-host-test.sh build/ws141-display-i09` → program/実executor+IRQ sourceの2試験PASS。
- `make -j2 ZEDBSD_CONFIG=config/ci/config-rpi4.mk BUILD=build/ws141-rpi4-y CONFIG_DRIVER_BCM2711_GPU=y vmunix`とBUILD=n・driver=nの同target → exit0、warning/error0、ELF/image checker各PASS。ログ`build/ws141-init-i09/kernel-{y,n}.log`。SHA256: y `defcfcaeb820f13b050ffaa1a9ecb1ad74144c2af36fa7d5dc4af1261127858c`、n `e7446d4f070cc09d1a41c79c3e8d00d0343d33290af8a3f759db8b94e9013e26`。
- mailbox変更箇所の全文規約/所有/null値を使用するloopの境界をreview、補助style-check source/header total0、git diff --check0。formatter形状は既存ANSI定義/paragraphを保持、無関係なformatはしない。
- i08で拒否された通知依存をsoftware上で解消。i08のuncleared履歴を遡って変更せず、このi09に結果を保存。最新main開始点`b224d174c150980a0cfe49f53ef5df71a7b455b5`はcleanで、i08起点以降の対象source/WS141には差分無し。次は専用worktreeで最新mainとのmerge・対象試験/buildを確認し、mainへ取り込み。
- WS048 p003/WS側のAPI拡張の記録投影はQ1へ残す（承認された他WS編集は提案の3 pathのみ）。WS048全体の再開や実機受け入れをこの依存修正で宣言しない。


## i09: mainへの統合結果（2026-10-09）

- 実装commit `819803b63036b0c174c22590087ef9ea17ce17bb`（message WIP）。i08の`733e9d16f`と合わせ、WS141 driver/当該arm64 source列/WS記録とhost、承認されたWS048のmailbox source/header/host3 pathだけを統合。
- 専用統合worktree `.claude/worktrees/ws141-integrate`を、実際の最新main `f9afbb573e7887557a7884d754a8a6a1c477c7cc`へ揃えてmerge。競合無し、merge `4bb7426b5aa5c7f911d30caf4c2532283fd96a67`（WIP）。開始時のb224d174c以降の他セッションの成果も保持した。
- 統合版の`sh plan/ws141/tests/display-host-test.sh build/ws141-integration-i09/display-host` → 2試験PASS。実mailboxは`make -f plan/ws048/tests/host-test.mk OUT=build/ws141-integration-i09/mailbox-host build/ws141-integration-i09/mailbox-host/firmware-host-test`と同じDTB/ASAN_OPTIONS=detect_leaks=0で実行 → 200163 checks PASS。前述LSanの制約は保持。
- 統合版のrpi4 driver y/n named vmunix target（i09実source確認と同じmake引数） → exit0、warning/error0、ELF/image checker各3 PASS。SHA256は実装branchと同じy `defcfcaeb820f13b050ffaa1a9ecb1ad74144c2af36fa7d5dc4af1261127858c`、n `e7446d4f070cc09d1a41c79c3e8d00d0343d33290af8a3f759db8b94e9013e26`。専用worktreeの`build/ws141-integration-i09/kernel-{y,n}.log`・`summary.json`へ保存。
- mailbox変更範囲をclang-format-19（ColumnLimit0、lines175:194）で確認し、出力はsourceとbyte一致。実source/headerのstyle-check total0、source/記録のdiff-check0（保存した提案diffのcontext構文は除外）。全WSのp007は残る。
- 検証中にmainが`b1972bf580150a3568c777f185fc0f871232c0b7`へ進んだため専用branchへ追加merge。変更はbeta2/bugの計画記録9 pathだけ。検証済み版とのsrc/include/platform/config/WS141/対象mailbox hostのdiffは0で、追加buildは不要と判断。結果commit `dde7c1ba7e27a0c2623280c368f31c1f8953a40b`（WIP）。
- mainがcleanで上記統合版のancestorであることを確認し、`git merge --ff-only codex/ws141-integrate`で取り込み。main HEADがdde7c1ba7、実装819803b63がancestor、取り込み前b1972bf58との差が担当25 pathだけであることを読み返して確認。共有Master/Queue/Guardrail/HAL API/toolchainには本成果の変更無し。push/外部連絡無し。
- i09はユーザーが承認したsoftware修正/build/統合の範囲でcleared。i08のuncleared結果を改変しない。p003はin-progress、WSはincomplete。実機でのR0 frame採用・元のHDMIへの復帰・buffer寿命/IRQ、T1回帰、flip/合成/resident登録・V3D投入・p007は未実施。
- Q1の保留投影: WS048 p003/WSの容量0tag契約拡張とhost結果、共有記録のLinux順再初期化の新承認/旧方針の置換、T1への依頼。担当から共有bodyは編集しない。次の実装Queueは自動開始しない。統合済みi08/i09のpatchと容量0tag提案を再適用しない。


## i10の選択・実装範囲（2026-10-09）

- 承認者/出典: このchatのユーザー「では続けてください。」。WS141/p003の既存P1/P2の次の有限範囲を選択。実機確認は後で行うという決定を保持する。
- scope: 初期化済みR0の同じport/mode/channel0で、callerが所有しDMA期間保持するRGB32 bufferを同期flipする部品。2つの専有SRAM slotとboot listを使い、cache clean→完整list→背景fill/次pointer→新listを観測したvblankという順。初期display以外の出力やPHY/PV/clockは変更しない。
- criteria: current/nextの同一性と他channelの停止を確認してからinactive slotへ書く。旧listのIRQや違うPVのIRQでは完了しない。timeout後は旧/新bufferを保持し、consoleへのfresh-frame復帰が証明されるまで再利用を拒否。成功した後だけ旧参照を退役。controllerをdisplay内へ保持し、IRQ/workerの状態をspinlockで保護する。
- 除外: 連続buffer allocator/公開GPU ops/device登録・自動boot flip・P3の2-plane/clock負荷・V3D。新部品を起動からflipさせず、元のR0 scanoutと実機条件を保持。partsのbuild/hostを確認して統合する部分attemptで、P2 whole acceptanceではない。
- 規則/依存: Full C全文・Guardrailのlicense/HAL/出力先制約。固定Linuxのflip/vblank side effectをignored sourceから確認、名称/構造/commentは独立実装。R0の実ソースと短いhost検証を依存出力とし、実機成功は仮定しない。HAL API変更無し。共有Master/Queue/Guardrailは更新しない。


## i10の実装確認（2026-10-09、main統合前）

- 起点 `4e173818d89053e03907b1de8ccd1c7ff724c8a5`。`display-flip.c/.h`へ実MMIOの同期flip/console復帰と、caller-owned buffer保持のprivate契約を追加。display内の状態はkernel寿命を持ち、既存PV/HVS source callbackと同じspinlockで保護する。IRQ sourceをW1Cしてからcurrent listを照合する。起動から新しいflipを呼ばず、allocator/公開GPU API/登録/HAL APIは変更していない。未使用present/restoreはLTO/section除去の対象なので、vmunix linkだけでは本体の実行を証明しない。
- 固定Linux v6.19のnext-list publicationとcurrent-list採用による完了/underrun再開を照合。専有slot 64/80へfresh 8-word plane＋ENDを完成させ、bufferのcache cleanとbarrierの後にnextを公開。console list43は保持する。他channel/portが所有されている時、mode/routing/active pointerが変わった時は書き換えず拒否。timeoutの後は旧/新の参照を保持し、遅延採用でも通常flipを再開しない。確認済みconsole復帰だけが不確かな保持を解除する。
- `sh plan/ws141/tests/display-host-test.sh build/ws141-flip-i10/host` → generator/実executor＋IRQ/実flip＋IRQの3試験PASS。両port/RGB order・literal9 words・cache/list/next順・inactive slot再利用・old-list/別PVの誤完了拒否・実frame sequence・cache中のpresent/restore競合拒否・timeout/遅延採用/復帰timeout/復帰再試行・foreign channel/list・DMA geometry/active aliasの拒否を確認。single-thread guard stubはreentry/lock中waitを検出するが、SMP memory ordering/実機DMA/firmware RAM寿命を証明しない。
- `make -j2 ZEDBSD_CONFIG=config/ci/config-rpi4.mk BUILD=build/ws141-rpi4-y CONFIG_DRIVER_BCM2711_GPU=y vmunix` → exit0、warning/error0、ELF/image checker3 PASS。同targetのBUILD=n/driver=nもexit0、変更対象が無いためup-to-date。y SHA256 `dbcf5bba5fe8083d491b84e788a23d040b9cf4ab8589e57ace81a45cfe87bae3`、n `e7446d4f070cc09d1a41c79c3e8d00d0343d33290af8a3f759db8b94e9013e26`。ログは`build/ws141-flip-i10/kernel-y-final.log`・`kernel-n.log`・`display-host.log`。共有LLVMは変更無し。
- C全文manual review: 順序/宣言/段落/条件/return/IRQ guardとbuffer所有を確認。clang-format-19 19.1.7（ColumnLimit0、定義引数tab復元）、GCC14.2.0 host。新source/header・private header・変更IRQと新host2 fileのstyle-check total0。runner `sh -n`、`git diff --check -- . ':!*.diff'`も0。630 hardware旧名の語単位一致0。GPL参照source/改名表はignored tempのみ。p007の全WS最終準拠/license/設計類似監査は別の残件。
- 今回のsoftware部品とbuild/host条件は達成。最新mainとの独立統合を続け、統合後の範囲でi10の結果を確定する。P2/whole p003/WSは完了にしない。実機R0/画面/IRQ/console RAM寿命、P1/P2 hardware受け入れ、連続buffer allocator・display ops/登録・P3合成、V3D投入とp007、Q1/T1回帰は未実施。


## i10のmain統合結果（2026-10-09）

- 実装commit `8ca85c4a9`、最新mainとのcode統合`d6c4fe08de26d8735c55e7dfcb6bdecbdc5b2d26`、追加された他WSの文書を保持したmain統合commit `181339820df839427f3322a7ac25f6663517096f`。main基点は`79817ed884e401895254bcafc4b5bc0ab1eeb4cd`、検証中の更新`d71e9572fb05c77b225a8ac805ec6c05d7617189`との差は他WS文書だけ。共有mainはclean状態を確認してfast-forward。担当差分はWS141/BCM2711 private sourceとarm64の当該source列のみ。master.mdを含む他セッションの更新はそのまま保持した。
- 専用統合worktree `/home/awe/zedBSD-claude1/.claude/worktrees/ws141-integrate`で`sh plan/ws141/tests/display-host-test.sh build/ws141-integration-i10/host` → 3試験PASS。driver=y named vmunix build → exit0、warning/error0、ELF/image checker3 PASS。driver=n named build → exit0、up-to-date（新しいchecker実行無し）。統合versionのhashは自worktree確認版と一致: y `dbcf5bba5fe8083d491b84e788a23d040b9cf4ab8589e57ace81a45cfe87bae3`、n `e7446d4f070cc09d1a41c79c3e8d00d0343d33290af8a3f759db8b94e9013e26`。ログは同worktree `build/ws141-integration-i10/display-host.log`・`kernel-y.log`・`kernel-n.log`。文書だけの追加追従でsource/build inputは変わらず、確認を再拡大しなかった。
- i10はcaller-owned bufferを扱う部品のsoftware/build/統合部分範囲で**cleared**。cycleの選択済みattemptを全件終え、finishedに戻す。p003は**in-progress**、WSは**incomplete**。P1/P2の実機条件と全display登録の達成は含まない。ユーザーが後で行うR0/画面/IRQ/console RAM寿命、Q1/T1回帰、連続buffer allocator/owner・display ops/登録・自動flip・P3合成、V3D投入、p007は残る。
- 再開点: buffer ownerを連続かつ1 GiB未満のCPU mapping付きで接続し、保持maskを尊重したpresent/restore/失敗時の寿命管理をdisplay opsへつなぐ。今回の追加APIはprivateのみ、公開GPU/HAL API変更は無し。次の有限scopeは次の継続指示で選択する。共有記録/WS048 bodyの投影とT1依頼はQ1、担当からMasterを更新しない。push/外部連絡/実機/QEMUは未実施。
- 統合済みi10保存patchは`build/ws141-handoff/ws141-i10-merged.patch`とmanifest。再適用しない。以前のi03/i08/i09 patchも再適用しない。最終記録commitはmachine-readable manifestで追える。


## 完成までの継続承認（2026-10-09）

- ユーザー原文「続けてください。完成まで自走してください。」。WS141の残る実装を一連の有限scope i11〜i15として承認。途中の部分attemptで終了せず、依存を実sourceで確認しつつ実装/検証/統合を続ける。実機をユーザーが後で行うこと、Masterを担当が更新しないこと、公開HAL API具体差分の事前承認、独立worktree、GPL資料非commitと最終監査は維持する。
- p006の方針判断: このchatのユーザー回答「WS141に実行器・compilerも含め、Keiland表示まで進める」。従来の方針決定だけのp006を実装へ拡張し、別WSへ移さずkernelのVulkan実行器とSPIR-V→V3D backendを含める。i915のGen12 machine codeを流用しない。公開GPU APIを保ち、実装済み処理だけをcapabilityで報告する。
- 完了判定: code/build/hostと実機/desktopの受け入れを別に記録する。実機未実施でWS completedやdesktop成功を主張しない。materialな新HAL API差分は具体案を用意して既存規則どおり判断を求めるが、それ以外の通常の技術判断で作業を止めない。


## i11: display所有・合成・登録のsoftware結果（2026-10-09）

- `buffer.c/.h`で連続run/CPU view/referenceのownerを追加。displayは全画面copy用の低1 GiB bufferを2つ恒久所有し、ordinary storageはopen別に作成/転送/map/破棄する。present後にsource resourceを破棄してもHVSはprivate copyを読む。timeout/closeではcontrollerがrunを保持し、新leaseはconsole復帰を先に確認する。
- `display-device.c`をR0後のattachへ接続。実装済みRESOURCE/TRANSFER/MAPPING/DISPLAY/DISPLAY_EVENTSだけを公開。2-open lease、非wrap sequence、fixed boot modeのquery/enum/validate、actual FIFO completionを実装。renderer/companion/BLOB/SHARE/COMMANDは後続i12〜i14。
- 2-planeのlistは128/160の17 word、primaryは64/80、console43/filter32〜42を保持。lower console/upper imageを独立に生成し、位置/premultiplied alphaを追加。両bufferのcache clean→完成list→next公開→selected PV/current一致を要求する。SRAM最低spanを0x42c4へ更新。
- Linuxのsingle-output plane load（unscaled 4 pixels/cycle、集計60%）とCOB要求の大きい方を計算。publication前にmax(old,new,500 MHz)をprovider上限で制限したclock要求、actual adoption後に必要rateへ下げる。firmware call中もBUSYを保持しspin guardは持たない。timeoutはtransition rate/旧新bufferを保持。clock引下げのrefusalは採用済みframeを失敗へ変えず、higher safe rateとsnapshotのclock_errorを残す。
- 起動はP1の1秒vblank観測→P2のgreen/purple stripe付き2-buffer flip→console復帰→P3の右上premultiplied checker合成→console復帰→P5登録。危険なwrite前の3秒pauseと各stopを維持。cadenceの数は観測値を印へ出す、host値を実機cadenceの証拠とは扱わない。
- BCM2711単独configは共通GPU coreを選んでいなかったため初回linkがdrv_gpu_register未定義で失敗。own arm64 source列でgpu.c/gpu-fence.cを不足時だけ追加し解消、PCI backend時の重複をfilter-outで避けた。公開GPU/HAL APIとroot Makefileは変更無し。
- `sh plan/ws141/tests/display-host-test.sh build/ws141-display-i11` → 4 PASS。実callback/allocator/flip/IRQを使い、RGBA→BGRA、source破棄、lease競合/旧completion不可視、restore timeout→retry、clock refusalのprepublication停止、17-word literal listとsmall positioned alpha、実boot診断経路を確認。物理DMA/ARM cache/SMP/electrical outputの証拠ではない。
- named build: `make -j2 ZEDBSD_CONFIG=config/ci/config-rpi4.mk BUILD=build/ws141-rpi4-y CONFIG_DRIVER_BCM2711_GPU=y vmunix`とdriver=n/BUILD末尾n → exit0、warning/error0、y checker3 PASS。ログ `build/ws141-display-i11{,-n}-build.log`。vmunix SHA256: y `3da45b164b6abb8e0782990f530af8280a61fcd1752709526b17e73dbef05493`、n `e7446d4f070cc09d1a41c79c3e8d00d0343d33290af8a3f759db8b94e9013e26`。
- C全文manual（object lifetime/排他/clock/adoption/unwind/条件と戻り）、clang-format-19/definition tab復元、補助style-check total0、git diff --check0。630hardware定数の旧名一致0、GPL source/改名表/作業文書はignored tempのみ。全WS/license/類似の最終監査p007は未実施。
- i11はsoftware部分だけcleared、p003/WSは実機のR0/P1/P2/P3/P5/console RAM寿命とQ1/T1回帰が未達のためin-progress/incomplete。main統合と再確認へ進み、i12のV3D power/MMU/jobを続ける。Master/共有Queue/Guardrail/他WS投影・GitHub公開はQ1へ保留、push無し。


## i11のmain統合とi12の再開（2026-10-09）

- i11実装 `d0141f6e26998fcf01be1d3b4f4b1d618399eccb`を最新main `c4263e998`基点へ専用worktreeでmerge `dae9fa7d5`。host4 PASS、rpi4 y/n build exit0・warning/error0、y checker3 PASS、vmunix hashはi11と一致。その間のQ1の更新 `825759eef`（rtld/記録等、kernel差分なし）を保持してmerge `40ce86ac0cd80df46d193714cff71759a18e440c`、共有mainへFFしread-back cleanを確認。
- push/GitHub公開/実機/QEMUは未実施。Master/他WSへの担当変更無し。i11部分cleared、whole p003/WSは未達を保持しi12へ継続。
- i12はnative電源providerを固定firmware DTBから照合。欠落していたDTBを正本commitからignored tempへ復旧しSHA256 `75761b73c284e26623e4d1624bff13e67bce2ae620880efd81d6571a3739fcfb`を一致確認、dtcでprovider/clock/reset/窓の構成を読む。実機が渡すoverlay後のtreeは未観測。
- `v3d-power.c`はdomain1/reset0/clock5の同一controllerとfirmware clock providerをFDTで検証。V1 native clock pulse→PM reset解除→clock on→ASB master/slave、V2 min/max/current照会・最大rate・running確認。V3D registerは未read。未知providerはwrite前に拒否。固定DTのRPiVid size0x20の直後にあるID word0x20を、同じpage内の既知4 byteとしてmapped span0x24へ拡張する（Linuxのpage-rounded mapによる同じアクセスと照合）。
- `v3d-power-host-test.sh`は実固定DTB＋actual FDT/parser/provider codeでnativeのstartup/reset順、PMの他bit維持、master ACK timeout時のslave復旧・reset未達、startup timeout後のregister admission拒否、emulator revisionでnative provider read0、foreign domain拒否を確認しPASS。kernel y build exit0/warning/error0/checker3 PASS。source全文review/format/補助style total0。i12全体は未終了、V3〜V10/MMU/cache/IRQ/job・GPU上の実完了/回復は次の実装。
- FDTのbounded string-list/reg-name lookupをshared internal helperへ移しdisplay startupも同じ実処理を使用。HAL/GPU公開APIの変更なし。GPL原文/改名表/fixed DTB/dtc出力はignored temp、最終license/類似監査はp007。


## i12: native V3D/V1〜V10のsoftware結果（2026-10-09）

- V1/V2のproviderに加え、`v3d-hardware.c/.h`でV3の4.2/単core/TFU/MMU/L3無し/VA32/PA30〜36の実read、V4の全L2T範囲とIRQ mask、V5の4 MiB表/scratchとnative PFN/readback/cache clean→barrier→MMU enable→page-entry flush→TLB clear、V6のcore/hub両bank ACK/latchを実装。旧世代のL2C/GCAを操作しない。公開後のPT/scratchは永続owner、cache/TLB timeout/IRQ fault/ASB失敗では保持する。resetはIRQ guardをjoinし、provider reset後の全hub/core IDの一致、表/cache/IRQの再設定を確認する。
- `v3d-job.c/.h`のkernel内部runnerは一つのworker slotだけを許す。全direct spanのPTE/物理幅を検査、binの実IRQ→render、予備256 KiB overflow最大4つ、TFUのfinal input-configuration launch、CSDのfinal first-word launch＋必須TMU/L2T cleanを実装。入力/出力の所有はcaller、`retired=false`のlaunched失敗はallocation/VAをquarantineする契約。公開clientの裏で勝手にresetしない。500 msごとにcurrent/returnまたは残batchの進捗を確認し、最大4窓（2秒）の有限上限を置く。TFU/停止したqueueは最初の500 msでtimeout。
- `cl.c`に1 tile（最大64x64、RGBA8、no MSAA）のclear/store生成を追加。clear値→2 dummy tile（最初にCLEAR、最後にVCD flush）→generic tile listの順。短い容量/geometry/stride/target alias/VA終端の拒否は全byte不変・used0。noopの既存imageは保持する。
- `v3d-diagnostic.c`をV6後・render device登録前にboot接続。private 2 MiB/VA 0x100000を保持してV7 noop→V8全4096 raster pixel確認→V9独立xy patternの8x8 raster→UB-linear（64 pixel）確認→V10 self-branch/500 ms timeout→native reset→fresh noop。普通のstage stopは次の段へ進まず、既に完了したstorageのPTEを除去/flushしてrelease。失敗したreset/translation flushはallocationとdescriptorを保持。ここでのresetはまだ他clientが存在しないboot専有scope。
- host: `sh plan/ws141/tests/v3d-power-host-test.sh build/ws141-power-i12-final` → PASS（actual固定DT/FDT/provider順）。`sh plan/ws141/tests/v3d-hardware-host-test.sh build/ws141-v3d-i12-final` → PASS（actual hardware/job/diagnostic source、literal MMU/cache/launch順、両bank IRQ、同時fault/完了、overflow、PTE tail欠落、cache/MMU/ASB timeout、CSD IRQ後clean、real packetからの出力model、missing store refusal、forced-loop reset→noop/PTE退去）。`sh plan/ws141/tests/clear-host-test.sh build/ws141-clear-i12-final` → C/primary XML oracleともPASS。hostの出力modelは物理V3Dの実行証拠ではない。
- named build: `make -j2 ZEDBSD_CONFIG=config/ci/config-rpi4.mk BUILD=build/ws141-rpi4-y CONFIG_DRIVER_BCM2711_GPU=y vmunix`とn/BUILD末尾n → exit0、warning/error0、y checker3 PASS。log `build/ws141-v3d-i12-final-build.log`/`build/ws141-v3d-i12-n-build.log`。SHA256 y `03bbf256302d6a96d386af14bc692c291f275f153a10237ae58287a92920fee0`、n `e7446d4f070cc09d1a41c79c3e8d00d0343d33290af8a3f759db8b94e9013e26`。
- C全文manual review（所有/retirement/IRQとworkerの排他/timeout/範囲と算術/段落と条件とreturn）、clang-format-19 19.1.7＋definition tab/packet table/one-argument-per-lineの復元、補助style total0、shell構文/diff check0。630 hardware旧名の語単位一致0。最終全WS/license/類似監査はp007で継続する。固定Mesa `v3dv_meta_copy.c`を正本commitから追加取得、MITのfile許諾/hashをlicense表へ追記、code/objectの取り込み無し。
- i12はsoftware/部分scopeをcleared。p004 whole acceptance/WS completionは実機待ちのまま。renderer登録/非同期worker/二device resource/shareはi13、Vulkan/SPIR-V/Keilandはi14、全最終検証はi15。Master/共有Queue/Guardrail/他WS/GitHub/T1の投影はQ1、push無し。


## i12統合確認とi13開始（2026-10-09）

- i12 source commit `ed62bb3bbc46a912cc644bf6037b30e95a3033c5`、統合commit `30350c8cba31875a01d58f804a5de13a1305a7ee`（WIP）。Q1のmain `1520080ede0f6fb1d6d3a8992f80ecdd22b0dd65`の変更を保持し、専用統合worktreeでmerge。競合なし。
- 統合treeのnamed rpi4 driver y/n buildともexit0、warning/error0。3つのhost（provider、hardware/job/diagnostic、clear/XML）PASS。y/n vmunix hashはi12 source結果と一致。logは統合worktree `build/ws141-i12-integration-{y,n}.log`。
- 共有mainを上記統合commitへfast-forwardし、担当branchも同期。i12のsoftware部分attemptはcleared、実機whole p004はin-progressのまま。push/外部公開無し。
- i13をin-progressにする。p005の二device/resource共有・VA所有・非同期worker/recoveryを開始。native allocationの独立referenceを共有し、scanout/fault/timeoutのDMA holdはresource/session破棄から独立させる。companion identityはcommon coreの現在の口で取得できないため0（任意preference無し）を保持し、役割とforeign sharingで選択する。公開API/HALを追加しない。


## i13の二device/allocation共有の実装（2026-10-09、継続中）

- 新規private `share.c/.h`、`v3d-memory.c/.h`、`render-device.c/.h`。native rendererはV1〜V10を安全に終えた後だけ別のGPU nodeとして登録。現在のcapabilityはresource/blob/transfer/map/share/allocation-share、COMMAND/CAPSET/Vulkanは未公開。device roleはdisplay/renderを別々に返し、optional companionは0。
- placed blobは実際のphysical runのsize/base alignment/全extent/max DMAを検証。HVSと共有可能なlow1GiB contiguous RAM、非snooping cached storageのCOHERENT条件はENOTSUP。flagだけを成功根拠にしない。
- exportはpage vectorとbufferの独立holdを持つ。元resource/openの終了から独立し、同device importは新descriptor、renderer importは新VA/local IDを持つ。foreign scanoutはlive native exportのvector identity/全extent/元のimmutable layoutを照合して独立buffer referenceを取得し、未知のexporter/MMIO/変更layoutは拒否。一般のforeign allocation-only importはcommon coreの既存EXDEV契約を維持。
- displayはCOPYとnative SHARED/FOREIGNを公開。ordinary presentは従来copy、BLOB presentはSRAM slotごとの独立buffer holdをnative publicationより先に取得。actual selected-PV adoptionが除いたslotだけを解放し、timeout/failed closeではold/candidate双方を保持。fresh console adoption後にだけ解放。RGBA/BGRAの上planeを独立encodeし、下console/HDMI mode/portは維持。
- rendererのsorted VA ownerはpage0を予約、全pageを同時mapしてcache/TLB flush後だけresourceを返す。mapのflush失敗は未公開viewをquarantine。unmap後flush失敗でもVA予約とphysical referenceを保持し、common faultを所有arm後に通知。coreの全owner退去とchecked native reset/最終表flushの後だけ回収。
- source build: named rpi4 y target exit0、warning/error0、checker3 PASS、SHA256 `932927e3741c81922f6ba0a13cc6b72f9963dd52806d7726db88deff422dac17`、log `build/ws141-i13-render-final-build.log`。
- short host: `sh plan/ws141/tests/display-host-test.sh build/ws141-i13-share-final-host`の4試験PASS（actual display/share/refcount/slot retirement、allocator placement違反、RGBA direct、source/capability/descriptor退去、2slot timeout/close/restore）。`sh plan/ws141/tests/v3d-hardware-host-test.sh build/ws141-i13-render-final-host` PASS（actual renderer/MMU callbacks、source/import別VA、元open/capability退去、PTE clear後TLB failureと未公開map failureのquarantine、live external owner時のreset拒否、failed native reset retained、再reset/flush後のrelease）。物理/SMP/cache電気的確認ではない。
- clang-format19.1.7、full C/manual ownership/VA/IRQ/failure scope review、style-check0、diff-check0、shell構文0、hardware630旧名照合0。新sourceは独自Zlib、GPL原文/構造転記・外部object無し。p007の最終全source/license/類似監査は後続。
- i13はin-progress。非同期worker/common completion/job supervision、Vulkan object/blob-id binding、SPIR-V/Keilandはまだ実装中/後続。現在のallocation-only recovery stopはnative commandを公開しない状態でのみ成立し、実行器をbindする前にworker join/callback drainへ更新する。実機whole acceptanceは未実施。

## i13 allocation checkpointのmain統合（2026-10-09）

source `f1afd22be`を専用統合treeでmerge `cfb3401f72940ea03a5fe0c528ab7c7b988ca283`（WIP）。統合named rpi4 y/n build exit0・warning/error0、actual display4hostとnative hardware/render/VA/share host PASS。y/n hashはsource版と一致。共有mainをcleanでfast-forwardし担当branchを同期した。i13全体はin-progress、worker/Vulkan/compiler/実機/全最終監査は残る。Master/shared recordsの作者変更は無し、push無し。


## i13 checkpoint: native workerとsupervised reservation（2026-10-09）

- `render-worker.c/.h`は16固定slotと1恒久kernel threadを所有し、最初のuserspace openで起動。prepared payloadをFIFOへ渡し、controller mutex内でbounded native CL/TFU/CSD runnerを呼び、disposerが独立viewを解放する。IRQ guardがqueue/session pending/fault/FINISHINGを保護する。common callbackは全lock外で一度だけ呼び、callbackのreturn後にslot/pendingを退役させる。
- submit refusalはpayload/completionを一切retainしない。stop_beginは新publicationを止め、queued payloadをcancelするがactive DMAを停止済みと扱わない。stop_pollはposted callbackを待ち、uncertain native storageがあればEIO。drainはcallbackをjoinし、DMA quarantineはview ownerへ独立保持する。native timeout/faultとtranslation flush失敗はdisposerより先にnative faultをarmし、common device lossをlock外で伝える。checked global reset/MMU flush後にだけworker uncertaintyを解除。
- supervised job tableはprivate binding関数を実装。実Vulkan queueが所有するsession timeline bitだけを受理し、同じslot poolに最大8 markerを予約してordinary decoder command用capacityを残す。commitはheap allocation/MMIOなしで既存slotをFIFOへ入れる。normal cancelはexact token/session/original callbackを確認して未投入slotだけをcallbackなしで撤回。fault cancelはRETAINEDとしてstop/drainまでcallbackを保持。stop_pollはordinary unpublished reservationを除外し、common monitorがnormal cancelできる契約（既存BUG-077対策）を満たす。
- `ops.jobs`/COMMAND/NOTIFICATION/JOB/CAPSETはまだallocation-only nodeへ公開していない。p006のdecoder/queue/compiler完成時に登録前の完全なbindingとして公開する。私有job tableとnative workerはactual source hostで直接確認済み。Vulkan/Keilandが動作可能とは主張しない。
- `sh plan/ws141/tests/v3d-hardware-host-test.sh build/ws141-i13-reservations-host` → PASS。実native runner/worker/VAを使用し16payload saturation/refusal、bin→render→callback、15queued cancellation、500ms hang→callback error/native quarantine→checked reset、8reservation saturation、foreign/stale/callback mismatch、normal cancel callback0、実job→ordered marker、RETAINED stop、unpublished normal cancel/idle、final-close callback joinを確認。
- hostの`kern/thread.h`は既存kthread_create/thread_startのopaque APIだけを宣言するfixture header（native sigset_t/tid_tとhost libc型の衝突を避ける）。schedulerは動かさずactual worker_stepを使う。物理DMA/cache/SMP/thread schedulingの証拠ではない。初期fixtureのclosed session再利用を修正し、1MiBモデルcache spanの期待値を追加。productionのhidden test controlなし。
- y build: `make -j2 ZEDBSD_CONFIG=config/ci/config-rpi4.mk BUILD=build/ws141-rpi4-y CONFIG_DRIVER_BCM2711_GPU=y vmunix` exit0、warning/error0、image checker3 PASS。ログ`build/ws141-i13-reservations-build.log`。vmunix SHA256 `c79948bc3c420ca545fe041394c886274d1b8e606a087f71534d32706465233a`。C全文manual/clang-format-19/definition tab復元/style total0、630hardware旧名一致0、git diff --check0。p007全WS最終監査は未実施。
- i13/p005はin-progress。software owner/worker出力をp006のscoped prerequisiteとして使い、compiler/decoderの実装を進める。公開bindingとKeiland描画は後続、実機whole acceptanceは後日ユーザーが行う。Master/shared projections/GitHub/T1はQ1、push無し。


## i13 worker統合とi14 compiler開始（2026-10-09）

- worker/reservation source `d03cd23d5`を統合worktreeへmerge `203b78809e338b6f1208cf21d2ba6aa6e35df5a6`。actual native host PASS、named rpi4 y/n build exit0・warning/error0。y `c79948bc3c420ca545fe041394c886274d1b8e606a087f71534d32706465233a`、n `e7446d4f070cc09d1a41c79c3e8d00d0343d33290af8a3f759db8b94e9013e26`。Q1最新mainのamd64 CI config更新を保持し`4422a38023c111c60439b426073308026ae53aff`へmerge、main/担当をFF同期。Masterを作者編集していない。
- i13/p005はin-progress。未公開のcommand/job table bindingはp006の実装と同時に完成させる。i14をin-progressにし、実装済みresource/VA/workerをscoped prerequisiteとしてV3D compilerを進める。whole p004/p005の実機clearanceを偽称しない。
- 既存Zlibの`i915/compiler/spirv.c`はdevice/MMIOを触らないscalar IR parser。コードを変更せずarm64でsourceを再利用し、V3D専用QPU encoder/loweringを担当driver内へ新規実装する。Gen12 code generator/batchは再利用しない。Keiland compositorのquad/panel SPIR-Vを対象に実際のoperation/interfaceを確認する。公開HAL変更無し。
- 固定Mesa commitのQPU/compiler一次sourceをignored tempへ取得し、各headerのMIT permission noticeを確認。命令のbit layout/入出力/hazard/終了はhardware事実として使用し、実装・構造・commentは独立Zlib。kernelへ外部compiler/NIRは取り込まない。file hash/licenseは監査表へ追記しp007で最終再検査する。

## i14 compilerのsoftware checkpoint（2026-10-09）

- 独立Zlib `qpu.c/.h` と `shader.c`、`shader-analyze.c`、`shader-lower.c`、`shader-output.c`、private headerを追加。既存Zlib scalar SPIR-V frontendはread-onlyでarm64のBCM source listへ追加。Gen12 code generatorや外部NIR/compiler実装を取り込まない。
- QPU4.2 encoderは単一ALU、明示のregister/accumulator/正確なsmall constant、signal/flag/portのalias検証を使用。partial wordは公開しない。固定MIT Mesaのactual decoder/repackerと45semantic opcode、全64RF MOVE、全48small constants、predicate/flag/signal destination/refusalを照合。fixed decoderの旧VDWWT/IID name aliasだけをtestで明示補正し、actual IID packerで同じwordを再確認する。
- graphics compilerはbounded SSAの定義順/meaningful source arity/last readerを検証、同じ入力をcanonical FIFO順で一度だけpreload、二threadの64RFからpayload/scratchを除いて値を割当。final outputsはepilogueまで保持。pure if-conversionのSKIP optimizationだけを省略し、SELECTをlane-wiseで維持。未対応loop/storage/discard/texture formはENOTSUPであり、未実装Vulkan featureは公開しない。
- Coordinate shaderはclip XYZW+floor-converted .8 viewport XY、render vertexはXY/depth/reciprocal W+fragment canonical varyingsをVPMへ出し、4.2 VPM wait後に終了。fragmentはpayload W/coefficientによるsmooth interpolation、normalized2D/four-float TMU lookup、scoreboard-owning final double switch、float RGBA tile出力、premultiplied source-over、RGBA/BGRA swizzleを生成。full-width constant/push/block/texture/sampler/viewport uniformの各消費をruntime descriptor列へ保存。終了SWと2delay NOPをowned code内へ含める。physical latency/math/texture/cacheの確認は未実施。
- `sh plan/ws141/tests/shader-host-test.sh build/ws141-i14-shader-final-host` PASS。actual Keiland quad/panelのcoordinate/vertex/fragment、fragmentのblend/swap4組合せを独立Mesa decoder/repackerで全word照合。別scalar IR interpreterとの32input差分（各panel mode/位置、nonunit W）、native uniform消費/TMU4result/TLB4channel/viewport/headerを確認。最後のnative4allocationを各々refuseし、partial binary非公開・全frontend/compiler ownership unwindを確認。普通のhost allocatorだけを置換、production test control無し。mock texture/単一laneはhardware execution・filter/derivatives・SMP/thread/hazardの証拠ではない。
- named y build `make -j2 ZEDBSD_CONFIG=config/ci/config-rpi4.mk BUILD=build/ws141-rpi4-y CONFIG_DRIVER_BCM2711_GPU=y vmunix` exit0、warning/error0、checker3 PASS。log `build/ws141-i14-shader-build.log`。SHA256 `c79948bc3c420ca545fe041394c886274d1b8e606a087f71534d32706465233a`。runtime未接続で未参照compilerはLTO/section GCでimageから除かれるためhashはworker版と同じ。実際のnew source compilationをlogで確認した。
- clang-format-19/定義引数tab復元、style total0、git diff --check0、既存630hardware rename旧名一致0。追加13Mesa sourceのheader許諾/hashを[license表](rpi4-gpu-license-audit.md#i14-compilerの追加参照2026-10-09)へ保存。p007の全WS最終規約/license/設計類似監査はまだ。
- i13/p005、i14/p006はin-progress。次はVulkan wire/session/object/reply buffer/nonzero BLOB allocation/pipeline/descriptor/draw CL/queue native payload接続。complete binding後のみCOMMAND/CAPSET/jobsを公開する。Keiland表示と実機whole acceptanceは未達。Master/shared projections/GitHub/T1はQ1、push無し。

## i14 compilerのmain統合・Vulkan runtime開始（2026-10-09）

compiler source `ee10b51ac3047c6f214164feaf62946eeda88ec0`を専用統合treeへmerge `7c5fa7ce4`、Q1最新main `effcde5a40127df371b9ef8113668a498353da45`を保持してmerge `5397c6329cc6cdf42470df97ac639e1b80874863`。統合named rpi4 y/n build exit0・warning/error0、actual shader/独立Mesa oracle host PASS。y `4630648c7841e8e265c61b1fb1abeb8dddb8147df4dcde48cdfe3ffa89051105`、n `ccd63d0d4e29682d5d78876ae9987eca9e0948248a314c5b0ed3a1137b63f214`。source hashとの差は保持したQ1のunix-socket修正による。logは統合tree `build/ws141-i14-integration-{y,n}.log`。main/担当treeをcleanでfast-forward read-back済み、作者がMasterを変更した差分無し、push無し。

i14は引き続きin-progress。device-independentな既存Zlib wire codec/generated record codecもread-onlyで再利用する。新native Vulkan objectはsession/kind/identityで分離し、registry/dependent object/prepared native payloadのreferenceを独立所有する。削除IDの再利用は古いnative ownerを置換しない。worker/callback join後のsession closeで全namespaceを撤去し、live objectが残る場合はsession storageを解放できない。complete decode/pipeline/draw/runtimeへの接続は作業中、capabilityは未公開。

## i14 Vulkan session/objectのsoftware出力（2026-10-09）

- 新規private `vulkan-private.h`、`vulkan-object.c`、`vulkan-session.c`。各sessionのtyped kind/identityを分離、live duplicateはEEXIST。registry/dependent object/prepared native workが独立referenceを持ち、removeはregistryだけを撤去、同じIDの新objectは古いretained payloadを置換しない。4096live objectの有限容量、reference overflowを拒否。controller mutexの下で呼ぶ部品であり、新たなpublic HAL/APIを追加しない。
- closeは全namespaceをwithdrawした後にregistry referencesを落とし、残るlive objectならEBUSYでsession/256KiB record arena/owner pointerを保持。worker/common callback join後、全依存が退役した時だけarena/sessionを解放しowner pointerをNULLにする。typed destructorはnative VA quarantineを別ownerへ残し、logical metadataの退役後もnative storage errorを返す契約。
- `sh plan/ws141/tests/vulkan-object-host-test.sh build/ws141-i14-vulkan-object-host` PASS。actual registry/session codeで2session/同ID別kind、duplicate拒否、prepared owner→ID削除→ID再利用→old final release、registry allocation refusal、memory→buffer依存、namespace撤去後のretained owner/close EBUSY・新publication拒否、late release/close再試行、destructor EIOでも全registry retirement、session/arena別allocation failureと全heap accounting0を確認。ordinary allocatorだけfixtureで置換、physical DMA/native schedulerの証拠ではない。
- source named rpi4 y build exit0、warning/error0、checker3 PASS。log `build/ws141-i14-vulkan-object-build.log`、image SHA256 `4630648c7841e8e265c61b1fb1abeb8dddb8147df4dcde48cdfe3ffa89051105`。未参照部品はまだLTO/GCで除かれる。clang-format-19/definition tab復元/style total0、git diff --check0。最終全WS/p007 auditは未実施。
- 既存Zlib `i915/render/codec.c`をread-only arm64 sourceとして追加。`codec.h`/generated `vulkan-codec.inc`もdevice-independent sourceとして参照し、i915 executor/object registry/Gen12 batchは再利用しない。次はbounded transport/reply/external stream、typed instance/device/memory/nonzero BLOB binding、pipeline/descriptor/draw/queueへの接続。i13/p005、i14/p006はin-progress、COMMAND/CAPSET/JOB未公開、実機未確認。Master/shared recordsはQ1。


## i14 Vulkan streamのsoftware出力（2026-10-09）

- `vulkan-stream.c`はshared Zlib codecをread-onlyで使い、実clientのSET_REPLY/SEEK_REPLY/VERSION/EXECUTEのwireを独立実装。replyはsessionの実blobをCPU referenceで保持し、明示capacityとalignmentを検査。VERSIONの最後の自然alignment u32をrelease atomicで公開し、decoder進捗をnative GPU完了と混同しない。
- 外部streamはdecode前に全体をcopyし、同じreply backingや後続client mutationから切り離す。total copy budget64 MiB/depth4、現clientのcount1/optional fields0だけ対応。未対応opcode/flag、short wire、out-of-range selector/seekは拒否。全失敗経路でCPU buffer hold/copyを解放。dispatchにはrequested flagを渡す。native buffer APIにNULL releaseの契約が無いため、empty frameではreleaseを呼ばない。
- `sh plan/ws141/tests/vulkan-stream-host-test.sh build/ws141-i14-vulkan-stream-final-host` PASS。actual libvulkan wire writer＋actual native stream＋actual shared codecで、reply offset/capacity/trailer、同backing再選択、外部streamのimmutable snapshot、unsupported flags/depth/容量/unaligned seek/short header/selector無しと全heap/reference baseline復帰を確認。typed callbackはfixtureであり、GPU/atomic SMP/native completionの証拠ではない。
- source named rpi4 y build exit0、warning/error0、checker3 PASS。log `build/ws141-i14-vulkan-stream-build.log`、image SHA256 `4630648c7841e8e265c61b1fb1abeb8dddb8147df4dcde48cdfe3ffa89051105`。まだunreferencedなのでimageにはGCされる。clang-format19/定義引数tab復元/style0/diff check0。最終p007は未実施。
- i13/p005、i14/p006はin-progress。次はtyped instance/device/query/memory/pipeline/draw/queue runtime。COMMAND/CAPSET/JOBは未公開、実機未実施、Master/shared recordsはQ1。


## i13/i14: Normal NC mappingの限定承認（2026-10-09）

libvulkanのHOST_COHERENT必須条件に対応するため、既存HALのuncached RAM kernel aliasに一致するuser mappingをGPU/VMへ渡す4 pathの具体的patchを用意。AArch64 syntax-only確認/patch check PASS。共有source担当境界を越える当該差分の質問へユーザー「OKです。」、正確な範囲を[依存提案・承認](uncached-ram-mapping-proposal.md)へ保存し実sourceに適用。i13/i14の同scopeへこの4 pathだけを追加。HAL API/user UAPI/既存default policy変更無し。Master/Guardrail/他WS projectionはQ1。private bufferのalias lifetimeとnative Vulkan memory runtimeへの接続を続ける。


## i14 Vulkan native root/queryとNormal NC owner（2026-10-09）

- `vulkan-device.c/.h`は実clientのgenerated instance/device recordをdecodeし、typed instance→physical→device→queueの独立parent edgeを保持。remote layers/extensionsはlocal WSI側で除かれるためnativeでは拒否。全optional feature0、family0/index0/count1のgraphics queueだけ。GetDeviceQueue2の実timeline chainを検査し、二つのpresent wordを独立検査。完全なroot/registryを作ってからIRQ guard内でdomainをclaim、失敗はroot/domain/parentをunwindする。既存同lookupだけidempotent、別ID/別parent/別timelineは拒否。
- implicit physical/queue childを親destroy時にnamespaceから外すが、prepared ownerはroot/parent/domainを最後まで保持。worker slotにtimelineを追加し、supervised reservation時からnormal cancel/FINISHING callbackの終了まで維持。旧queueのfinal release後も同sessionの古いcallback slotが存在するdomainの再利用はEAGAIN。Queue/DeviceWaitIdleはsole workerの前続native同期完了とactual ready/fault/job_busy/uncertain/stoppingを確認、busy/lossを成功扱いしない。
- `vulkan-query.c`はactual client codecでphysical property/zero optional features/Normal NC coherent memory type0/256 MiB driver budget、graphics+transferの一family、RGBA/BGRA UNORM画像、float vertex formatsを返す。2D/4096/one level/layer/sample/one colour target/128 push/8 texturesの有限scope。depth/sRGB/compressed/storage/compute/geometry/timestamp等はclaimしない。private API1.1は既存transport要件であり正式Vulkan conformance証拠ではない。**各limit/format/memoryの完成runtime enforcementが公開の前提**。
- 承認済みshared4 pathを実sourceに適用。`bcm2711_buffer_create_uncached`は既存HALのNormal NC aliasを独立所有し、元cached direct mapをdata accessしない。render/display mappingはbufferのimmutable cache policyをuser VMへ渡す。最終ref後にNC unmap→physical free、unmap/free failureはstorageを保持。既存blob_id0/COHERENT placement refusalはまだ保持し、nonzero VkDeviceMemoryとのbindingは次の作業。
- host: `sh plan/ws141/tests/vulkan-device-host-test.sh build/ws141-i14-device-final-host` PASS（actual libvulkan wire.c/codec.c＋native transport/object/session/root/query、完全discovery record、feature/family拒否、exact queue/domain ownership、retained old queue、old callbackによるdomain再利用拒否、registry allocation refusal unwind、全heap0）。`sh plan/ws141/tests/uncached-mapping-host-test.sh build/ws141-i14-nc-final-host` PASS（actual buffer/VM source、Normal NCとDevice属性の区別、unaligned byte copyの正しいretained alias、VM fork/pin refが最後までunmapを防ぐ、unmap→physical free、mapping failure unwind）。ordinary allocator/descriptor/native ready/lock/CPU aliasはhost fixture、physical GPU/cache/SMP/schedulerの証拠ではない。
- final source y build `make -j2 ZEDBSD_CONFIG=config/ci/config-rpi4.mk BUILD=build/ws141-rpi4-y CONFIG_DRIVER_BCM2711_GPU=y vmunix` exit0、warning/error0、checker3 PASS。log `build/ws141-i14-device-final-y.log`、image SHA256 `4415e968fe2aff0ad918f12870d7d0be05072967924f25b6e005ffdd265e8ab3`。新root/queryは未接続でGCされる。clang-format19/definition tab復元、full changed-scope manual review/style total0/diff check0。p007 whole WS auditは未実施。
- 次はVkDeviceMemory/nonzero BLOB binding、image/buffer/descriptor/pipeline/command buffer/draw/queueの完成とnative job接続。i13/i14/p005/p006 in-progress、COMMAND/CAPSET/JOB未公開、Keiland/実機未確認。mainの新しいQ1成果を保持して統合する。Master/shared planning/Guardrail/他WS projectionはQ1、push無し。


## i14 root/query/Normal NCのmain統合確認（2026-10-09）

- source commit `ee89c4b7d`、object/session `85450f1e8`、transport `9da0b4c27`を、Q1最新main `372ff3515`へ専用統合worktreeでmerge。統合commit `ea4d0e299228bce8d688fe56e6ccbda5824fadca`、競合無し。Q1のUSB/desktop/plan成果を保持。Master変更無し、push無し。
- 統合treeのactual client/native root-query hostとNormal NC buffer/VM hostともPASS。named rpi4 y/n build exit0、warning/error0、各checker3 PASS。log `build/ws141-i14-root-integration-{y,n}.log`。y SHA256 `4415e968fe2aff0ad918f12870d7d0be05072967924f25b6e005ffdd265e8ab3`、n `32d5dc572e8742792695f3444a8cd37de8a276b3406a539468607b4478c4453c`。hostは物理GPU/cache/SMPの証拠ではない。
- 検証済み統合treeへ共有mainと担当branchをfast-forwardする。i13/i14はin-progress、復帰点はVkDeviceMemory/nonzero BLOB/resource/pipeline/draw/queue。COMMAND/CAPSET/JOB未公開、Keiland/実機/p007未達。共有Master/Guardrail等の限定例外/progress投影はQ1。

## i14 VkDeviceMemoryとplaced BLOBのsoftware出力（2026-10-09）

- 承認済みi13/i14内で、private `vulkan-memory.c/.h`、rendererのprivate Vulkan session pointer/aggregate declaration budget、nonzero BLOB routingを追加。普通のcached `blob_id=0` allocatorのcoherence拒否を保持。public COMMAND/CAPSET/JOBはまだ未公開、実sessionのVulkan pointerは未接続。p005/p006とi13/i14はin-progress、Keilandの表示や実機の成功を主張しない。
- 実 `userland/desktop/libvulkan/memory.c` と `wire.c` を照合。AllocateMemoryはtype0、同openのtyped device、exact output ID、ordinary/EXPORT/IMPORT_RESOURCEの一段chainを確認。共有markerはOPAQUE1とprivate WSI0x200を受け付ける。0x200はrenderer内部のshare hintで、Linux dma-buf fdの提供ではない。allocation descriptor/device edge/宣言budgetを先にpublishし、physical RAMはBLOBでactual alignment/DMA limit/COHERENT条件を受け取った後にNormal NCで確保する。
- importは同openの実BLOB/native viewのみ、cached RAMやquarantined viewを拒否。独立view referenceが元resourceの破棄後もstorageを保持。nonzero BLOBはtyped VkMemory/extent/share permissionとphysical placementを確認し、RAM→実VA map/cache flushの成功後にviewをpublish、返すbuffer referenceとVkMemoryのview referenceを分離。後続resourceはさらに独立VA viewを持つ。
- queried256MiB heapに対し、live VkMemory declarationのpage-rounded extentをdriver-wideで合計し、import aliasも保守的に加算。これは宣言budgetであり、破棄されたVkMemoryの後もBLOB/VM/share capabilityが保持するphysical RAMの測定値やhard physical heap制限ではない。resourceの既存extent/count/VA制限と独立ownerは保持する。
- actual flush failureはzero-reference/quarantined native viewのownerにRAM/VAを残す。lazy BLOBのmap失敗時もcontrollerのadmissionを採り直し、native不確定ならlock外でcommon lossを公開する。memory identity/budgetの退役とDMA storageの退役を分離。VkMemory destructorのnative unmap failureはquarantineに渡し、結果を返す。
- **先行root/query checkpointの修正**: 実 `vulkan_command_begin` はvoidも常にrequested=1でopcode echoを要求する。root DestroyInstance/DestroyDeviceの「parameter reply無し」をrequested=0と取り違えていたため、0/1を受け付けるよう修正。前のhost fixtureは手動headerでこの不一致を見逃した。fixtureをactual `vulkan_command_begin` の8-byte headerへ変更し、FreeMemoryも実client requested=1/echoを確認。旧host PASSの履歴は当時の範囲として残す。runtime入口が未公開だったため実clientで動作済みとは扱っていない。
- bounded host: actual client wire/codec + native stream/root/query/object/session/memory + **actual v3d-memory.c/mmu.c** を実行。物理allocator/native flushは明示fixture、hardware/cacheを模擬成功と主張しない。late placement、repeat aliasの同一RAM、厳しいDMA ceilingとbad alignmentの拒否、元VkMemory→resource→import VkMemoryの独立退役、aggregate declaration exhaustion、payload/registry OOMのbudget/device edge unwind、failed flush後のactual VA quarantine/fixtureが与える後のreset境界でのrecoveryを確認。全heap0、timeline0、reply owner1。別のNormal NC buffer/VM試験のownership証拠と区別。
- commands: `sh plan/ws141/tests/vulkan-device-host-test.sh build/ws141-i14-memory-host` → memory/placed BLOB/import/budget/actual VA quarantine PASS、actual client root/query/domain PASS。named rpi4 driver y `make -j2 ZEDBSD_CONFIG=config/ci/config-rpi4.mk BUILD=build/ws141-rpi4-y CONFIG_DRIVER_BCM2711_GPU=y vmunix` → exit0、warning/error0、arm64 check3 PASS。formatter/最終source/build再確認とmain統合の結果は次の追記に保存する。
- next: VkBuffer/Imageのlayout/requirements/binding、descriptor/sampler/shader/pipeline/renderpass/framebuffer、recorded command/draw/native CL/queue/common submissionとcomplete public runtime binding。closeはworker/common callback join後にtyped namespaceを退役し、残存logical ownerがあるsessionをfreeしない。near-final p007全source規約/license/類似/build統合と実機受け入れは未実施。Master/shared board/syncはQ1。

### memory最終source確認

- `sh plan/ws141/tests/vulkan-device-host-test.sh build/ws141-i14-memory-final-host` → 上記2範囲PASS。
- 最終named rpi4 y build → exit0、warning/error0、arm64 check3 PASS。log `build/ws141-i14-memory-final-y.log`、vmunix SHA256 `8cceed68fc984cc067b3ed3918c53bf4a36bfd80b770ba2ed4a5516d3055fdb6`。
- clang-format-19 19.1.7適用後、definition argument tab/public-before-static/ANSI declaration/comment/ownership/error pathを全文C規約で確認。新memory/header/render private source/root correction/hostのstyle-check total0、`git diff --check` 0。失敗時のnative retirement errorを無視せずpublication refusalより優先。
- 残る全WSのfull-standard conformance/license/類似監査はp007、公開Vulkan runtime/Keiland/物理動作は未完了。

### memory/BLOBのmain統合

source `7ad8b94699d6080f950911d7e1ce877c440cba3f` をQ1 main `52551818e3f32a3390daf0300ff6984849b47966` と専用統合treeでmergeし `63537b070d6e481946ecb57ba40c2f3af181e40e`。対象pathの衝突無し、Q1のUSB/desktop/共有記録変更を保持。統合版 `vulkan-device-host-test.sh build/ws141-i14-memory-integration-host` の2範囲PASS、named rpi4 y/n buildともexit0/warning/error0。y arm64 check3 PASS・SHA256 `8cceed68fc984cc067b3ed3918c53bf4a36bfd80b770ba2ed4a5516d3055fdb6`、nは対象source無しでup-to-date・SHA256 `32d5dc572e8742792695f3444a8cd37de8a276b3406a539468607b4478c4453c`。共有mainと専用branchのHEAD readbackをmerge SHAで確認、main clean。Masterを担当が編集していない。次はbuffer/image/binding、i13/i14とp005/p006 in-progress、COMMAND/CAPSET/JOB/Keiland/実機/p007は未完了。

## i14 buffer/image/requirements/bindingのsoftware出力（2026-10-09）

- private `vulkan-resource.c/.h` とarm64当該source列を追加。実clientのgenerated standard recordをread-only Zlib codecでdecodeし、typed buffer/image create/destroy、requirements、bind、linear colour subresource layoutを接続。COMMAND/CAPSET/JOBは未公開、resource runtimeはまだpublic entrypointから呼ばない。p005/p006とi13/i14はin-progress。
- bufferは実requested byte extentと64-byte-rounded allocation requirementを分離。usageはvertex/index/uniform/transferのみ、最大256MiB。画像は2D4096-square、1level/layer/sample、RGBA/BGRA8UNORM、sample/colour/transferのsubset。pitchはwidth×4の64-byte alignment、colour subresourceはoffset0/rowPitch/sizeをexactに返す。optimalのnative storageもrasterだがCPU subresource layoutを公開しない。TMU/RCLへのactual pitch/format loweringは後続。
- optional external declarationのexact type/single chain/opaque1またはprivateWSI0x200をnative側で確認し、shared codecがskipする未実装意味を黙認しない。sampler/descriptor/runtimeの未実装capabilityを公開したとの主張はしない。
- bindはsame-device typed memoryを独立retainし、既存binding/rebind・不正alignment・required interval超過・未backed/quarantined memoryを拒否。buffer/image identityやVkMemory identityの破棄後もprepared resource/view ownerが残る間、memory/device/declaration budget/native RAMを保持。draw/transfer向けbacking resolverはexact logical extentとimmutable allocation intervalを確認し、borrowed VA view/Normal NC CPU aliasだけを返す。jobはcontroller mutexを離れる前に独立retainする責務。
- actual client `vulkan_command_begin` とstandard record encoder/decoderでnative stream/object/root/query/memory/resourceおよびactual `v3d-memory.c/mmu.c` を実行。65-byte buffer/128-byte requirement/type bit1、17×3 external linear image/pitch128/extent384、misaligned bindのmutation無し拒否、nonzero offsetのVA/CPU span、logical extent超過の拒否、memory identity/BLOB退役後のbinding保持、prepared ownerのresource identity退役後の保持/final releaseを確認。全heap0。native physical allocator/flushは明示fixture、GPU/cacheの物理動作を証明しない。
- 最初のhostはfixtureのroot用destroy helperをresourceへ流用してdevice IDを送らずassert FAIL。sourceのresource parserがactual required parentを拒否した結果。fixtureを実 `vulkan_object_destroy_remote` の`device/id/allocator`形式へ修正後、`sh plan/ws141/tests/vulkan-device-host-test.sh build/ws141-i14-resource-corrected-host` と `... build/ws141-i14-resource-final-host` → memory/VA・resource/binding・root/query/domainの3範囲PASS。external image declarationをactual encoderへ供給した追確認 `... build/ws141-i14-resource-external-host` も保存。failed coreはignored temp扱い、担当が削除しない。
- named rpi4 y build `make -j2 ZEDBSD_CONFIG=config/ci/config-rpi4.mk BUILD=build/ws141-rpi4-y CONFIG_DRIVER_BCM2711_GPU=y vmunix` → exit0/warning/error0、arm64 check3 PASS。log `build/ws141-i14-resource-final-y.log`。resource routerは未接続のため未使用section除去後hashはmemory統合版と同じ。clang-format-19、definition argument tab/全C標準manual、style-check total0、diff-check0。whole WS p007/license/類似監査は後続。
- next: image view/sampler/owned SPIR-V module、descriptor/pipeline layout、render pass/framebuffer/pipeline compile、command buffer/native draw/queue/public runtime。実機はユーザーが後日実施、WS completed/Keiland描画成功はまだ記録しない。

### external image fixtureの再確認

`build/ws141-i14-resource-external-host` はFAIL（actual writerのexternal_memory_typeをfixtureが0のままにしており、OPAQUE宣言をnative type0へ変換した）。実 `wire.c:vulkan_encode_image_external` / native context設定を照合し、fixture writerにもnegotiated opaque type1を設定。sourceは正しくunsupported type0を拒否していた。修正後 `sh plan/ws141/tests/vulkan-device-host-test.sh build/ws141-i14-resource-external-fixed-host` → 上記3範囲PASS、style total0/diff0。source `b1bbe9845a714f530e1b37c7d24651bba1b12b96` はfixture correctionを伴う最終source検証後に統合する。物理acceptance/public runtimeは未達。

### buffer/imageのmain統合確認

source `b1bbe9845a714f530e1b37c7d24651bba1b12b96` とexternal-type fixture correction `deccc248112bb359946dae6a9f20a11a2cad38c3` をQ1 latest `bb7d07f23fa39941aa62ea48bda182a866130901` と専用統合treeでmergeし `4d1e343927e0c3ee69420c1ccbd60351d45f49a5`。統合版host3範囲PASS、named rpi4 y build exit0/warning/error0/check3 PASS。nはこのprivate sourceを含まず先行統合済みの検証を保持し追加build不要。共有main/専用branch HEADのreadbackがmerge SHA、main clean。Q1の他WS/共有記録を保持、Masterを担当が編集していない。i13/i14/p005/p006 in-progress。

## i14 immutable view/sampler/SPIR-V moduleのsoftware出力（2026-10-09）

- private `vulkan-input.c/.h` とarm64当該source列を追加。実client codecでCreate/DestroyImageView、Sampler、ShaderModuleを接続。generated recordのpNext/配列extentを複写cursorで確認してからdecode、complete creation tailをconsumeしてから普通OOMを構造化replyする。public COMMAND/CAPSET/JOB/runtimeは未公開。
- image viewはsame-device actual colour image、2D/full sole subresource/same format、identityまたはexplicit same-channel swizzleに限定。image parentとdevice rootの独立referenceをretain、sourceimage identityの退役後もview/jobが保持する。remaining mip/array countはsole subresourceに解決する。immutable format/swizzleは後続TMU/RCLで下ろす。
- samplerはnormalized2D、nearest/linear min-mag、single mip、repeat/mirrored-repeat/clamp-edgeを保持。comparison/anisotropy/border/unnormalizedは拒否。zero LOD bias/minと非負finite maxをIEEE float bit複写で確認、kernel FP instructionを使わない。actual Keilandのnearestとglass-linear recordをsource/hostで確認。hardware filtering/schedulingは未検証。
- shader moduleは最大128KiB、complete codeSize=encoded words×4、SPIR-V headerを確認してown host blockへ複写。command arenaやoriginal stream pointerを保存しない。stage/entry/interface/命令対応の実compiler validationはpipeline create時に既存private compilerへ渡す後続で、module creationをnative compilation成功と偽称しない。
- `sh plan/ws141/tests/vulkan-device-host-test.sh build/ws141-i14-input-host` の最初のlinkはactual client handle conversion symbolsが不足。実 `objects.c` のnondispatchable representation/wire ID helpersをGC付きlinkへ追加し、viewの入力はfixture native IDを直castせずactual local client objectからencode。次のlinkは先行 `-include time.h` のfeature selectionでpipe2 prototypeが出ずcompile FAIL、host compileに `_GNU_SOURCE` を設定。対象client sourceはread-only、kernel/source/toolchain変更は不要。
- 修正後 `sh plan/ws141/tests/vulkan-device-host-test.sh build/ws141-i14-input-client-object-host` → memory/VA、resource/binding、immutable inputs、root/query/domainの4範囲PASS。actual image-view parent retain/registry OOM unwind、prepared viewがimage/view identity退役後も残る、actual Keiland nearest/linear sampler、actual `kwl_quad_vert` source copy、arena/stream overwrite後のbyte保持、retained moduleがidentity退役後も残る、declared nonzero sourceのabsent array拒否、全heap0/timeline0/reply owner1。普通host allocator/native flushは明示fixture、物理cache/IRQ/GPU/QPU timingは証明しない。
- named rpi4 y build `make -j2 ZEDBSD_CONFIG=config/ci/config-rpi4.mk BUILD=build/ws141-rpi4-y CONFIG_DRIVER_BCM2711_GPU=y vmunix` → exit0/warning/error0/check3 PASS、log `build/ws141-i14-input-final-y.log`、vmunix SHA256 `8cceed68fc984cc067b3ed3918c53bf4a36bfd80b770ba2ed4a5516d3055fdb6`。input routerは未公開/未参照でGC除去、hash不変を機能稼働の証拠と扱わない。clang-format-19後definition tab/ANSI/public/static order/所有/エラー経路/full C標準manualとstyle-check total0/diff0。
- next: descriptor layout/pool/set/updateとpipeline layout（actual Keiland combined image sampler/512-set pool/32-byte push）、render pass/framebuffer/graphics pipeline、draw/queue/common worker integrationとpublic runtime。i13/i14とp005/p006はin-progress、Keiland/実機/p007未達。Master/shared投影はQ1。

### immutable inputのmain統合確認

source `64007b3e74814e696287c0caffde85f821c009e5` をQ1 latest mainへ専用treeで統合。first merge `b2a07f3d5c7829635f613923dfa2a73d21a0b3a8` でhost4範囲/named rpi4 y build PASS。Q1のmainが `7ca3c5e4037e8d609f260ad1b5b317024b32a9fc` へ進みfast-forward不能だったため、それを専用treeへ再統合し `c603fd47566f3c62e550bb8ac1be7e2158b39025`。新TCP headerとQ1の他WS/共有記録を保持しnamed rpi4 y buildを再実行、exit0/warning/error0/check3 PASS（`build/ws141-i14-input-refreshed-integration-y.log`）。共有mainと専用branch HEADのreadbackは最終merge SHA、main clean。nは当該private source無しで既存確認を保持。Masterを担当が編集していない。次はdescriptor/pipeline layout、p005/p006とi13/i14 in-progress、Keiland/実機/p007は未達。

## i14 canonical descriptor/pipeline layoutのsoftware出力（2026-10-09）

private `vulkan-layout.c/.h` と当該arm64 source列を追加。実client codecのwidth/array framingに従う独立native decoderでDSL/PipelineLayout create/destroyを接続。canonical binding order、duplicate/count/stage/typeの拒否、combined image sampler/uniform block（各binding1element）のfinite interface、immutable sampler/same-device parentを保持。4set合計textures8/uniforms4、push128bytes/4byte境界/vertex-fragment stage許可を検証。各stageは一つのdeclared range、overlapが無い場合も同stage複数rangeを拒否。unsupported storage/descriptor indexing等を公開しない。

layoutはarena/application pointerを保存せず、immutable samplerとdeviceを独立retain。pipeline layoutはset interfaces/deviceを独立retainし、exact push permissionを各wordに保持。source layout/sampler/public pipeline identity退役後もdependent pipeline/prepared ownerが残る間、依存graph全体を保持。partial constructionは成功したretainだけをpayloadへassignし、publication/parent失敗で全edgeをunwind、retirement errorを優先する。

`sh plan/ws141/tests/vulkan-device-host-test.sh build/ws141-i14-layout-host` と `... build/ws141-i14-layout-final-host` → actual client/object/record/native sourceの5範囲PASS。順不同2bindingのcanonical order/immutable sampler保持、actual pipeline layout encoder、registry OOM時のset edge unwind、vertex32/fragment96-byte push permission、repeated-stage拒否、public sampler/layout/pipeline identity退役後のprepared graph保持とfinal heap0/timeline0/reply owner1を確認。allocator/flushはfixture、物理GPU/cacheは未検証。

named rpi4 y build → exit0/warning/error0/check3 PASS、log `build/ws141-i14-layout-final-y.log`。layout router未参照でGC除去されるためhash不変、稼働可能との主張無し。clang-format-19後definition tab/full C manualを確認、style-check total0/diff0。p005/p006とi13/i14はin-progress、COMMAND/CAPSET/JOB未公開、Keiland/実機/p007は未達。

nextはactual Keilandの512-set pool/allocate/free/reset/update、immutable draw descriptor snapshots、render pass/framebuffer/graphics pipeline/native CL/queue/common worker/public runtime。pool/setの退役はold prepared ownerが保持するstorage/chargeと新しいpublic identityを分離する。Master/shared投影はQ1。

### canonical layoutのmain統合確認

source `d2887e8b7dbd282fa84181c9539c589328877f80` をQ1 latest `5ccfd126997d3202c20207c14b70ead78cf4040a` と専用treeでmergeし `389dd95f3afd964cd2843458aadf9561a91b6133`。統合版actual host5範囲/named rpi4 y build exit0/warning/error0/check3 PASS。対象外Q1変更を保持し、共有main/専用branch HEADをmerge SHAでreadback、main clean。nは当該private source無しで先行検証を保持。Master担当編集無し。i13/i14とp005/p006 in-progress、next pool/set/update/draw runtime、Keiland/実機/p007未達。

## i14 descriptor pool/set所有のsoftware出力（2026-10-09）

private `vulkan-descriptor.h`、`vulkan-descriptor-pool.c`、`vulkan-descriptor-sets.c` と当該arm64 source列を追加。actual client recordでpool create/destroy/reset、complete set batch allocate/freeを接続。poolはsame-device/free flag/finite capacityとsupported combined image/uniform typeだけを受け、live/old setのfinal ownerにcapacity chargeを結ぶ。one-command allocation/freeは最大64sets、session namespace4096を既存限界として保持。Keilandの512-set pool declarationを許容。

Allocateはcomplete input/output arraysをconsumeしてからwhole-batch fresh IDs/same-device interfaces/duplicate IDs/capacityを確認。各setがdevice/pool/layout/immutable samplerを独立retainし、chargeはcomplete payloadのみで取得。partial batch OOMはpublish済みidentityと未publish payloadを別々に退役、全charge/edgeをrestore、output count0のstructured Vulkan failureを返す。freeはselected same-pool IDsを全検証してからregistry edgeを退役。pool reset/destroyはpublic child identityをwithdrawし、prepared setは旧storage/dependencies/chargeを保持、last ownerでpoolが退役。mutable draw bindings/descriptor updateはまだ未接続。

`sh plan/ws141/tests/vulkan-device-host-test.sh build/ws141-i14-pool-sets-host` と `... build/ws141-i14-pool-sets-final-host` → actual client handle/record codec/native sourceの6範囲PASS。512-set declarationを持つbounded two-texture pool、2nd set registry OOMでfirst ID/双方charge/全parent unwind、complete batch output順序、exact free、reset後retained old setのcharge維持/capacity拒否/final release後reuse、public layout/pool退役後のprepared set graph保持、全heap0/timeline0/reply owner1。512sets同時の実確保・physical GPU/cache/IRQはこの試験では実施していない。

named rpi4 y build → exit0/warning/error0/check3 PASS、log `build/ws141-i14-pool-sets-final-y.log`。routersは未公開/未参照、kernel機能稼働の証拠とは扱わない。clang-format-19/definition tab/full C manual（successful-retain-before-field-publication、charge-after-complete、null-safe independent release、first native error preservation）を確認、style total0/diff0。p005/p006とi13/i14はin-progress、COMMAND/CAPSET/JOB未公開、Keiland/実機/p007は未達。

next: ordered descriptor write/copy updatesとimmutable draw snapshots、render pass/framebuffer/graphics pipeline、native CL/queue/common worker/public runtime。Master/shared投影はQ1。

### descriptor pool/setのmain統合確認

source `40309b8150d0fff8c32033fd92afb5f3fb82acb5` を専用統合treeでmergeし `82d179fbfe5985622d2c5dc05a0714cd109b7631`。actual host `build/ws141-i14-pool-sets-integration-host` の6範囲PASS、named rpi4 y build exit0/warning/error0/check3 PASS（`build/ws141-i14-pool-sets-integration-y.log`）。共有main/専用branch HEADをmerge SHAでreadback、main clean。Q1の他WS/共有記録を保持、Master担当編集無し。nは当該private source無しで先行検証を保持。i13/i14/p005/p006 in-progress。

## i14 ordered descriptor更新とdraw snapshotのsoftware出力（2026-10-09）

private `vulkan-descriptor-update.c` と当該arm64 source列を追加。actual client `descriptor_write` のselected image/uniform/texel framingとgenerated copy encoderに従い、write全件→copy全件の順序を保持。各操作は既存single-element interfaceに限定、各family64操作、最大128destinationのheap stagingを固定上限にする。初回destination cloneとreplacement cloneは成功retainだけをfieldへ公開。complete command検証前はlive setを変更せず、後続copyの不正入力/普通OOMで全staged edgeを退役。copyは先行write/copyのstaged stateを見て、destination immutable samplerを優先する。job向け公開private clone helperはexact resource interval/view/sampler/bufferを独立保持し、mutable setを後から参照しない。

image updateはsame-device sampled/bound image、GENERAL/SHADER_READ_ONLY layoutとsame-device samplerを確認。uniform updateはbound uniform bufferのlogical extent、4-byte offset、range1..65536、VK_WHOLE_SIZEのlogical remainderだけを受け、padded memoryを範囲へ加えない。old actual bindingはcomplete publication後に退役し、最初のnative retirement errorを維持して他のedge cleanupも完遂。普通void updateにparameter replyを捏造しない。

最初のcompileは新sourceが存在しないallocator名 `kern_kcalloc/kern_kfree` とこのscopeにないUNUSED_PARAMETER macroを使いFAIL。実projectのkern_calloc/kern_freeと明示unused commentへ修正。追加host fixtureのBLOB struct名も実 `gpu_blob_create` へ修正後、`sh plan/ws141/tests/vulkan-device-host-test.sh build/ws141-i14-update-host` と `... build/ws141-i14-update-final-host` → 7範囲PASS。actual client header/handle/standard record/native stream/actual VA sourceを実行、image write→copyのordering/immutable override、sampler retain overflow時の先行view unwind、後半invalid copyによる全rollback、transaction OOMのmutation無し、65-byte bufferのoffset4/WHOLE_SIZE=61、prepared old view保持、pool resetと全public identity退役後のimage/sampler/buffer/allocation owner保持・final heap0を確認。selected-field image-write helperは実client framingを忠実にencodeするfixtureでありvkUpdateDescriptorSets関数自体の実行ではない。physical allocator/native flushはfixture、GPU/cache/IRQ動作未検証。

clang-format-19/definition tab/full C manual（ANSI宣言/公開-static順/forward/所有/first error/finite bounds）とstyle total0/diff0。named rpi4 y build exit0/warning/error0/check3 PASS、log `build/ws141-i14-update-final-y.log`、SHA256 `edc090d1cd6b963206e381b7316c2ef7682bd91293a4c02cdbf9ecfebdddbb28`。public COMMAND/CAPSET/JOB/runtimeは未公開、i13/i14/p005/p006はin-progress、whole p007/Keiland/実機は未達。next render pass/framebuffer/compiled graphics pipeline、recorded native draw/queue/common worker/public runtime。Master/shared投影はQ1。

### descriptor更新のmain統合確認

source `d3a2069522da3a370657a01a356861976d290510` を専用treeでmergeし `26f6f243558fd6c1d2773a3a75683ceda55b837a`。統合版host7範囲/named rpi4 y build exit0/warning/error0/check3 PASS（`build/ws141-i14-update-integration-y.log`）。共有main/専用branch HEADのreadbackはmerge SHA、main clean。対象外Q1変更/Masterを保持、担当Master編集無し。nは当該private sourceを含まず先行検証を保持。i13/i14/p005/p006 in-progress、next render pass/framebuffer/native pipeline/draw。

## i14 render pass/framebufferのsoftware出力（2026-10-09）

private `vulkan-target.c/.h` と当該arm64 source列を追加。actual standard client recordを独立decodeし、single-colour/single-sample/single-subpass、RGBA/BGRA8、clear/load/discardとstore/discard、initial/final colour layoutを保持。input/resolve/depth/preserve/multiview/imageless/self-dependency等の未実装意味を黙認しない。external↔subpass0 dependency最大4、graphics/transfer/host stage/accessとBY_REGIONをretain。actual Keiland compose clear→present、load→present、backdrop→shader-readの1/2dependencyを照合。client PRESENT_SRCはwireでGENERALに変換されるため、native側もその実recordを受ける。dependency/cache/load/storeのactual hardware loweringは後続native command実行の責務。

framebufferはsame-device/pass format/bound colour image/full view/actual extent/layer1を確認。complete recordとallocator/output tailをconsumeしてからheap確保。pass/view/deviceの成功retainだけをpayloadへ公開し、registry OOMや後続edge失敗で全取得分を退役。old prepared framebufferはpublic framebuffer/passが無くてもexact targetとattachment lifecycleを保持。compatible passはこの限定single-colourのformat一致、load/store/layout/dependencyの差は互換性を破壊しない。render-area granularity1×1をactual same-device passから返す。

`sh plan/ws141/tests/vulkan-device-host-test.sh build/ws141-i14-target-host` → actual client header/handle/standard record/native sourceの8範囲PASS。clear/load互換性・present→GENERAL mapping・backdrop2dependency/sourceコピー・1×1 query・framebuffer registry OOM時のpass/view unwind・画像width16に対するwidth17 refusal・prepared framebufferのpublic framebuffer/pass destruction後のgraph保持/final heap復元を確認。先行descriptor snapshotが全public resourceを破棄した後のbacking維持も継続PASS。native physical allocation/flushはfixture、GPU/cache/IRQは未検証。

clang-format-19後definition tab/full C manual（ANSI宣言/公開-static順/forward/finite exact counts/失敗時所有/first error）とstyle total0/diff0。named rpi4 y build exit0/warning/error0/check3 PASS、log `build/ws141-i14-target-final-y.log`、SHA256 `edc090d1cd6b963206e381b7316c2ef7682bd91293a4c02cdbf9ecfebdddbb28`。routersは未公開/未参照、COMMAND/CAPSET/JOB/runtime/Keiland/実機/p007は未達。i13/i14/p005/p006 in-progress。next graphics pipeline/compiler interface validation/native code storage、recorded draw/queue/common worker/public binding。Master/shared投影はQ1。

### render targetのmain統合確認

source `1346dfb92c533f256cf6a67968e345cb87d50fee` を専用treeでmergeし `1f60f07f3b49e223c60bfd02bc9cae8a37c7c16b`。統合版host8範囲/named rpi4 y build exit0/warning/error0/check3 PASS（`build/ws141-i14-target-integration-y.log`）。共有main/専用branch HEADをmerge SHAでreadback、main clean。対象外Q1変更/Master保持、担当Master編集無し。nは当該private source無しで先行検証を保持。i13/i14/p005/p006 in-progress、next compiled graphics pipelineとnative draw/queue/public binding。

## i14 compiled pipeline graphのsoftware出力（2026-10-09）

private `vulkan-pipeline.h`、`vulkan-pipeline-build.c`、`vulkan-pipeline-state.c` と当該arm64 source列を追加。kernel側temporary creation fieldsから、dynamic viewport/scissor・single-sample triangle list・raw float vertex input・full RGBA opaque/premultiplied source-over・fill/cull/windingをimmutable metadataへ複写。既存reported16binding/16attribute/offset2047/stride2048を守り、unsupported stages/depth/discard/derivatives/specialization/other statesをrefuse。same-device module/layout/passを確認し、SPIR-Vの唯一のmain entryとexecution modelをnative側で明示検証（read-only frontendはarbitrary entry選択をしないため）。fragment sourceのcanonical varying keyをderiveし、coordinate/vertex/fragmentをexisting independent QPU4.2 compilerへ渡す。

全emitted uniformのset/binding type/stage visibility、exact push word permission、native coordinate/render vertex FIFOとdeclared float attribute componentの対応を確認。partial compilationやparent retain failureは全program/実取得parentを退役。compiledpipelineはlayout/pass/deviceを独立保持、source module/command arena pointerを保存しない。native codeはCPU storageで保持、GPU upload/VA/code cache visibilityは後続draw preparationの独立owner責務。wire batch create/destroy routerはまだ未接続。

`sh plan/ws141/tests/vulkan-device-host-test.sh build/ws141-i14-pipeline-build-host` と `... build/ws141-i14-pipeline-build-final-host` → actual client module/layout records・native scalar compiler/typed ownersの9範囲PASS。actual Keiland quad/SPIR-Vの3native variants、VPM coordinate6words/vertex2input/fragment2varyings、premultiplied output、wrong vertex push stage permission/不足attribute componentの拒否、frontend allocation OOM時のparent/program unwind、source moduleとpublic layout退役後のowned native code/interface保持・final heap復元を確認。graphics create wire request自体はまだ試験していない。前の固定MIT Mesa native word/source differential試験の証拠を保持、physical GPU/cache/timingは未検証。

clang-format-19後definition tab/full C manual（ANSI/section/forward/finite record/fallible retain-before-field/first error/FP bits/source entry/interface matching）とstyle total0/diff0。named rpi4 y build exit0/warning/error0/check3 PASS、log `build/ws141-i14-pipeline-build-final-y.log`、SHA256 `edc090d1cd6b963206e381b7316c2ef7682bd91293a4c02cdbf9ecfebdddbb28`。public runtime/COMMAND/CAPSET/JOB未公開、i13/i14/p005/p006 in-progress、Keiland/実機/p007未達。next actual pipeline wire batch/canonical selected-state decoder、recorded command/native CL/queue/common worker/public binding。Master/shared投影はQ1。

### compiled pipeline graphのmain統合確認

source `c6b013e546bd17171c42a094669f63f8d248ff47` を専用treeへmerge `8d100db17547ce86de1e9824ab8adfbbce847568`。統合版host9範囲/named rpi4 y build exit0/warning/error0/check3 PASS（`build/ws141-i14-pipeline-build-integration-y.log`）。Q1 mainが進んだため差分を確認し、他WS/T1のrecord-only変更を保持するmerge `3584304e8283e00ff0452e2e983ff6dff1e7e957`。対象sourceの変更無し、追加build不要。共有main/専用branch HEADを最終merge SHAでreadback、main clean。Master担当編集無し、nは当該private source無しで先行検証を保持。i13/i14/p005/p006 in-progress、next graphics wire decode/batchとnative draw/queue。

## i14 graphics pipeline wire batchのsoftware出力（2026-10-09）

private `vulkan-pipeline.c`、`vulkan-pipeline-decode.c`、`vulkan-pipeline-record.h` と当該arm64 source列を追加。actual client `pipeline_encode_graphics` のselected-state framingを独立decodeし、exact headers/absent chain/zero flags、2shader/main string、count-selected16binding/16attribute、dynamic1viewport/scissorのstatic array省略、single-sample mask0/1、colour1/constant4/dynamic2とcanonical no-derivative tailを保持。self-owned finite temporary fieldsへ複写し、arena/application/wire pointerを保存しない。graphics batch最大4でcomplete inputとoutput identity vectorをconsumeしてからwhole fresh IDチェック・compile/publicationへ進む。ordinary OOM/unsupported interfaceはmember failureとnullable exact outputを返し、合法なpartial successを独立保持。public destroyはregistry referenceだけを退役。

`plan/ws141/tests/vulkan-client-pipeline-host.c` はreadonly実 `resources.c` と `pipeline.c` をincludeし、real private encoderとreal `vulkan_render_pass_subpass`/handle conversionを使用するhost wrapper。single-colour subpassのlocal metadataだけは明示fixture、client source変更無し。`sh plan/ws141/tests/vulkan-device-host-test.sh build/ws141-i14-pipeline-wire-host` と `... build/ws141-i14-pipeline-wire-final-host` → 9範囲PASS。actual encoder→native stream/decoder/compiler/registryを実行、dynamic array omissionとrecord widths、同batchのfirst native success/second wrong push visibility failure、result count2/exact firstID/second0、prepared pipelineがpublic destroy後もcode/interfaceを保持しfinalreleaseできることを確認。先行pure backend/module/layout/descriptor/target/actual VA ownership試験もPASS。

kernel stackのconcrete record-array risk確認として、actual AArch64 source compile commandへ`-fno-lto -fstack-usage`だけを追加してprivate `build/ws141-i14-pipeline-stack/`へ4translation unitをcompile。shared LLVMはreadonly、source/build config変更無し。actual non-LTO static frames: pipeline dispatch4512、pipeline build672、shader compile816、SPIR-V parser3712 bytes、frontend中最大補助frame736。ARM64_SYS_STACK_SIZE=16384を照合。これはindividual compiled frame evidenceであり、公開runtime上位callerを含むtotal call pathとLTO形を証明しない。public binding/p007で全経路を再確認する。

clang-format-19後definition tab/full C manual（ANSI/section/forward/selected-state exact counts/自己所有record/complete tail-before-publication/partial vector/成功retainとfirst cleanup error）とstyle total0/diff0。named rpi4 y build exit0/warning/error0/check3 PASS、log `build/ws141-i14-pipeline-wire-final-y.log`、SHA256 `edc090d1cd6b963206e381b7316c2ef7682bd91293a4c02cdbf9ecfebdddbb28`。COMMAND/CAPSET/JOB/public runtimeは未公開、cache/computeは実装済みとしない。i13/i14/p005/p006 in-progress、Keiland/実機/p007未達。next command pool/buffer/recorded graphics state、native code/uniform/attribute/texture/CL prepared owner、queue/common worker/public runtime。Master/shared投影はQ1。

### graphics pipeline wireのmain統合確認

source `1c4945fd5cb1d81a3fea7b502891dab5b7860d76` を専用統合treeでmergeし `502608f4bf4d7e6021a427312a96ba2d0fe5bbe7`。統合版actual host9範囲/named rpi4 y build exit0/warning/error0/check3 PASS（`build/ws141-i14-pipeline-wire-integration-y.log`）。共有main/専用branch HEADをmerge SHAでreadback、main clean。対象外Q1変更/Master保持、担当Master編集無し。nは当該private source無しで先行検証を保持。i13/i14/p005/p006 in-progress、next primary command ownership/native recorded draw/queue/public binding。

## i14 primary command所有とactual vkCmd記録のsoftware出力（2026-10-09）

private `vulkan-command.h`、pool/batch/bufferの3 source、`vulkan-record.h`、record/decode/validateの3 sourceと当該arm64列を追加。actual client pool/allocate/begin codecsを独立decodeし、family0/primary、transient/reset/one-time/simultaneousの有限意味を保持。64-buffer batchはwhole fresh output vectorを検証後、complete parent graphを取得してpublish。普通OOMで全prefix/未publish payloadを巻き戻し、output count0を返す。poolのborrowed child listとbuffer→pool/deviceの独立edgesでcycleを作らず、resetはidentityを保持、destroy/freeはregistryだけをwithdraw。全childのpendingを先に検証し、partial pool reset/freeを拒否。recording/initial/executable/invalid、individual reset flag、互いに排他的なprimary usage flagsを保持。

実 `commands.c` を既存readonly client encoder wrapperへ追加し、real public vkCmdBeginRenderPass/SetViewport/SetScissor/BindVertexBuffers/BindPipeline/BindDescriptorSets/PushConstants/Draw/EndRenderPassの9eventをnative streamへ実行。client metadataは明示host fixture、native typed objects/resources/compiler/backingは既存actual sourceを使う。recorded void headersはrequested=0、Endだけrequested=1。selected colour clearのbranch0/representation2/array4とignored extra entry、16binding/4set/128-byte push、raw IEEE viewport words/scissor/target bounds、same-device/compatible canonical interfacesを確認。scalar payload/typed referencesをfinite immutable nodesへ複写、最大1MiB retained native event storageをstream batch容量と別に制限。partial acquisition/OOMは成功retainだけを退役し、最初のvoid failureを後続Endへ返す。native CL/submitはまだ実装済みとしない。

**descriptor更新意味の修正**: 再開時の「record後submit前のdescriptor更新を有効なまま許す」という想定を取り消す。ordinary Vulkan 1.0 layoutはupdate-after-bindを提供せず、更新したsetを記録で参照するprimaryはinvalidとなる。successful transactionごとのset generation、bound-recordのexact saved generation、End/将来submitのcurrent確認を追加。generation wrapとpending setのupdateを拒否、public set withdrawalもcurrent確認でinvalid。prepared descriptor cloneはactual jobが使うresourcesの独立lifetime用であり、無効なrecordingへ実行許可を供給しない。仕様一次確認: [vkUpdateDescriptorSets](https://docs.vulkan.org/refpages/latest/refpages/source/vkUpdateDescriptorSets.html)、[vkBeginCommandBuffer](https://docs.vulkan.org/refpages/latest/refpages/source/vkBeginCommandBuffer.html)、[set/pipeline compatibility](https://docs.vulkan.org/spec/latest/chapters/descriptorsets.html#descriptorsets-compatibility)、[identically defined objects](https://docs.vulkan.org/spec/latest/appendices/glossary.html#glossary-identically-defined)。user-facing checkpointの前の説明も同chat内で訂正。

`sh plan/ws141/tests/vulkan-device-host-test.sh build/ws141-i14-record-final-host` → actual client/native sourceの11範囲PASS（log `build/ws141-i14-record-final-host.log`）。2nd buffer registry OOMの全rollback、pending second child時の全prefix未変更、reset identity維持、withdrawn childのpool保持/final heap復元、real9event順序/clear/viewport、5th node OOMをEndへ通知・再record成功、pending set updateの全rollback、ordinary executable recordのupdate invalidation、全public pool/set/buffer/samplerを退役した後のold primary graph保持・final added heap0を確認。pending ownership/physical allocator/cache flushは明示fixtureであり、actual hardware completionの証拠にしない。

最初のhost compileはsnippet出力directory不存在のためtest function追加にFAIL、directoryを作って修正。client poolのprivate type未公開をfixtureの実generic object prefixに修正。最初のactual recording試験はfixtureがvulkan_command_beginでwhole writerをresetして既存SET_REPLY/record bytesを消したためFAIL、real End headerを既存record streamへappendするよう修正後PASS。gdbはsandbox ptraceがOperation not permittedで利用不可、追加ptrace/escalation/回避は実施しない。source確認とhost fixture correctionで解決。ignored failed core/tmpは削除せずQ1 cleanup対象として残す。

clang-format-19後definition tab/ANSI/section/forward、finite framing/short circuit、successful-retain-before-field/publication、borrowed list非cycle、first native cleanup error、kernel FP無しとgeneration/pending意味を確認、style total0/diff0。named rpi4 y build exit0/warning/error0/check3 PASS（`build/ws141-i14-record-final-y.log`）、SHA256 `edc090d1cd6b963206e381b7316c2ef7682bd91293a4c02cdbf9ecfebdddbb28`。routersは未公開/未参照でGC除去、hashをGPU稼働証拠としない。後続paragraph comment/condition line-break editsは意味変更無し、統合treeで当該最終sourceを再buildする。

p005/p006/i13/i14 in-progress、COMMAND/CAPSET/JOB未公開、physical/Keiland/p007未達。next: current generationを検証したimmutable draw preparation、stage別push/descriptor/vertex state、native GPU code/uniform/TMU/attribute/CLとindependent VA owners、transfer/barrier/queue sync/common worker、complete public binding。Master/共有投影はQ1。

### primary command/graphics recordingのmain統合確認

source `8dd4a94d76426b283ed0548d5ca7ea7fbd12055e` とQ1 record-only main `a42a544353d4e5e57da6847b7835c95f552f5e77` を専用統合treeで保持し、merge `d097f9b4660a54e2fbe0c1e8fa896a6b5d0234dc` へ統合。統合版actual host11範囲とnamed RPi4 y buildがexit0、warning/error0、arm64 check3 PASS。logs `build/ws141-i14-record-integration-host.log` と `build/ws141-i14-record-integration-y.log`、vmunix SHA256 `edc090d1cd6b963206e381b7316c2ef7682bd91293a4c02cdbf9ecfebdddbb28`。main/専用branchを同SHAへfast-forwardしcleanを確認。Master担当編集無し、nはprivate source無しで先行証拠を保持。i13/i14/p005/p006 in-progress、public binding/Keiland/実機/p007は未達、immutable draw state検証へ継続。

## i14 ordered draw stateのsoftware出力（2026-10-09）

private `vulkan-layout-compat.c`、`vulkan-draw.h`、draw/walk/validate sourceとarm64列を追加。pipeline layoutはexact push range groupingも保持し、combined V|F rangeと独立V/F rangesをword permission unionだけで互換と誤判定しない。canonical set definitionの共有比較、set Nまでの全prefixとexact push ranges、ordinary binding disturbanceを実装。stage別push word/layout、部分vertex bind、current pass/pipeline/dynamic viewport/scissorをserialized heap stateへ解決する。全graphをcallback無しで検証してから2回目にCPU-only準備callbackを呼ぶ。callbackは独立ownerを取得し、失敗時の全prefix rollbackをcallerが担う契約、DMA launchは禁止。

actual compiled coordinate/vertex/fragment uniform streamだけを消費し、unused descriptor/attributeに不要な設定を要求しない。頂点範囲はfirstVertex/vertexCount/stride/format全byteをlogical resource内で確認、allocation paddingを使わない。UBOはexact descriptor range内のwordとactual coherent VA spanを確認。sampled textureはinitialized combined view/sampler/implemented layout、全image span、attachment feedbackを拒否。独立image/memory handleでもnative VA spanが重なれば拒否。clear-only passもcurrent target backingを確認。no FP実行、no application/wire pointer保持。borrowed stateはcallback終了前に独立DMA ownerへ変換する必要がある。

一次仕様 [Pipeline Layout Compatibility](https://docs.vulkan.org/spec/latest/chapters/descriptorsets.html#descriptorsets-compatibility) を参照し、bind pipelineだけではpush valuesをdisturbせず、bound descriptorのprefix互換と再bindによるdisturbを分ける。これはp006内部実装で外部HAL/UAPI/他Phase scopeの変更無し。

`sh plan/ws141/tests/vulkan-device-host-test.sh build/ws141-i14-draw-state-final-host` → actual client/native sourceの12範囲PASS。real9vkCmd→whole state walking→begin/draw/end callback、temporary-state OOMとcallback OOM時のheap復元、48-byte vertex resourceを7vertex/firstVertex1で越えるlogical fetch拒否、stage違いpush不足、alias feedback拒否、synthetic immutable extra binding prefixによるcompatible higher-set維持/incompatible lower-rebindによるhigher-set disturbance、combined/separate push range不互換とdeclaration order違い互換を確認。synthetic prefix/immutable scalar mutationsは明示fault fixture、actual client decoderやGPU DMAを実行した証拠ではない。callbackはborrowed stateを観察するだけで、independent prepared GPU jobはまだ作っていない。

既存recording-only fixtureはsampled viewとframebufferが同じimageで、recording ownershipの試験には使えたがvalid native drawではattachment feedbackになるため、別actual sampled image/viewを同じcoherent memoryのnonoverlapping offset2048へbindして修正。productionにfallbackや特例を入れない。レビュー中のnative alias check追記が一時的に別helperへ入った編集ミスをsource確認で訂正、試験前に除去。

formatter19/definition tab/style total0/diff0、ANSI/section/forward/short circuit/全interval64bit/first error/borrowed pointer lifetime/heap stack reductionを確認。named RPi4 y build exit0、warning/error0、arm64 check3 PASS（`build/ws141-i14-draw-state-final-y.log`）、SHA256 `edc090d1cd6b963206e381b7316c2ef7682bd91293a4c02cdbf9ecfebdddbb28`。後続comment/return整理は意味変更無し、統合treeで最終sourceを確認する。global p007全文適合/公開runtime stack/physicalは未達。private routersはGC除去、vmunix hashはGPU実行証拠にならない。

i13/i14/p005/p006 in-progress、COMMAND/CAPSET/JOB未公開、Keiland/実機/p007未達。next independent immutable prepared draw owners（pipeline/target/consumed vertex/descriptor backing、stage push snapshot）、native GPU code/uniform/TMU/attribute/CL、transfer/barrier/queue/common worker/public binding。Master/共有投影はQ1。

### ordered draw stateのmain統合確認

source `9abe122bdc0844092631cad457e13d55954aa973` とQ1最新main `2bc2c5da5fde332dc1c23f5e697efae8e14edb76` のUSB source/記録を保持し、統合merge `42394d2393a10e7b6ec0dac2e335bb1966e5a6e5`。統合版actual host12範囲/named RPi4 y buildがexit0、warning/error0、arm64 check3 PASS（`build/ws141-i14-draw-state-integration-host.log`、`build/ws141-i14-draw-state-integration-y.log`）。main/専用branchを同SHAへfast-forward、cleanをreadback。Master担当変更無し、public binding/Keiland/実機/p007未達、i13/i14 in-progress。独立prepared primary/descriptor snapshotの実装へ継続。

## i14 immutable prepared CPU graphのsoftware出力（2026-10-09）

private `vulkan-prepared.h/c`とarm64 source列を追加。8MiB上限の有限CPU event snapshotsがbegin/draw/end、raw viewport/scissor、stage別push、draw scalars、exact compiled pipeline/vertex/pass selectionsを保持。独立primary reference→immutable owned record graphのedgesがpipeline/framebuffer/vertex/binding layoutのlifetimeを供給し、shader-consumed descriptorだけを各canonical slotへ一回cloneしてview/sampler/UBO intervalの独立ownersを追加。wire/arena/application pointerは保持しない。UBO contentをCPU prepare時に先読みせず、native workerで先行queue workが完了後のexact intervalから読む後続契約。

全snapshot/全distinct recorded set bookkeepingが完成した後、全counter overflowを先に検証してprimaryとordinary set pendingを一括charge。重複set bindは一primary一charge、simultaneous primaryは別prepared ownerごとにcharge。pending primaryのreset/destroyは先行guard、pending descriptor updateは先行guard、set freeのwhole-vectorとdescriptor pool reset/destroyの全child事前guardを追加。setがunusedでもordinary Vulkan binding flagsではpending mutationを許可しない。false retirementはprepared graphをそのまま保持しEBUSY、no launchまたは完了/checked resetが証明したtrue retirementだけ全charges/clones/rootを退役。actual submitted one-time lifecycleは後続queue acceptance/completionの責務、CPU prepare成功をGPU submission成功とはしない。

`sh plan/ws141/tests/vulkan-device-host-test.sh build/ws141-i14-prepared-host` と `... build/ws141-i14-prepared-final-host` → actual client/native sourceの13範囲PASS。actual prepared root/state/event/set allocation6箇所の全OOM prefix unwind、sampler reference overflow時の先行view unwind、set/primary pending overflowのcounter未変更、2つのsimultaneous prepared ownersの独立charges/retirement、normal primary重複準備EBUSY、exact cloned view/sampler/vertex/pushの保持、retired=falseの無解放、real ordinary update refusal、actual command resetとidle164→pending163のcomplete free batch拒否、descriptor pool reset/destroyの全identity未変更、final heap/reference復元を確認。prepared false-retirementは明示software fault inputで実DMA uncertaintyの証拠ではない。

formatter19/definition tab/style total0/diff0、ANSI/section/forward/short circuit/complete publication-before-charge/one root非cycle/clone-once mask/first cleanup error/counter protocol/full CPU heap lifetimeをreview。named RPi4 y build exit0、warning/error0、arm64 check3 PASS（`build/ws141-i14-prepared-final-y.log`）、SHA256 `edc090d1cd6b963206e381b7316c2ef7682bd91293a4c02cdbf9ecfebdddbb28`。後続build helperを抽出したchecked-call整理は意味変更無し、統合treeで最終sourceを再確認。private runtimeは未参照でGC除去、hashをnative GPU稼働証拠にはしない。

**worker接続時の残条件**: actual `render-worker.c`はexecute/disposeの後、failure時もfinish_requestでslot/callbackを退役し、retired=false payloadを自動再disposeしない。native disposerはprepared_release(false)だけ呼んでpointerを失ってはならない。future Vulkan runtime/controllerの明示quarantine listへprepared/native GPU storage owner全体をtransferし、logical callback終了とchecked reset後のDMA owner retirementを分ける。session closeも全typed live owner/session descriptorの寿命を保持し、EBUSYをfree成功に変換しない。既存worker/public HAL/UAPI変更をこのcheckpointで行わず、private actual queue/disposer bindingで実装する。Q1はshared投影のみ担当、方針質問は不要なin-scope ownership設計。

i13/i14/p005/p006 in-progress、COMMAND/CAPSET/JOB未公開、native GPU storage/code/uniform/TMU/fetch/CL、transfer/barrier/queue/common/public runtime、Keiland/実機/p007は未達。next fixed 4.2 packet schemaに基づく独立native shader/attribute/texture/sampler record生成とjob backing owners、actual FIFO executor/uncertainty quarantine/recovery。Master担当変更無し。

### prepared CPU graphのmain統合確認

source `fd027cac690f9177b45c2dbc2f7bfbb3073dc7ff` とQ1 main `7874438256b81d77effad05b50467d8961ae2a3d` を専用統合treeでmerge `f4cfe8d2ea5fb08bef64ba9c72b1e049fe41a007`。actual host13範囲/named RPi4 y build exit0、warning/error0、arm64 check3 PASS（`build/ws141-i14-prepared-integration-host.log`、`build/ws141-i14-prepared-integration-y.log`）、vmunix SHA256 `edc090d1cd6b963206e381b7316c2ef7682bd91293a4c02cdbf9ecfebdddbb28`。検証中に進んだQ1 record-only main `ba587ee6cd6294c8e6aea1a50e788936c4b54093` を保持してfinal merge `6730aa6684f3735274f6adedb132838c53e9c2c2`、source差分無しで追加build不要。main/専用branchを同SHAへfast-forward、cleanをreadback。Master担当編集無し（Q1作成記録のみ保持）。i13/i14 in-progress、次はnative packet/GPU backing/queue/public binding、Keiland/実機/p007未達。

## i14 native shader/fetch/texture recordとUIF変換のsoftware出力（2026-10-09）

- `native-state.h` / `native-shader.c` / `native-texture.c`をprivate driverへ追加。実GPU intervalを別途所有するcallerへ、4.2の36-byte shader /16-byte float attribute /24-byte texture・sampler recordを返す。MMIO/launch/allocation/public capability無し。失敗時はwhole output reservationを不変に保つ。
- ShaderはCOORD/VERTEX/FRAGMENTのexact stage、two-thread、uploaded code/uniformの全32-bit VA arithmeticを確認。共有VPM sectorはmax(input,output)の8 scalar rows/512 bytes、実IDENT1容量をcallerが供給する。stageごとのhalf VPMにlive output+2 cached batchesが入る時だけ許可。後続CLのVCMは2 batchesとする契約。default attributesは16×vec4の256-byte owned storageを要する。
- 固定MIT compiler資料と照合し、fragment RF0のreal centre Wをshader flagで明示。早期texture switchでscoreboardを取得せず、既存compilerのfinal switchでtile accessを開始。early Z無し、clip有り、implicit point/line varying無し。full standard/manual finite address/record reviewとstyle helper total0。
- 現resourceのlinear/optimal imageは共にraster backingであり、TMUへ直接そのpointerを渡さない。sampling用のstrict UIF scratchを実FIFO execution時、先行writeのcompletion+CPU visibility後に作る。single-level RGBA/BGRA、4×4 utile/8×8 UIF block/32px column、heightを8pxへpad。strict UIF+extended+explicit XOR disableでsmall imageのautomatic layoutとXORを使用しない。zero UB_PAD/全padding zero。native allocation/clean/retirement/quarantineとの接続は後続。
- 新資料5 fileのheader許諾とSHAをlicense auditに追加。固定Mesa XMLとMIT hardware事実のみ使用し、upstream source/objectをproductionへ取り込まない。
- scoped試験 `sh plan/ws141/tests/native-state-host-test.sh build/ws141-i14-native-state-final-host` exit0。全8 recordを固定hash XMLのfield定義から全byte比較。画像はproductionのforward offset式を使わずphysical block/utileの順に逆decodeし、37×131の4847 pixel、全zero native padding、元raster/row padding不変、alias・short span・short capacity・VA wrap・VPM不足時のatomic refusal/canaryを確認。これはnative fetch/filter/QPU executionの証拠ではない。p006未完了に必要な試験として同Phaseから参照し、WS終了時の整理はQ1。
- named build `make -j2 ZEDBSD_CONFIG=config/ci/config-rpi4.mk BUILD=build/ws141-rpi4-y CONFIG_DRIVER_BCM2711_GPU=y vmunix > build/ws141-i14-native-state-final-y.log 2>&1` exit0、warning/error0、arm64 check3 PASS。vmunix SHA256 `edc090d1cd6b963206e381b7316c2ef7682bd91293a4c02cdbf9ecfebdddbb28`。未接続helpersはGC除去されるため同hashはnative executionを示さない。nはsource list対象外、再build無し。
- pending: native code/uniform/TMU/attribute upload owners、graphics CL、transfer/barrier/queue/public/common binding、worker disposeのretired=false quarantine、実機Keiland/console RAM lifetime、p007。COMMAND/CAPSET/JOBは未公開。i13/i14/Phase/WSのwhole acceptanceはin-progress/incompleteを保持。Master/共有Queue/他WSは担当から更新しない。

### native record/UIFのmain統合確認

source `70e271e1c`をQ1 main `f2fd4c8b2`と独立integration worktreeで結合、`37c9472045beecbf17400086fbf117369be23d68`をmain/ownへff。integration `build/ws141-i14-native-state-integration-host.log` exit0（8 record/4847 pixels）、`build/ws141-i14-native-state-integration-y.log` exit0/warning/error0/arm64 check3 PASS。両checkout cleanを確認。他WS/Q1の変更を保持し、担当からMasterを編集していない。

## i14 native upload storageのsoftware出力（2026-10-09）

- `native-storage.h/c`追加。kernel-private cached allocationとactual native VAを独立所有。physical placementは実hardware PA bits（30..36）から決め、framebufferの1GiB制限やbus aliasを仮定しない。per allocation256MiB boundは既存buffer ownerと同じ。将来native job全体のscratch/code/uniform予算はexecution側で別途上限を確認する。
- exact compiled QPU numerical wordsをlittle-endianへ書き、全initialized page paddingを含むCPU clean後にnative code ownerを渡す。new helperはMMIO launch/public protocolを追加しない。codeを含む全VAとRAMはviewの独立referenceが保持する。
- `release(retired=false)`はEBUSYでwhole owner/mapping/PTE/RAM不変。trueはview referenceを一度消費してroot pointerをNULLにし、failed final unmap/flushは既存native space quarantineへ委譲する（errorでもcallerが再消費してはならない）。wrong spaceのreleaseは拒否。failed initial mapもspaceがzero-reference quarantined viewとRAMを保持。checked reset/recovery後にだけVA/physicalを再利用。pending prepared/native job全体を保持するcontroller quarantine listはまだ接続していない。
- `sh plan/ws141/tests/v3d-hardware-host-test.sh build/ws141-i14-native-storage-final-host > build/ws141-i14-native-storage-final-host.log 2>&1` exit0/PASS。actual native MMU/view/upload codeでcode bytes・zero page pad・complete clean・uncertain release不変・wrong space・normal release/null/idempotence・physical allocation refusal・final/initial flush failure・provider reset failureとchecked recoveryのreference/VA/RAM境界を確認。fixtureのbuffer modelをactual page roundingとidentified reachabilityへ追従。physicalGPU timing/fetch/QPU/SMP未検証。
- 初回host linkは先行i13のVulkan BLOB entry追加へallocation-only hardware fixtureが未追従だったため失敗。Vulkan sessionを開かない同fixtureではnonzero typed BLOBを明示的にENOTSUPへrefuseするstubを追加。actual typed Vulkan/BLOBは既存Vulkan-device host13の担当で、今回mock成功として代替していない。次のfixture assertionはprovider-reset失敗をEIOと誤期待し停止。既存provider fixtureのETIMEDOUT契約をsourceで確認してexpectedだけ修正し最終host PASS。
- named build `make -j2 ZEDBSD_CONFIG=config/ci/config-rpi4.mk BUILD=build/ws141-rpi4-y CONFIG_DRIVER_BCM2711_GPU=y vmunix > build/ws141-i14-native-storage-y.log 2>&1` exit0、warning/error0、arm64 check3 PASS。vmunix SHA256 `edc090d1cd6b963206e381b7316c2ef7682bd91293a4c02cdbf9ecfebdddbb28`（まだruntime未接続でGC除去）。new C/headerとchanged hardware fixtureのstyle補助total0、finite owner/consumption/manual全文確認。nの対象source無し、再build無し。
- 残件: exact uniform/descriptor pointer/TMU scratch/fetch GPU preparation、graphics CL、transfer/barrier/queue/common/public/runtime、whole-job retired=false quarantineとclosing session lifetime、実機Keiland/console RAM寿命、p007。i13/i14/p006 in-progress、WS incomplete。Master/共有投影はQ1。

### native upload ownerのmain統合確認

source `2a3b4939c`をQ1 main `9d2cca819`と独立integration worktreeで結合、`89cf1a8f90bb2fc4389799f3782144076b3ee8f2`をmain/ownへff。integration hardware host `build/ws141-i14-native-storage-integration-host.log` exit0/PASS、named y `build/ws141-i14-native-storage-integration-y.log` exit0/warning/error0/arm64 check3 PASS。main/own clean確認、他WS/Q1 authored source/records保持。native runtimeは未公開。

## i14 integer viewport uniformのsoftware出力（2026-10-09）

- `native-viewport.h/c`を追加。既存recordのx/y±4096・positive width/height≤4096・depth endpoints0..1/±zeroという6 copied IEEE word契約を再確認し、shader XY scale（half width/height×256 = dimension×128）、depth scale（max−min、reversed許可）、depth offset（exact min bits）をatomic outputへ生成。kernel FP命令/FP type/外部softfloat implementationを使わない。
- Normal XYはexact exponent shift、subnormal XYはzero低bitを保持してnormalへnormalize。Depthはunsigned significand+guard/round/stickyでnearest-evenを1回だけ行う。equal cancellation、reversed sign、normal/subnormal transition、(-0)−(+0)を保持。これはVulkan depth uniformの整数変換であり、一般softfloat package/APIを追加していない。
- [native-state host](tests/native-state-host-test.sh) を拡張、`sh plan/ws141/tests/native-state-host-test.sh build/ws141-i14-native-viewport-host` exit0。host `fesetround(FE_TONEAREST)` とvolatile floatの独立multiply/subtractをoracleとし、256 boundary pair＋1024 seeded finite pairで4 uniform wordの全bitsが一致。元のfixed XML8 record/4847 pixel変換も同scoped試験内でPASS。invalid depth/inf dimensionはwhole output unchanged。native QPU算術、floating precision/physical schedulingの証拠ではない。
- named y build `make -j2 ZEDBSD_CONFIG=config/ci/config-rpi4.mk BUILD=build/ws141-rpi4-y CONFIG_DRIVER_BCM2711_GPU=y vmunix > build/ws141-i14-native-viewport-y.log 2>&1` exit0/warning/error0/arm64 check3 PASS。target Cは既存`-mgeneral-regs-only`、shared LLVM未変更。private sourceのformatter19/definition tab/style helper total0/diff0とbounded shift/round/sign/output/manual review。公開runtime call-path/LTO total stackを含む全文最終確認はp007のまま。
- Native uniform streamへpush/constant/UBO execution-time readとowned texture/sampler addressを接続するのが次。native code/framebuffer/attribute/uniform/TMU storageのwhole-job owner・quarantine、graphics CL、transfer/barrier/queue/public/common、実機Keiland/console RAM寿命/p007は残る。i13/i14/p006 in-progress、WS incomplete、Master/共有投影はQ1。

### viewport integer loweringのmain統合確認

source `8b047c34f`をQ1 main `93e057939`と結合し、integration `fb442e680`で native-state/XML/pixel/viewport host exit0、named y build warning/error0/arm64 check3 PASS。検証中に追加されたQ1のrecord-only `aa0927a9f`（T1 requests/Master）を保持して `9f2209d42ea98642622bfb282124834616de1371`をmain/ownへff、双方clean確認。record-only mergeなのでsource再検証不要。integration logs `build/ws141-i14-native-viewport-integration-host.log` / `build/ws141-i14-native-viewport-integration-y.log`、1280 arithmetic casesはhost内logにPASS。vmunix SHA256 `edc090d1cd6b963206e381b7316c2ef7682bd91293a4c02cdbf9ecfebdddbb28`（unbound/GC、native execution proofではない）。

### 次のnative uniform owner接続の設計メモ

同p006内のroutine lowering: prepared drawのstage別pushコピー、compiled constant bits、viewport helper、cloned exact UBO intervalから実FIFO execution時のword read、independently owned 32-byte aligned texture/sampler record addressとcompilerのlow config bitsをexact consumption順に生成する。ordinary pending graphを保持し、whole streamをCPU stagingへ完成してからnative storageへupload/cleanする。public identityの再lookupやsubmit-time UBO内容snapshotは使わない。全CPU/GPU storageをwhole-job rootに保ち、failed retired=false disposerでcontroller quarantineへtransferする契約とclosing session寿命を接続前に完成する。まだその新module/queue/launch/public bindingは未実装。

## i14 FIFO scalar uniform streamのsoftware出力（2026-10-09）

- private `vulkan-uniform.h/c`追加。real prepared drawのcompiled coordinate/vertex/fragment consumption順にconstant bits・stage別copied push・integer viewport・exact cloned UBO word・borrowed texture/sampler address+low configを生成する。whole CPU streamを独立所有し、失敗は全unpublished prefixを解放。UBOはFIFO execution時に読む契約で、submit時の内容snapshotやCPU source pointerの保持はしない。
- immutable pipeline/set layoutのcanonical binding number/typeとconsumed clone maskを確認。UBOはtyped buffer・descriptor logical range・actual coherent backingのlogical buffer4bytesを確認しlittle-endianで読む。GPU descriptor recordのVAはnonzero32align/whole24bytesを確認するだけで、そのallocation/DMA lifetimeはwhole native job ownerの責任。stream上限1MiB、empty streamはCPU array無し、native upload時placeholderは後続。
- `sh plan/ws141/tests/vulkan-device-host-test.sh build/ws141-i14-native-uniform-final-host > build/ws141-i14-native-uniform-final-host.log 2>&1` exit0、14範囲PASS。actual client pipeline/3 compiled programs/prepared cloneのwhole uniform順序、OOM root/array、late sampled address refusal、実buffer101のcoherent backingを使うsynthetic immutable UBO metadataの4byte/8byte読み取り、後続書き込みによる次stream更新と旧stream不変、descriptor7byte/logical65byte端/quarantined view/unused clone拒否、heap baseline復元を確認。numeric GPU descriptor addressesとUBO metadataは明示fixtureで、native record mapping/queue launch/実GPU executionの証拠ではない。
- 最初のoracleはfixture viewport maxDepthを0と誤期待しassert停止。actual client sourceのmaxDepth=1を確認してexpected depth scaleだけ訂正、production変更無し、最終PASS。failed coreは削除/ptraceしない。
- named y build `make -j2 ZEDBSD_CONFIG=config/ci/config-rpi4.mk BUILD=build/ws141-rpi4-y CONFIG_DRIVER_BCM2711_GPU=y vmunix > build/ws141-i14-native-uniform-y.log 2>&1` exit0、warning/error0、arm64 checks PASS。SHA256 `edc090d1cd6b963206e381b7316c2ef7682bd91293a4c02cdbf9ecfebdddbb28`、未接続GCのためnative execution proofではない。formatter19/definition tab/style helper total0/diff0、全文のowner/late error/success-last/ANSI/call separationをscope内確認。n対象source無し、再build無し。
- 次: native uniform/code/TMU/fetch GPU preparationとwhole-job root/quarantine、graphics CL、transfer/barrier/queue/common/public、closing session lifetime。Keiland/実機/console RAM寿命/p007未達、i13/i14/p006 in-progress・WS incomplete、Master/共有投影はQ1。Phase内部のlowering、HAL/UAPI/foreign dependency変更無し。

### FIFO uniform CPU loweringのmain統合

source `e110205f5`をQ1 main `167441271`（Master/WS199のrecord-only変更）と結合し、`09badd393`をmain/ownへff、双方clean確認。production全roots `src include platform userland config`はhost/build確認済みsource commitと差分0を確認し、record-only mergeなのでoptional再試験を行わない。Q1 authored recordsを保持、担当からMasterは編集しない。

### 次のnative draw GPU preparation設計

同p006内部: FIFO時点でsampled rasterをstrict UIF scratchへ変換し32align descriptorを独立upload、各stageのcode/uniform、canonical vertex float fetchとdefault values、shader+attribute recordsを1draw rootのmapped storage群として所有する。頂点はlogical source intervalを確認して必要scalarをprivate packed streamへコピーし、firstVertexをsource側で適用してnative array drawを0へrebase（VertexIndex builtinは既存compiler非対応）。formatよりshader幅が大きいfloat入力の欠けたcomponentはVulkanの0/0/0/1を補う。coordinate/vertexの現compilerは同じcanonical input metadataを使用するため相違は拒否。job全体native staging上限256MiB、全page paddingを含める。prepareはMMIO/job launch無し、retired=falseは全mapped群/root保持、trueはfailed mapping teardownをspace quarantineへ渡してroot参照を一度消費する。controllerのwhole prepared owner/quarantineとclosing sessionはruntime接続前に完成する。

## i14 whole native draw preparationのsoftware出力（2026-10-09）

- `vulkan-native-draw.h/c`を追加。FIFO時点でconsumed sampled rasterをstrict UIFへcopy/clean、texture/sampler32align records、actual compiled3 codeとscalar uniforms、(0,0,0,1)16 defaults、canonical packed vertex input、shader+attribute recordをrootのindependent mapping群として保持。public identityやcommand arenaをnative storageへ残さず、enclosing jobがprepared primary/pending chargesをDMA完了まで保持する契約。新rootはMMIO/CL launchを行わない。
- source firstVertexをlogical resource側で適用し、private copied fetchをnative firstVertex0へrebase。stride/source format/last full vertexを64bitで確認、全shader inputsが同じcanonical C/V FIFO metadataであることを再確認。packed inputはconsumed componentのみ、欠けたfloat format componentは0/0/0/1。native maximum indexはcopied vertex count−1でpaddingに入らない。shader recordのVPMはactual IDENT1から取得。
- [Vulkanのvertex input specification](https://docs.vulkan.org/spec/latest/chapters/fxvertex.html) のcomponent default契約に合わせ、先行pipeline validatorの「未実装なのでR32→vec2を拒否」という限定を今回の実装に追従。real pipeline buildでもR32 input→actual quad vec2を受け付け、全program/parentを正常retireするhostを確認。synthetic narrower-fetch fixtureでは実logical coherent sourceから欠けたsecond wordをzero補充し、隣のwordを読まないことを確認。32bit float以外/undefined bindingは引き続きrefuse。
- native staging上限はjob全体256MiB、rootごとに全4096byte paddingをcharge。available budgetは全root成功時のみ減り、late errorは不変。whole fixed owner arrayは48 canonical sampled slots×2＋3 code/uniform pairs＋default/fetch/shader=105 mappingを上限。retired=falseはroot/全storage不変、trueは全mappingを一度消費し、failed translation teardownはnative spaceが保持。workerのwhole pending payload/session quarantine接続は後続。
- `sh plan/ws141/tests/vulkan-device-host-test.sh build/ws141-i14-native-draw-final-host > build/ws141-i14-native-draw-final-host.log 2>&1` exit0、15範囲PASS。real compiler/prepared graphとactual MMU/view sourceで11 upload owners/45056 padded bytes/11 complete cleanを確認。host cached RAMは独立CPU allocationとunique numerical PAのfixture、physicalGPU/cache成功の代替ではない。exact compiled code bytes、shader/attribute pointers/count/stride/max index、48 logical fetch bytes/全zero padding、source mutation後のowned raster/fetch不変、retired=false保持→true全解放、4selected late OOMのmapped prefix unwind、job budget部分取得後refusal、firstVertex3のexact後半24bytecopy、logical fetch end拒否とheap復元PASS。
- named y `make -j2 ZEDBSD_CONFIG=config/ci/config-rpi4.mk BUILD=build/ws141-rpi4-y CONFIG_DRIVER_BCM2711_GPU=y vmunix > build/ws141-i14-native-draw-y.log 2>&1` exit0/warning/error0/arm64 checks PASS。未接続private modulesはGC除去、vmunix SHA256 `edc090d1cd6b963206e381b7316c2ef7682bd91293a4c02cdbf9ecfebdddbb28`、実GPU execution証拠ではない。formatter19/definition tabs/style total0/diff0と全文のbounded ownership/late rollback/canonical ordering/zero padding/FP無しをscope内確認。n source無し、再build無し。
- 次: graphics BCL/RCL/pass/tile/framebuffer owners、transfer/barrier/queue/common/public binding、whole prepared+native job controller quarantineとclosing session lifetime。実機Keiland/console RAM lifetime/p007未達。p006/i14 in-progress・WS incomplete、Master/共有投影はQ1。Phase内部prepared lowering、foreign interface/HAL/UAPI変更無し。

### Native draw ownerのmain統合

source `e3f95ae81`をQ1 main `0eacd4a80`（fidoctl sourceと他WS records）と独立integration treeでmergeし、`725d9f0e4`をmain/ownへff、双方clean。kernel全rootsとfixture依存libvulkan/Keiland shaderは検証版とdiff0、fidoctlは対象kernel/hostの依存に含まれないためoptional再buildを行わない。Q1 authored変更保持、Master担当編集無し。

### Graphics CL準備のreviewで判明した4.2制約

固定MIT Mesa `v3dvx_cmd_buffer.c`のnative packet手順を再確認。GFXH-930ではinput無しでもCS/VSへ1 attribute readが必要なため、native drawのzero-input recordにowned defaultsを使うstride0・1 scalarのdummyを追加（compiled programは値を消費しない）。viewport offsetはunsigned fine u14.8＋signed coarse64px units、negative centerの補正を要する。clipperのzero/small Z scaleは4.2 guardband問題に対するmin0.0005を必要とする。shader depth出力はVulkanのexact rangeを維持し、clipper packet側だけhardware有効rangeを使用する設計。これらは同p006内のhardware command実装で、public/HAL/foreign dependency変更無し。

## i14 native draw BCLとclipperのsoftware出力（2026-10-09）

- `native-bin.h/c`と`vulkan-native-bin.c`追加。whole native draw ownerが116byteのcomplete BCL stateをコピー所有する。point/line/sample、drawable/render-area/viewport/scissor intersection、clipper XY/depth/planes/offset、facing/first provoking vertex、RT0 write mask、hardware blend/feedback/query disable、全32 varying flags/centroid reset、VCM2 batches、owned shader pointer+attribute count、rebased triangle-arrayを全byte生成。still pass全体BCL/RCL/launch無し。
- native viewport helperに4.2 clipper用のpure integer計算を追加。half dimensionとcentreをnearest-even、unsigned u14.8 fine＋signed64px coarse（negative補正）をnative fixed ties-awayで生成。viewport edgesはIEEE centre±half後のinteger truncation。4.2 clipper zero/small depth guardbandにはmin0.0005/actual reverse signを適用し、そのnative transformのplane boundsをonce-roundで計算。shaderのexact Vulkan depth scaleは不変。depth-disabled single-colour範囲のnative clipping workaroundであり、physical precision/CTS acceptanceは未確認。
- zero-input native drawではGFXH-930のmandatory unused CS/VS attributeをowned defaultsからstride0/1 scalarで読み、extra allocation不要。固定MIT `v3dvx_cmd_buffer.c` SHA256 `8e314e2c49360193d888de3ad07b0a2755c63def7dab94c31893007fac561e9c`を再確認、既存license tableと一致。hardware fields/手順の事実を独立実装、upstream packing code/source/objectをproductionへ取り込んでいない。
- `sh plan/ws141/tests/native-state-host-test.sh build/ws141-i14-native-bin-final-host > build/ws141-i14-native-bin-final-host.log 2>&1` exit0。116byte whole BCLを固定hash XMLから全field/全byte比較（flags24境界/negative coarse/fixed offsets/relocations/facing/masks/cache/draw lengths）、capacity/VAwrap/dummy omission時のatomic bytes保持。clipper host independent IEEE/ceil/round/edge oracleで288 boundary＋512 fixed cases PASS、従来1280 shader arithmeticと8record/4847pixelも同scoped fixture内PASS。初回expectedはtiny negative subnormalをfloatで/64しunderflow0としてcoarse0を誤期待。数学的ceilのoracleだけdoubleへ修正し、sourceのcoarse−1/fine64（effective zero）と一致。source fallback/ptrace/削除無し。
- `sh plan/ws141/tests/vulkan-device-host-test.sh build/ws141-i14-native-bin-final-device-host > build/ws141-i14-native-bin-final-device-host.log 2>&1` exit0、actual Vulkan/compiler/MMU host15範囲PASS。real rootのwhole BCL length/shader VA+count/clip16×8、synthetic scissor4×3とoff-drawable empty window、zero-input synthetic compiler metadataのdummy pointer/default root/count10/stride0/read1を確認。synthetic metadataは実QPU executionではない。
- named y `make -j2 ZEDBSD_CONFIG=config/ci/config-rpi4.mk BUILD=build/ws141-rpi4-y CONFIG_DRIVER_BCM2711_GPU=y vmunix > build/ws141-i14-native-bin-y.log 2>&1` exit0/warning/error0/arm64 checks PASS、SHA256 `edc090d1cd6b963206e381b7316c2ef7682bd91293a4c02cdbf9ecfebdddbb28`（unbound/GC、execution証拠ではない）。formatter19/definition tabs/style0/diff0、full scoped integer shifts/bounds/source alias/atomic state/canonical flags/manual owner確認。n対象変更無し。
- 次はpass BCL/RCL/tile pool/state/overflow/output ownerへこのdraw sequenceを接続、load/clear/storeとtile workaround、transfer/barrier/queue/public/runtime、whole job/session quarantine。Keiland/実機/console RAM寿命/p007未達。i14/p006 in-progress、WS incomplete、Master/Q1投影未変更。同Phase内部、foreign commitment/HAL/UAPI変更無し。


### Native draw BCLのmain統合

source `272234f57`をmain/own/integrationへffしてclean確認。固定XML/actual prepared graph/clipper hostとnamed y buildで確認済みのsourceそのものを統合した。Master/共有record担当変更無し、push無し。後続Q1の他WS更新は次の独立integrationで保持する。

## i14 whole native pass CLとGPU ownerのsoftware出力（2026-10-09）

- private `native-pass.h/c`追加。single-colour RGBA8・single-layer/sample・64×64tileのwhole BCL/RCL/generic tile listを生成する。BCLは全owned draw sequenceを保持してlayer/config/cache/start/flushを付加。RCLはCommon first/clear/type/ZS last、initial block64、supertile config、GFXH-1742の2dummy NONE stores/初期clear/cache、pool set0/generic start-end、選択supertile/endを生成。genericはoptional raster load→EndLoads→triangle list→SetInstanceID0→implicit branch→store or NONE→colour/ZS clear→EndTile/Return。fragment compilerのBGRA変換と二重swapしない。
- 固定MIT `v3dv_cmd_buffer.c`のPTB pool（全tile×64を4096align＋8192＋512KiB）、TSDA（全tile×256）と`v3dvx_cmd_buffer.c`のcommand手順/回避策を再照合。supertilesにはhardware総数256の制限があるため、独立generatorはsquare power-of-two groupingで1/2/4tile単位を選ぶ。4096×4096では64×64native tilesを16×16groups of4×4へまとめる。selected groupsがrender area外のtileを含む場合、runtimeは全selected tileをLOADしてoutside samplesを保持する。固定hardware fields/factsの独立実装で、upstream packing/source/object取り込み無し。
- private `vulkan-native-pass.h/c`追加。actual immutable BEGIN〜END eventの非zero drawをFIFO時点でnative rootへ連結し、0vertices/0instancesはGPU input read無しでskip。output coherent viewをchecked independent retainし、copyした数値target/area/clear stateと9storage（BCL/RCL/generic/pool/state＋4×256KiBoverflow）を所有。all padded pagesとdraw inputsはsubmissionの256MiB aggregate budgetから取得。late errorは全prefixをretireしてcaller budget/cursor/target contentsを保持。retired=falseはwhole root/全draw/command/overflow/outputを不変保持、trueはmapping teardownの初回error後も全prefixを消費しfailed unmapはnative spaceが保持する。
- actual passはLOADを常に使い、部分render-areaとgroup内隣接tileのoutside samplesを保持する。CLEARのexact raw float union/areaをCPU rootへコピーするが、target clear/normal native launchはまだ未接続。whole preparation/OOM中にtargetを変更しない。後続executionでwhole pass preparation成功後にrectangular coherent clearし、CL launch/visibility/retirementを処理する。whole worker payload/controller quarantineとclosing-session lifetimeは接続前の必要条件のまま。
- `sh plan/ws141/tests/native-pass-host-test.sh build/ws141-i14-native-pass-final-host > build/ws141-i14-native-pass-final-host.log 2>&1` exit0。fixed hash XML oracleで7whole streams（load bin/render/tile、clear bin/render/tile、discard tile）の全byte/relocations/workarounds一致。全draw byte preservation、capacity/tile/output/pool/state不足、complete native/CPU alias/wrap拒否時byte不変/active0、max4096×4096の256groupsによる4096tile coverage/サイズを確認。初回oracleのXML field大小文字を修正、照合でZS clear raw word位置の1byte誤りをproduction修正。最初の1×1supertiles最大4096案はfixed hardware上限確認で撤回し、final groupingへ修正した。physical execution証拠ではない。
- `sh plan/ws141/tests/vulkan-device-host-test.sh build/ws141-i14-native-pass-final-device-host > build/ws141-i14-native-pass-final-device-host.log 2>&1` exit0、16範囲PASS。actual Keiland compiler/prepared graph/MMU/storage sourceから1native draw＋9pass owners、全20upload clean、1,646,592padded bytes、130byteBCL/106byteRCL、pool/TSDA/4overflowのexact job pointers、独立output reference、CLEAR preparationでも全512target byte不変を確認。false whole retain→true全退役、selected late OOM10/35/60、pool前budget exhaustion、synthetic0vertex/0instanceとmissing END、output retain overflow、全prefixheap/reference/budget復元PASS。host cached RAM/PA/cache observationはfixtureで、physical DMA/QPU/Keilandの成功証拠ではない。
- named y `make -j16 ZEDBSD_CONFIG=config/ci/config-rpi4.mk BUILD=build/ws141-rpi4-y CONFIG_DRIVER_BCM2711_GPU=y vmunix > build/ws141-i14-native-pass-y.log 2>&1` exit0/warning/error0/arm64 checks3 PASS。SHA256 `edc090d1cd6b963206e381b7316c2ef7682bd91293a4c02cdbf9ecfebdddbb28`（unbound private/GC、native execution証拠ではない）。formatter19/definition tabs/style total0/diff0、full scoped bounds/ownership/rollback/non-FP/atomic publicationを確認。n対象source無し、optional再build無し。
- 固定MIT sources SHA256を既存license tableと再確認: `v3dv_cmd_buffer.c`=`eaf3c553f3fcd8e26213981d05ae94b88635684b1a5b360d35271424d0861f6d`、`v3dvx_cmd_buffer.c`=`8e314e2c49360193d888de3ad07b0a2755c63def7dab94c31893007fac561e9c`、`v3d_limits.h`=`a4ccb6b77ccc2862c2a56fd49bc461fdab24bb4d6dee28a08d872f7b1220856b`。whole WS full-standard/license/similarity p007は最終runtime接続後に実施する。
- 次: deferred clear/native CL execution、transfer/barrier/queue/public/common binding、whole prepared+native-pass job/controller quarantineとclosing session lifetime、final runtime LTO stack/p007。i13/i14/p005/p006 in-progress・WS incomplete、実機/Keiland/console RAM寿命未達、COMMAND/CAPSET/JOB未公開。HAL/UAPI/foreign ownership契約変更無し、Master/共有投影とGitHub公開はQ1へ保留。


### Whole native passのmain統合

source `c48a96952`をQ1 main `ac73b6915`と独立integrationで結合、`76d6afb67df0c52e37bf7a2bdb626b293eb3d320`をmain/ownへffして双方clean。統合版native-pass/XML host・actual Vulkan host16範囲・named y build `-j16` exit0/warning/error0/arm64 checks3 PASS。logs `build/ws141-i14-native-pass-integration{,-device}-host.log` / `build/ws141-i14-native-pass-integration-y.log`、vmunix hashはsource checkpointと一致（private GC）。Q1のsystem UAPI/smartcard・userland/recordsの変更を保持。Master担当変更無し、push無し。続きはexact deferred clear→native runnerとwhole-job/session quarantineを含むruntime。

## i14 deferred clearとnative pass executorのsoftware出力（2026-10-09）

- `native-colour.h/c`追加。raw IEEE4componentsをclamp/nearest-evenのexact integer UNORM8へ変換、RGBA/BGRAのstorage wordをatomic publishする。negative/−0→0、positive infinity/finite≥1→255、NaN→deterministic0。no FP、overflow/oversized shifts無し、source/output aliasを全component read後の一回publicationで扱う。fragment blending/rasterizationはGPUのまま。
- `vulkan-native-execute.c`追加。controller mutex下で全command/overflow/output/draw viewとnative ready/power、exact coherent CPU/native binding、全row/area boundsをpreflight。whole pass preparation成功後にだけCLEARのexact rectangleへexplicit LE bytesをwrite、publication barrier→existing `bcm2711_v3d_job_run`→native resultのretirementを保持、成功時output read barrierを実行する。LOAD/DONT_CAREはCPU clear無し。rootはsingle-use、completed/uncertainどちらの再実行もEBUSY、uncertain replayはretired=falseを保つ。retired=falseではcallerがwhole payload/controller quarantineへtransferする責務を維持し、callback終了をDMA retirementとみなさない。
- `sh plan/ws141/tests/native-colour-host-test.sh build/ws141-i14-native-execute-final-colour-host > build/ws141-i14-native-execute-final-colour-host.log 2>&1` exit0。independent host double IEEE oracleで1037cases（16edge/765quantization threshold neighbours/256fixed finite）、distinct RGBA/BGRA、atomic refusal/output alias一致。
- `sh plan/ws141/tests/vulkan-device-host-test.sh build/ws141-i14-native-execute-final-device-host > build/ws141-i14-native-execute-final-device-host.log 2>&1` exit0、17範囲PASS。actual prepared framebuffer/draw/storage ownerとsynthetic copied3×2clear areaをreal private executorへ渡す。faulted admission/malformed areaのno target change/no handoff、exact6pixels RGBA(64,128,191,255)/全surrounding512bytes保持、barrierとnative runnerへのexactCL handoff、single-use再clear/再launch拒否、LOADでCPU不変、synthetic timeout/retiredfalse時whole allocation/linkeddraw保持とreplayfalseを確認。**native job runnerだけは明示host fixture**で、physical IRQ/cache/DMA/QPU/checked-resetの成功証拠ではない。fixtureのtrue releaseはhost DMA無しによる限定proof。
- named y `make -j16 ZEDBSD_CONFIG=config/ci/config-rpi4.mk BUILD=build/ws141-rpi4-y CONFIG_DRIVER_BCM2711_GPU=y vmunix > build/ws141-i14-native-execute-y.log 2>&1` exit0/warning/error0/arm64 checks3 PASS、SHA256 `edc090d1cd6b963206e381b7316c2ef7682bd91293a4c02cdbf9ecfebdddbb28`（private unbound/GC、実行証拠ではない）。formatter19/definition tab/style total0/diff0、full scoped numerical conversion/no FP/source alias/whole-owner lifecycle/clear bounds/one-past final-row pointer/conditional ordering確認。n対象無し。
- 次: whole prepared+native pass submission/controller quarantineとclosing-session lifetime、transfer/barrier/queue/public common binding、final LTO runtime stack/p007。COMMAND/CAPSET/JOB未公開、native execution physical/Keiland/console RAM寿命未達、i13/i14/p005/p006 in-progress・WS incomplete。Master/共有投影はQ1、HAL/UAPI/foreign ownership変更無し、push/GitHub公開無し。

### Private native executorのmain統合

source `800cf2e278c03d885eba5cf54acce7404fdbe979`をmain/integrationへff、3tree clean。main基点は検証版と同じ`76d6afb67`で、source同一のためoptional再試験を追加しない。host1037/17範囲/y buildのsource evidenceを保持。Master担当編集無し、push無し。

### 次のwhole pending job/session retirementの設計

同p005/p006の内部runtime契約: pending primary/descriptor clones/current native pass/outputをone payloadで保持し、worker dispose(false)はCPU rootをpersistent controller quarantineへallocation無しでtransferする。common callback終了後もwhole graph/namespace/render sessionが保持される。renderer closeはregistryをwithdrawし、まだtyped ownerが残る閉じたsessionをinternal listへ残すがexternal sessions数は減らしresetを可能にする。checked native reset→whole payload退役→closed Vulkan/session退役→native-space translation recovery→worker admission reopenの順を使う。public COMMAND/CAPSET/JOBはこのlifetimeとtransfer/queue/runtimeが完成するまで未公開。routine internal owner completionでHAL/UAPI/foreign契約変更無し。


## i14 whole pending native jobとclosed rendererのsoftware出力（2026-10-09）

- `vulkan-native-job.h/c`追加。one pending primaryのdescriptor clone charges/current native pass/outputをCPU rootへ保持、FIFOのpass単位で前passのnative retirement後に次passのcoherent bytesをstageする。resident padded budget256MiB、single-use replay拒否、submit前OOMはpending/ref/allocationを戻す。worker dispose(false)はallocation無しでwhole rootをcontroller quarantineへtransfer、callback/slot終了でDMA ownerを失わない。
- actual renderer closeはnamespace registryをwithdraw、typed ownerが残るclosed rendererを内部listへ保存してexternal sessionsを減らす。checked native hardware reset成功後にwhole payload→closed namespace/renderer→native-space translations→worker admissionを退役/回復する。reset失敗ではlogical rootsを消費しない。private production Vulkan owner/COMMAND/CAPSET/JOBはまだ未接続。
- `sh plan/ws141/tests/vulkan-device-host-test.sh build/ws141-i14-native-job-device-host > build/ws141-i14-native-job-device-host.log 2>&1` exit0、18範囲PASS。actual prepared/compiler/MMU ownersでOOM1/2/3、successful pass＋pending disposal、replay拒否、explicit synthetic runner timeout後のwhole-root transfer/idempotence、external sessions/failed-ready回復拒否、synthetic recovery後の全pending/ref/budget退役を確認。初回fixtureのpower/initializedが未設定だったassertをfixtureだけ修正。native execution/resetのphysical proofではない。
- `sh plan/ws141/tests/v3d-hardware-host-test.sh build/ws141-i14-native-job-hardware-host > build/ws141-i14-native-job-hardware-host.log 2>&1` exit0。actual render open/close/hardware resetとtyped session/object codeでclosed registry withdrawal/arena retention、provider reset failure時保持、最後のtyped destructor中render owner alive、checked modeled reset成功後closed list消費を確認。actual recovery依存のdescriptor release sourceをlinkし、unused compiler pathsはsection GC。初回link不足をdefining source追加で修正。physical MMIO/DMA/IRQ timingのproofではない。
- named y `make -j16 ZEDBSD_CONFIG=config/ci/config-rpi4.mk BUILD=build/ws141-rpi4-y CONFIG_DRIVER_BCM2711_GPU=y vmunix > build/ws141-i14-native-job-y.log 2>&1` exit0、warning/error0、arm64 checks3 PASS。SHA256 `9af5a31668ce014c963ee0192e6ee3f42420e5d2dc6ae967774a761142563bb4`。close/reset referencesの追加で以前のprivate GC hashから変化したが、native execution/Keiland証拠ではない。formatter19/definition tabs/style total0/diff0、full scoped ownership/destructor/session lifetime/checked reset ordering/rollbackを確認。n対象無し。
- 次: transfer/barrierとprimary queue submit、public dispatch/common binding、final runtime LTO stack/full-standard/license/similarity p007。p005/p006/i13/i14 in-progress、WS incomplete、Keiland/実機/console RAM寿命未達。共有Master/Queue投影/GitHub公開はQ1に保留、HAL/UAPI/foreign source変更無し、push無し。


### Whole native jobのmain統合

source `8aceafd03`をQ1 main `cbd140860`とintegration worktreeで結合し、`3e497a178e07fc2ec0b235c97c3d06314b93233d`をmain/ownへff。source checkpointとintegrationの`src/include/platform/config/toolchain/plan/ws141/tests`は全て同一とgit diffで確認し、optional同一kernel再試験を追加しない。Q1のpasskey/sessiond/Keiland userland・Master・各WS記録を保持、Master独自編集無し、push無し。host18/close-reset/y evidenceを保持。

### Explicit barrier/implicit pass layoutの設計

p006内部の記録/準備/runtime出力: barrier専用derived record nodeはwhole recording1MiBをgraphicsと共有し、memory/buffer/image combined64 entries（Keiland batch32含む）をheap上へcomplete copyしてtyped objectを独立retainする。録画/preparationではold layoutを推測せず、FIFOで全imageのcurrent layout/actual coherent backingをpreflightしてからvisibility barriersとlayout publicationを実行。native render passはsuccessfully retired後にimplicit finalLayoutをcommitし、explicit initialLayoutは実行前にチェック。unsupported queue-family transfer/in-pass/feature scopes/extension chainsは普通のvoid failureとしてEndへ伝え、recording failureは最初のものを保持。公開COMMAND/JOBは全runtime完成まで未接続。


## i14 explicit barrierとimplicit pass layoutのsoftware出力（2026-10-09）

- `vulkan-barrier.h/c`・`vulkan-barrier-decode.c`・`vulkan-barrier-validate.c`追加。実clientのreply-free barrier framingを全copyし、same-device bound buffer/full-colour imageを記録nodeが独立retain。combined64 entriesをheapで扱い、Keiland32image batchを包含。shared whole primary1MiB recording budget、first void errorを保持、unsupported stage/access/queue transfer/extension/in-pass、duplicate image transitionsを拒否。global/whole-size buffer/remaining mip-layer scopesを保持する。
- prepared eventは独立pending primaryに保持されたimmutable recordを指し、native jobのFIFOにbarrier実行を追加。全targetのexact coherent backing/current oldLayoutをチェックした後にpublication/read barriersとnewLayoutをcommit、最後の対象の失敗でも先のlayoutを変えない。UNDEFINEDのsourceはdiscard選択としてcurrent mismatchを要求しない。render passは明示initial layoutをlaunch前に確認、成功してGPU DMA/output visibilityが退役した時にimplicit final layoutをcommit。faulted native admissionはCPU layout publicationより前に拒否する。
- [Khronos Vulkan synchronization正本](https://docs.vulkan.org/spec/latest/chapters/synchronization.html)のexecution/memory dependencyとImage Layout Transitionsを2026-10-09に確認。先行operation完了だけでなくavailability/visibilityも必要、oldLayoutはUNDEFINEDまたは現在値。独立normal-NC/whole native GPU cache退役/owned input staging/IO publicationでこのsubsetを実装する。physical cache/dma/IRQの証明は実機待ち。
- `sh plan/ws141/tests/vulkan-device-host-test.sh build/ws141-i14-barrier-device-host > build/ws141-i14-barrier-device-host.log 2>&1` exit0、19範囲PASS。**actual public vkCmdPipelineBarrier encoder**→typed immutable recording→independent prepared/native jobを検証。global memory/whole-size buffer/two colour imageの所有、末尾layout mismatch時全対象不変/no barriers/no launch、成功後全layout/pending charges/retirement、duplicate transitionsのEnd failure/no partial retainを確認。初回fixtureのtexture image/view identity180/181取り違えを修正。UNDEFINED sourceで意図したlate mismatchが合法discardとして成功した試験を、explicit prior layoutのfixtureへ修正。productionのUNDEFINED semanticsは変更無し。新hardware-typed view include不足は正しいprivate defining headerで解消。
- `sh plan/ws141/tests/v3d-hardware-host-test.sh build/ws141-i14-barrier-hardware-host > build/ws141-i14-barrier-hardware-host.log 2>&1` exit0。actual close/reset hostとnew recovery link dependencies PASS。native runnerはVulkan fixtureでexplicit mock、physical実行証拠ではない。whole pending graph/native reset lifetimeの結果を保持。
- final named y `make -j16 ZEDBSD_CONFIG=config/ci/config-rpi4.mk BUILD=build/ws141-rpi4-y CONFIG_DRIVER_BCM2711_GPU=y vmunix > build/ws141-i14-barrier-final-y.log 2>&1` exit0、warning/error0、arm64 checks3 PASS。image SHA256 `f8a0acb6cf38c36d7793bb3556e9bc773961573c86604deb3491e4d59b8c2fac`。formatter19/definition tabs/style total0/diff0、manual typed-node prefix/independent edge rollback/first-error/atomic layout/whole pending lifetime/kernel stack heap/non-FPを確認。first-semantic-error preservationの微修正後もfinal build PASS、optional全host再試験無し。n対象無し。
- 次: native transfer/copy/clear、primary QueueSubmitとpublic dispatch/common COMMAND/JOB/CAPSET binding、final runtime LTO stack/full-standard/license/similarity p007。p005/p006/i13/i14 in-progress、WS incomplete。Keiland表示/実機/console RAM寿命未達、COMMAND/CAPSET/JOB未公開。Master/共有投影/GitHub公開はQ1に保留、HAL/UAPI/foreign source変更無し、push無し。

### Explicit barrierのmain統合とnative transferの再開点

source `8c132b488`をQ1 main `0978d813e`と独立integration worktreeで結合、`cf3a6b70e509efea252c83f8ae5972677e0fed02`をmain/ownへff。`src/include/platform/config/toolchain/plan/ws141/tests`はsource checkpointと同一とgit diffで確認し、optional同一kernel再試験を追加しない。Q1のpasskey/Keiland userlandとMaster/各WSの変更を保持、Master独自編集無し、push無し。

次のnative transferレビューで、actual `libvulkan/wsi-swapchain.c`は同family0だけでなく、external共有targetに対して0↔`WSI_QUEUE_FAMILY_EXTERNAL`（0xfffffffe）のownership acquire/release barrierを記録することを確認。現在のprivate barrierはこれをENOTSUPにしており、public runtime完成前にopaque external memoryのexact same-allocation/coherent visibility境界として実装する残件。Keiland普通のwindow sampling barrierはignored pair、batch上限32。actual WSIはsame-size copy/image-to-buffer、size/format変換時nearest blit、mirror headはlinear blitとblack clearを使う。`vulkan-query.c`のBLIT feature bitsはまだprivate/unboundだが、公開時までにnative GPU loweringと一致させる必要がある。CPUによるdisplay copyをGPU copyとして成功させる短絡はしない。native pass/compiled shader/TMU input staging/whole-owner rootをmeta transferにも利用し、FIFO source visibilityとfalse-retirement quarantineを維持する。TFUは現在aligned packed raster→tiled conversionだけなので、arbitrary raster region/linear blitの代用として無検証で使わない。


## i14 external-family共有メモリのadmission checkpoint（2026-10-09）

- actual WSIの0↔external-family（0xfffffffe）をprivate barrierへ追加。same-device bound resourceのmemoryがexternal用として宣言/実importされたものだけ受け入れ、普通のprivate allocationではEINVAL、unrelated family/foreign/mixed ignored indicesはENOTSUP。native completion/output visibility後のexact coherent backing/read-write barriersとworker callback/timeline orderingを外部境界でも維持する。metadataはtyped graphに保持、CPU書き換えや新DMA launchを追加しない。public runtime/fence integrationは後続で、physical external-consumer acceptanceではない。
- [Khronos external-family正本](https://docs.vulkan.org/refpages/latest/refpages/source/VK_QUEUE_FAMILY_EXTERNAL.html)でsame device/driverのexternal endpointと標準値~1Uを確認。shared `include/libc/vulkan/vulkan_core.h`はKHR aliasがcore tokenへ展開するが、そのcore tokenが欠けるため両名ともcompileで未定義になった。共有header担当には手を入れず、private `BCM2711_VULKAN_EXTERNAL_FAMILY`を標準値0xfffffffeで追加（actual WSIと一致）。初回のcore名、KHR名によるcompile失敗をこのprivate定義で修正。共通生成headerの補完はQ1へ任意の投影事項、runtimeの必須依存にはしない。
- `sh plan/ws141/tests/vulkan-device-host-test.sh build/ws141-i14-external-barrier-host > build/ws141-i14-external-barrier-host.log 2>&1` exit0、19範囲PASS。actual typed export declaration50とprivate declaration52を持つ数値image-description fixtureをnative validatorへ渡し、external release/acquire双方のshare qualification・private allocation refusal・unrelated/mixed family refusal/no allocation residueを確認。descriptionだけはsyntheticで、real external client/provider/GPU execution証拠ではない。existing actual public barrier/runtime/whole-job testsも同host gateでPASS。
- named y `make -j16 ZEDBSD_CONFIG=config/ci/config-rpi4.mk BUILD=build/ws141-rpi4-y CONFIG_DRIVER_BCM2711_GPU=y vmunix > build/ws141-i14-external-barrier-y.log 2>&1` exit0、warning/error0、arm64 checks3 PASS。SHA256 `f8a0acb6cf38c36d7793bb3556e9bc773961573c86604deb3491e4d59b8c2fac`（private dispatch未公開/GC、実行proofではない）。formatter19/definition tabs/scoped style total0/diff0、manual exact declaration/typed ownership/finite family subsetを確認。HAL/UAPI/shared source変更無し、n再build無し。
- 次: GPU copy/clear/blitのmeta native lowering、primary QueueSubmitとpublic runtime/common binding、final stack/p007。i13/i14/p005/p006 in-progress、WS incomplete。Keiland/実機/console RAM寿命未達。Master/共有投影/GitHub公開はQ1、push無し。

### External-family admissionのmain統合

source `ea6db5eb9`とpreceding integration/resume記録`7c9931597`をQ1 main `a763d9d3f`へ独立integrationで結合、`c0c12c481a4eba8db65b3de1043a058529d6a244`をmain/ownへff。source checkpointとintegrationの`src/include/platform/config/toolchain/plan/ws141/tests`は全て同一、actual host19/named y warning/error0/style0 evidenceを保持し同一sourceのoptional再試験無し。Q1のpasskey/sessiondと各WSの変更を保持、Master担当変更無し、push無し。外部宣言のadmissionだけを成立させた段階で、native transfer/public queue/fence/Keiland/physical acceptanceをclearedとしない。次はGPU meta copy/clear/blit。
