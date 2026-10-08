<!-- awesome-plan project=zedbsd record=ws113-p005 -->

# ws113-p005: compositor拡張とlibkeiland

Parent: [WS113](../ws.md)
Status: in-progress → Q1 の判定待ち（2026-10-08 q902 P1 の照合: q855 P1 実装、T1-366 QEMU で displays-p005.sh PASS（T1 の台帳）。5330 の明るさの slider と Fn の key は未、p008）（旧: planned）
Disposition: normal
Primary Milestone: MG006（WSから継承）
Queue / attempts: none / 実装未承認
Purpose / goal: Settings用の照会/変更/通知APIを公開
Prerequisites: p004 cleared/出力状態と適用API
Investigation bound: 90分の有限1 Phase Queue案。選定時にscope/時間を再照合する。

## Procedure / affected components

専用Wayland protocolを設計/実装し、libkeiland公開APIがoutput一覧・mode・座標・適用結果・hotplugを包む。認可/入力検査/失敗を明示。
[設計](../design.md)の対応段と[全文方針](../../standards/ws113-display.md)を契約とする。材料が変わればWSと影響する他Phaseへ同時反映し、既存Queueの実装scopeを拡張しない。

## Clearance / verification

独立clientがlibkeiland経由で二択と配置を照会/適用/購読でき、無効配置や権限不足は状態を壊さず拒否。Settingsにdriver直呼出しなし。
結果は対象環境・source revision・commands・artifactsと結びつける。既存の実機/QEMUを混同しない。未解決の前提で調査上限に達したらattemptをunclearedとし、証拠と再開条件を記録する。

## Standards / limits / evidence

