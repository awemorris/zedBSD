<!-- awesome-plan project=zedbsd record=ws106p002 -->

# ws106p002: source・データと参照を移動

Status: cleared（2026-10-09 Q1 判定: 30 件の照合と build）
Disposition: normal
Parent: [WS106](../ws.md)
Queue / Attempt: q540 / q540-i01

## 目的・範囲

p001 の確定表だけを移動し、package registry、config、Linux build、現役の試験の参照を更新する。履歴の旧経路は当時の証拠として保持する。

## 完了条件

T1/T2 を満たす。source とデータの move 前後の hash と差分を記録する。

## 前提・未決・実行手順

依存: p001。WS の scope と acceptance、設計の未決を確認する。
技術的な細部は委任範囲で決める。対象/受け入れ/外部契約を変える結果は実装前に計画と承認範囲へ反映する。
調査→変更表と手順の確定→有限 Queue の承認→実装→指定検証→結果・WS・Master・Queue の照合。
最初の p001 は調査の案（1 session / 最大60分、満たせない点と再開条件を残す）。後続の timebox/command は設計後に選定する。

## 適用規則・影響する部品・検証

[WS の制約/部品/受け入れ](../ws.md)、[Guardrail](../../guardrail.md)、
[C 規約全文](../../coding-style.md)、[自動化](../../standards/automation.md)を適用。
新規/変更 C は全文該当節を読み、clang-format-19 と style-check の限界を補う。
最終 conformance は全 WS の source を全文で review。build warning 0、必要な契約検証、diff-check を記録する。
具体的な build/config/tool version と script は p001 の結果で固定する。`make check` は禁止。
zedBSD の起動は boot-test.sh の PNG。Linux の既存検証は WS105 の手順と許可範囲、FreeBSD は検証環境の確定が必要。

## 証拠・結果・再開条件

q540 の partial 移動・検証を実行済み。下記checkpointと終了結果を参照。
再開: prerequisite の実 output と変更の所有、scope snapshot、Queue 承認を確認する。

## イベント

2026-10-01 / review-20261001-planning: 新設した Phase 案。親 WS の目標への寄与と依存を記録。実装の選定は未実施。
GitHub の Phase 作成/comment/Project の projection は公開保留、local outbox に保持する。


## 2026-10-01 調査による scope 補完（q539）

ユーザー回答「13 ファイルも追加して移動する」を採用。base/tests直下の syscall-smoke.c、posix-r2.c、posix-r2-remaining.c、
susv4-xsi.c、posix-phase5-helper.c、smp-resource-stress.c、dyntest.c、tlstest.c、rpathdep.c、rpathtest.c、versiontest.c、versionuse.c、versiontest.map を userland/tests/ 直下へ。
これに付く grouping Makefile と platform の source/object/map/runner参照も更新する。
対象30 package は保持。T1/T2 の移動/参照台帳は追加13filesを含む。T3/T4もこの追加範囲のbuild/hash/規約reviewへ適用。
[移動手順と検証](../design.md)、[台帳](../survey.json)。p001→p002→p003の依存は変更しない。
Event ws106-q539-scope-update: 調査で初期inventoryの漏れを発見し、ユーザーの具体的追加指示と全変更Phase/WSへの影響を記録。
GitHubの各Phase/WS event deliveryはoutbox pending。ime-probe所有確認と既存style扱いの回答を待つ。

2026-10-01 / ws106-q539-design-revision: root13fileとgrouping/menu/referenceを含む[移動手順](../design.md)へ詳細化。既存styleは[限定例外](../../standards/ws106-relocation.md)で保持。ime-probeの所有未確認なら、その1件を選定から外したpartial Queueで他29件＋13fileを先に移す。Phase全体のclearanceには残り1件の移動も要る。

## q540 checkpoint（partial scope、ime-probe回答待ち）

29 package＋直下13filesを移動。164 tracked files、124 byte-identical、40はMakefile/locator等の許容差分、58 Cはinclude1行以外同じ実装。全ID/default/platform/dependencyを保持、Testsへ29件。
Linux gcc/clang clean build/install exit0、ELF24各/header331各/source同期PASS、model/texture/contentと5appのinstall確認。zedBSDはconfigにtest IDをcommand lineで明示して28app＋static POSIX/SUS/SMP/helper/dynamic loader artifact build exit0、disk-image exit0、warning0。
最初のbin-target buildは未選択IDの規則が無いcommandでFAIL、既存の構成選択を明示して再実行PASS。誤ったsource問題とはしない。追加include確認はgpu-shareの既存private -Iを考慮してPASS。
既存style候補は1300−ime-probe28=1272でnormalize完全一致、新指摘なし。実機/GPUの機能回帰はpure moveの規則に従い未実施。
ime-probeは未変更で人間作業の非競合回答待ち、whole Phaseはまだclearedでない。scope補完/続行の条件を保持。

2026-10-01 / ws106-fonts-tracking: 追加した既存menuconfig fixtureは移動前からのFonts分類欠落でFAIL。別条件の[BUG-129](../../bugs/BUG-129.md)へ記録。移動固有のregistry比較PASS、全fixture PASSは未主張、修正はWS106の移動scope外。

## q540-i01 終了 / 2026-10-01T14:50:18.951054+00:00

29 package＋追加13filesの承認partial scopeはcleared。164filesのhash/mode/参照・registry保存、Linux GCC/Clang clean build/install warning0（ELF24/source331各）、zedBSD28app/POSIX/loader/image warning0、boot-test.sh login PNG確認。ime-probe2filesは非競合回答待ちで未変更。whole p002はuncleared、p003は未実行、WS106はincomplete。

限定されたQueue itemのclearanceとwhole Phaseの未達を分ける。T1/T2全30件は未達（ime-probe1件のみ）。所有確認後に同じPhaseの残り2filesを新attemptで選定し、全30件のregistry/style/hashを再照合する。
[検証checkpoint](../verification-checkpoint.md)、[BUG-129](../../bugs/BUG-129.md)、[boot PNG](../../history/ws106/q540/evidence/login.png)。
Phaseはremote closeしない。Event: ws106-q540-partial-cleared、Phase/WS/Queue/Board delivery outbox pending。

## 2026-10-09 P1: 残りの 1 件（ime-probe）の照合

- ime-probe は既に `userland/tests/ime-probe/`（Makefile と main.c）にある。移したのは 71c8a1487（2026-10-07、master.md の「ユーザー（make menuconfig）: desktop の下の試験の program は tree ごと tests へ（userland/desktop/ime-probe → userland/tests/ime-probe）」）。この Phase の新しい attempt で移す物は残っていない。
- 照合:
  - [inventory.md](../inventory.md) の移動先の `userland/tests/*` は全部ある。
  - 移動元で残るのは `userland/base/test`（POSIX の test、対象外）と `userland/desktop/keiland-linux.mk`（参照の file、移さない）だけ。
  - 古い path（`desktop/ime-probe`・`base/ime-probe`）を参照する make・shell・python・md は、master.md の履歴の 1 行のほかに無い（`grep -rn`、build/・plan/history・plan/ws106 を除く）。
- build: `make ZEDBSD_CONFIG=plan/ws035/tests/config-amd64-zdesktop.mk BUILD=build/p1-ws177 build/p1-ws177/bin/ime-probe` は rc 0、warning 0。ELF の検査（libwayland-client.so・libc.so）は PASS。
- 未実施: ime-probe の移動の前後の hash と mode の比較（移したのは別の作業で、前の版との比較の記録は無い）。p003 の全体の build・install・boot。
