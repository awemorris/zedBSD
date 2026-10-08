<!-- awesome-plan project=zedbsd record=ws113-p001 -->

# ws113-p001: 契約・能力と実機fixture

Parent: [WS113](../ws.md)
Status: in-progress → Q1 の判定待ち（2026-10-08 q902 P1 の照合: C1〜C4 は 2026-10-05 に決定済み（contracts-beta2.md の「ユーザーの決定」）、後続の p002〜p015 がこの契約で実装済み）（旧: in-progress（q702-i01、P2、2026-10-05。残りの契約を [contracts-beta2.md](contracts-beta2.md) に確定。確認の 4 点 C1〜C4 は Q1・ユーザー、判定は Q1））
Disposition: normal
Primary Milestone: MG006（WSから継承）
Queue / attempts: q586 / q586-i01 / A3（契約調査のみ、uncleared）、q702 / q702-i01 / P2（残りの契約の確定）
Purpose / goal: hotplug/複数出力/拡張とmirror/Settings/窓所属の仕様を確定
Prerequisites: 既存WS075/WS089/WS103の実出力を確認（context）
Investigation bound: q586-i01で07:12–08:42 UTC（2026-10-02）、90分の有限1 Phase。scopeは読取契約設計とWS113文書/証拠だけ。

## Procedure / affected components

driver UAPI、Vulkan Display実装、Settings/Waylandの所有を調査。安定output ID、0/1/2台、mode/座標/保存、異解像度mirror、pointer移動閾値、通知順と失敗を設計。
[設計](../design.md)の対応段と[全文方針](../../standards/ws113-display.md)を契約とする。材料が変わればWSと影響する他Phaseへ同時反映し、既存Queueの実装scopeを拡張しない。

## Clearance / verification

標準Vulkan拡張の結線と実機試験fixture、依存・制約・後続のAPI契約が定まり、ユーザーの二択/窓単一所属に矛盾しない。
結果は対象環境・source revision・commands・artifactsと結びつける。既存の実機/QEMUを混同しない。未解決の前提で調査上限に達したらattemptをunclearedとし、証拠と再開条件を記録する。

## Standards / limits / evidence