[Guardrail](../../guardrail.md)、[C全文](../../coding-style.md)、[scoped full rule](../../standards/ws113-display.md)、[automation](../../standards/automation.md#ws113-multi-display-coverage-2026-10-02)を実装前に読む。formatter/style-checkは補助、意味/所有/イベント順はfull/manual review。HAL API変更は差分ごとの事前承認。compositorのGPU UAPI直接ioctl禁止。無関係なtoolchain変更、aggregate make check、既存WS089/WS099のPhase改変は含めない。

Commands/results/commit/environment/artifacts/skipped checks: 未実施（計画のみ）。Findings: [現状調査](../design.md)。Resume: prerequisiteの実出力を確認し、このPhaseだけを新Queueへ選定・承認後に開始。

## p001契約調査による詳細化（2026-10-02）

[origin p001](../phase001/phase.md)、[契約](../phase001/contracts.md)、[ID/完了比較](../phase001/identity-completion.md)、[fixture](../phase001/fixtures.md)、[WS summary](../ws.md)を入力とする。

Procedure: 専用managerのbegin/output/done snapshot、changed、expected topology/config serial+token/genのconfiguration/apply/result、request_idを確定。public libkeilandがregistry/version/object/snapshot/callback寿命とENOTSUPを包む。active session peer credentialはOS moduleで確認する通常案。compositor-owned displays.confのversion/persistent keyとtemp/flush/rename、appliedとsavedを別結果にする。

Verification / resume: D07–D09を適用。partial snapshotをUIへ公開しない、stale/duplicate/missing/overflow/unsupported/unauthorized/busy/backend/rollback/persistence結果を分ける。私有Vulkan identity APIはmain不採用、A2 local keyを採用。snapshot clone/draftとclient切断退役、独立clientの変更通知を確認。

Status/dependenciesは上記のまま。未採択architecture/製品判断とactual prerequisiteを確認し、新QueueでこのPhaseだけを有限選定・承認後に実装する。q586はp001文書のみで後続sourceを許可しない。

## 採択済local port ID / native capability入力

[main採択A2とsource](../phase001/identity-completion.md)、[native capability結線](../phase001/native-contract.md)を使う。自Phaseへの影響: snapshot key/label/persistableはA2 schema/一意性を検査。保存を同machine/PCI port scopeに限定、kind/portの人向けlabelを公開、UUID問い合わせを必須にしない。
後続の実装権限/依存は不変。actual API番号/layout/共有callback差分は選定前にowner/main review、HAL変更なら事前承認。

## Event

2026-10-02 / ws113-multidisplay-plan-20261002-ws113-p005-created: current userの5条件・3つの追加判断をこのPhaseへ投影。planned/Queue none。GitHub body/comment/Projectへの公開は保留。

2026-10-02 / ws113-contract-design-20261002-a3-ws113-p005: p001のsource/一次仕様で明らかになった不足に合わせ、上記の自Phase procedureと検証/resumeを詳細化。D07–D09を適用。partial snapshotをUIへ公開しない、stale/duplicate/missing/overflow/unsupported/unauthorized/busy/backend/rollback/persistence結果を分ける。私有Vulkan identity APIは未採択。snapshot clone/draftとclient切断退役、独立clientの変更通知を確認。 origin/WSリンクは上記。planned/Queue noneを保持。GitHub body/comment/Projectはmainへdelivery依頼pending。

2026-10-02 / ws113-technical-choice-20261002-a3-ws113-p005: mainのdelegated technical decision messageからD-BOOT/LAYOUT/REC/AUTH/PORT通常案を採択記録。自Phase影響: active session同UID peer検査、Settings限定secret無し、保存keyはstandard policy詳細を待つ。 [origin](../phase001/phase.md)/[詳細](../phase001/identity-completion.md)/[WS](../ws.md)。依存/Queue権限不変、main remote delivery pending。

2026-10-02 / ws113-local-port-id-20261002-a3-ws113-p005: mainのD-ID A2/旧bootpreferred技術採択messageを受領。snapshot key/label/persistableはA2 schema/一意性を検査。保存を同machine/PCI port scopeに限定、kind/portの人向けlabelを公開、UUID問い合わせを必須にしない。 [origin](../phase001/phase.md)/[sourceと範囲](../phase001/identity-completion.md)/[WS](../ws.md)。既往eventを保存し、該当current designを更新。p001 in-progress、他Phase planned/Queue none。main remote delivery pending。

## 2026-10-05 計画（q702、ベータ2）

入力: [契約の確定](../phase001/contracts-beta2.md) D-PROTO・D-AUTH2・D-STORE・D-BRIGHT・D-BRIGHT-KEY・D-BRIGHT-BOOT、contracts.md §8。

範囲:
- protocol（`userland/desktop/libkeiland/system/kl-system-protocol.h`、compositor `wayland/system.c`、libkeiland `system/system-protocol.c`）: `kl_system_manager_v1` version 4 の request 7 `get_displays(new_id kl_system_displays_v1)`。`kl_system_displays_v1`: event `begin(topology_serial hi/lo, config_serial hi/lo, mode, health)`・`output(token hi/lo, generation hi/lo, key, label, x, y, width, height, refresh_mhz, flags（internal・active・anchor・has_backlight）, brightness)`・`done`・`result(request, status, applied, saved)`。request `configure(request, expected serials, mode)` + `place(request, token, x, y)` + `apply(request)`、`set_brightness(request, token, level)`、`destroy`。status は stale・invalid・unsupported・unauthorized・busy・backend_failed・rollback_failed を分ける。
- libkeiland（`system/`、`keiland.h`、`exports.map`）: `kl_system_displays_open/close/snapshot/configure/apply/set_brightness`、snapshot の寿命、callback（既存の `kl_system` と同じ dispatch）。`KL_VERSION` の次。
- compositor: 設定の適用は p004 の transaction、明るさは backend の `kl_backend_backlight_*`（p013）。Fn の key（`KEY_BRIGHTNESSUP/DOWN`）で 5 % ずつ（D-BRIGHT-KEY）、session の始めに displays.conf の明るさを適用（D-BRIGHT-BOOT）。active な session でない時は変更を拒む（D-AUTH2）。
- 試験の client: `userland/tests/display-probe`（snapshot を log に出す、configure・apply・set_brightness を引数で）。

試験: host（libkeiland の protocol の encode・decode、`plan/ws131/tests/host-system.c` に displays の stub と case を足す）。QEMU（T1、Venus の 2 出力）: display-probe で snapshot に 2 出力、拡張 ⇔ mirror の apply が applied=1 saved=1、stale な serial が `stale`、明るさは has_backlight=0 で `unsupported`。実機（p008）: 明るさの set と Fn の key。
受け入れ: 上の QEMU と host の PASS、warning 0（zedBSD・Linux の build。Linux・FreeBSD の backend は ENOTSUP の stub）、規約。目安 3〜4h。依存: p004、p013（明るさの backend）。衝突: WS089・WS131 の `kl_system` を変える Phase と直列。

### D-LIMIT の反映（2026-10-05）

`output` の event の flags に `limited`（同時に表示できる数の制限で今は使えない）を足す。configure・apply で limited の出力を含めても失敗にせず、applied の snapshot で limited のまま返す。

## 実装（2026-10-07 q855、P1）

範囲（Q1 への送付のとおり、2026-10-07 Q1 の ACK: version 18・KL_VERSION 58）。

| 部分 | file |
| --- | --- |
| protocol | `userland/desktop/libkeiland/system/kl-system-protocol.h`: `kl_system_manager_v1` version 18、request 13 `get_displays`、`KL_SYSTEM_CAPABILITY_DISPLAYS`、`kl_system_displays_v1`（request apply(request, serial, mode, places)・set_brightness(request, key, percent)、event output(key, label, x, y, width, height, refresh_mhz, flags, brightness)・done(serial, mode)・result）、flags（INTERNAL・ANCHOR・SHOWN・BACKLIGHT・LIMITED）、`KL_SYSTEM_RESULT_STALE`。places は "KEY X Y" の行（key は空白を含めてよい） |
| compositor | 新しい `wayland/displays-shell.c`: object の作成と snapshot（列挙の全 display: anchor・head・limited・kept_off、label は A2 の kind と port、明るさは内蔵だけ）、apply（active な session でなければ DENIED、serial が違えば STALE、mode と places の検査は heads.c の `kwl_displays_apply`、拒否は INVALID、applied と saved を別に）、set_brightness（内蔵でなければ・light が無ければ UNSUPPORTED、0〜100、蓋で消している間は戻す値を更新、displays.conf の `brightness=`）、変化ごとに全 object へ snapshot（`kwl_displays_tell`、heads.c の変化から）、Fn の KEY_BRIGHTNESSDOWN/UP で 5 % ずつ（`kwl_displays_key`、seat.c の key の道で lock・greeter の後）、session の最初の frame の後に displays.conf の明るさを適用（`kwl_displays_tick`、display.c）。`system.c` の get_displays と capability、`protocol.c`・`kwl.h` の kind、`output-switch.c` の `kwl_output_display_internal`、`displays.c` の `brightness=`、Makefile 3 つ |
| libkeiland | `system/system-protocol.c`・`.h`（interface、manager の 14 request）、`system/system.c`（listener、object の作成（version 18）、`kl_system_displays_get`・`_mode`・`_apply`・`_set_brightness`、capability）、`system/system-view.c`・`system-private.h`（snapshot の pending と done、STALE → ESTALE）、`include/keiland/keiland.h`（KL_VERSION 58、`struct kl_display`・`kl_display_place`、`KL_SYSTEM_HAS_DISPLAYS`・`KL_SYSTEM_CHANGED_DISPLAYS`、`KL_DISPLAY_*`）、`exports.map`（exports.py で生成） |
| 試験 | `userland/tests/keiland-system` に displays・display-mode・display-place・brightness、`plan/ws131/tests/host-system.c` に displays の偽物（2 display、serial 7、stale、明るさ）と case、ついでに既に link で落ちていた `kwl_sleep_answers`・`kwl_sleep_request` の偽物（ws052 の sleep の後、host に sessiond は無い）。`plan/ws113/tests/host-displays.c` に brightness の行。T1 用の `displays-p005.sh`・`config-amd64-p005.mk` |

確認（2026-10-07）:
- `sh plan/ws131/tests/host-system.sh build/p1-host-system/host-system` PASS（ASan・UBSan: displays の capability、snapshot 2 行の key・label・位置・flags・明るさ、mirror の apply と places の文字列 "…hdmi:B 1920 0\n"、snapshot の mode、古い serial で ESTALE、明るさの set と snapshot、mode・key・範囲の EINVAL）、`sh plan/ws113/tests/host-displays.sh` PASS、`host-output-switch.sh` PASS。
- build（warning 0）: zedBSD の wayland（shot.c）・libkeiland.so・settings・keiland-system、keiland-linux。`exports.py --check` OK。style-check: 新しい file 0、変えた file は増えない。`keiland-os-boundary/check.sh` は B3 の 2 件（sessiond と printd の Makefile、この変更の前から）以外 PASS。
- 未実施: QEMU（T1 `displays-p005.sh`）、実機の明るさと Fn の key（p008、5330）、compositor 側の displays-shell.c の host 試験（compose と Vulkan に依るので QEMU で見る）。
- 制限: result の saved は libkeiland の `kl_system_take_result` には届かない（既存の形、applied だけが errno になる）。Linux・FreeBSD の backlight は backend が ENOTSUP。