[Guardrail](../../guardrail.md)、[C全文](../../coding-style.md)、[scoped full rule](../../standards/ws113-display.md)、[automation](../../standards/automation.md#ws113-multi-display-coverage-2026-10-02)を実装前に読む。formatter/style-checkは補助、意味/所有/イベント順はfull/manual review。HAL API変更は差分ごとの事前承認。compositorのGPU UAPI直接ioctl禁止。無関係なtoolchain変更、aggregate make check、既存WS089/WS099のPhase改変は含めない。

Commands/results/commit/environment/artifacts/skipped checks: 2026-10-02 q586-i01でsource/plan全文・関係functionをrg/sed/catで読取、Khronos一次ページopen、docsのgit diff --checkとlocal link存在照合PASS。baseline0e68854ac、A3-001 commit6e34d1bb9 / main ACK8021bc210、A3-002 commit6a0a532b2、A3-003 addd258356、A3-004 df2f36ff3 / main ACKec870f856。全checkpoint/commandは[evidence ledger](evidence.md)。build/製品実装/hardware/SSH/host操作は未実施。
Findings/artifacts: [source能力20行](source-audit.md)、[契約](contracts.md)、[ID/完了保証](identity-completion.md)、[fixture](fixtures.md)、[native capability結線](native-contract.md)、[現状/依存](../design.md)。D-ID A2/旧boot anchor等はmain技術採択、D-ATOMICは未採択材料。標準仕様から実能力を推定しない。Resume: D-ATOMICの要求解釈/physical gateをuserまたは既存authorityが採択し、main reviewと必要Phase/WS投影を保存後、同p001のexact残scopeを新attemptへ選定・承認する。後続p002は未承認。[q592候補/criteria](next-selection.md)は準備だけ。

## 採択済local port ID / native capability入力

[main採択A2とsource](../phase001/identity-completion.md)、[native capability結線](../phase001/native-contract.md)を使う。自Phaseへの影響: D-ID A2/旧boot anchorは解決、D-ATOMICのみuser回答待ち。native capability semantic contractを設計入力へ追加。
後続の実装権限/依存は不変。actual API番号/layout/共有callback差分は選定前にowner/main review、HAL変更なら事前承認。

## q586-i01 outcome（2026-10-02 08:42 UTC）

Attempt: uncleared。Phase: uncleared / normal、WS: incomplete。
Reason: 90分上限に到達。D-ATOMIC（pointer境界のlogical owner同時更新とstrict physical消去/gapの受け入れ解釈）のuser回答が未解決で、全契約確定criteriaを満たしていない。read-only調査/設計は完了し、production実装や実機検証を行った結果ではない。

Evidence: 能力20行、contracts、identity-completion、native-contract、fixtures、next-selection、evidence ledger。A3 commitsはledgerへ、最新調査checkpoint ed06a2226。diff-check/local-path links/samplekeylength PASS。D-ID A2/boot/layout/reconnect/auth/初回fixtureはmain技術採択。未知native FIRST_PIXEL_OUT/power/timing/counterと現physical fixtureは後続readiness/実装gateに保持。

Residual / resume: D-ATOMIC採択を要求と検証へ投影し、必要なsource completion契約を選定。mainのcanonical Queue/Agent lane/WS/Master/Past Log/remote event deliveryを照合し、新attemptのapproval後に残設計を継続する。q592/p002を自動開始しない。同一agent sessionでmainの次指示を待機する。

## Event

2026-10-02 / ws113-multidisplay-plan-20261002-ws113-p001-created: current userの5条件・3つの追加判断をこのPhaseへ投影。planned/Queue none。GitHub body/comment/Projectへの公開は保留。

2026-10-02 / q586-start: 最新userのAgent A N=3開始指示から、p001契約設計調査のみをA3へ最大90minで選定。Vulkan仕様/実source/fixtureの能力を照合し、driver/HAL API/製品source編集や実機占有は後続。scopeとclearanceはlane/snapshotへ固定。

2026-10-02 / q586-A3-checkpoint1: [source-audit](source-audit.md)18行をmainへ統合。A3-001 6e34d1bb9 → 8021bc210 ACK。i915 HPD固定sequence/単一出力、Display extension4commandsと依存、Vulkan handleの非永続性を実source/一次仕様に照合。whole criteria未達、API/状態/fixture調査継続。hardware/runtime未実施。

2026-10-02 / ws113-contract-design-20261002-a3-ws113-p001: source/一次仕様照合からnative固定sequence/単一output、EXT全entry依存、永続ID欠落、scanout移動保証差を記録しcontracts/identity-completion/fixturesを保存。p002–p009のprocedure/検証/resumeを詳細化し、それぞれへeventを残した。依存順/既往WS/製品sourceは不変。D-ID/D-ATOMIC等の材料をmainへ送付、通常提案の技術裁量を分離。in-progress/q586-i01を保持。GitHub origin/foreign Phase/WS deliveryと全体projectionはmain依頼pending。

2026-10-02 / ws113-technical-choice-20261002-a3-ws113-p001: mainのdelegated technical decision messageからD-BOOT/LAYOUT/REC/AUTH/PORT通常案を採択記録。自Phase影響: D-IDのstandard短port key/UUID別gateを詳細化し、D-ATOMIC回答と旧override互換性を待つ。p001 in-progress。 [origin](../phase001/phase.md)/[詳細](../phase001/identity-completion.md)/[WS](../ws.md)。依存/Queue権限不変、main remote delivery pending。

2026-10-02 / ws113-local-port-id-20261002-a3-ws113-p001: mainのD-ID A2/旧bootpreferred技術採択messageを受領。D-ID A2/旧boot anchorは解決、D-ATOMICのみuser回答待ち。native capability semantic contractを設計入力へ追加。 [origin](../phase001/phase.md)/[sourceと範囲](../phase001/identity-completion.md)/[WS](../ws.md)。既往eventを保存し、該当current designを更新。p001 in-progress、他Phase planned/Queue none。main remote delivery pending。

2026-10-02 / ws113-next-selection-20261002-a3-ws113-p001: checkpoint evidence ledgerとq592候補のexact scope/criteria/readinessを保存。既存p002全scopeの選定材料で、implementation権限を新設しない。D-ATOMIC user回答待ち、first-pixel/native NEXT_REFRESHは未実証。strict F1/F2案を短い比較材料として追記したがmandatory処理/APIには採択しない。in-progress/q586期限を保持しmain reviewへ。

2026-10-02 08:42 UTC / ws113-q586-result-20261002-a3-ws113-p001: q586-i01をunclearedで終了、Phase uncleared/normal。90分内にsource/一次規約と全契約案/影響Phase/有限fixture/次候補を保存したがD-ATOMIC未決でclearance不可。source/build/hardware未実施、他条件の実装成功は主張しない。上記artifact/checkpointとresume conditionを保持。[WS](../ws.md)/[evidence](evidence.md)。mainのoutcome projection/remote delivery pending。同session待機。

2026-10-02 / ws113-beta1-plan-p001: fg019 の計画で、次の attempt を 0.75h（D-ATOMIC の採択の反映と whole-Phase の契約の確定だけ）と見積もった。D-ATOMIC のユーザーの回答までは開始しない。Status は uncleared のまま。

2026-10-02 user（D-ATOMIC）:「WS113は推奨でよいです。」→ **(a) logical owner の同時更新**を受け入れの解釈として採択。pointer の境界で窓の所属を compositor の中で一度に切り替え、最大 1 frame 程度の両画面での見え・不表示は許容。present_wait/present_id の追加と 2 head の同時 latch は要求しない。p001 の残り（0.75h、採択の反映と whole-Phase の契約の確定）を次の attempt で行う。

## q702-i01（2026-10-05、P2）: 残りの契約の確定

- [contracts-beta2.md](contracts-beta2.md) を作った（contracts.md の上に重ね、食い違いはこちらが優先）。確定: D-ATOMIC (a)、D-GOP（Guardrail の scanout の規則）、D-GOP-INV、D-RELEASE、D-BOOT2、D-HOTPLUG、D-MODES、D-EXT（4 entry 全部、counter は 0）、D-UAPI、D-PROTO（`kl_system_manager_v1` version 4 の `kl_system_displays_v1`）、D-AUTH2、D-STORE、D-BRIGHT（FreeBSD の backlight(9) と同じ形の kernel の device、i915 が provider、backend 経由）、D-BRIGHT-KEY、D-BRIGHT-BOOT、D-QEMU（Venus の `max_outputs=2`）。
- 実出力の照合: main `41633f4` の source を読んだ（native の UAPI、i915 の `output.c`・`panel-backlight.c`、Venus の scanout と topology の sequence、libvulkan の KHR_display、compositor の `compose_display()`、backend の display、`kl_system_manager_v1` version 3、Settings の Display の頁）。
- 確認が要る点: C1（最初の session で GOP の出力先以外も拡張で点けてよいか、推奨は全て拡張）、C2（共有の UAPI の追加 2 件、推奨は許可）、C3（Fn の key の kernel の部分を WS049 へ）、C4（WS051 p002 との分担、推奨は WS113 p002 で行う）。
- 後続の構成の変更: p002 を絞り（規則・inventory・HPD）、p011（2 つ目の出力）・p012（native の power・refresh）・p013（明るさの下層）を足した。依存は ws.md の図。
- 未実施: build・実機（文書だけ）。この attempt の範囲は計画で、製品の source は変えていない。
