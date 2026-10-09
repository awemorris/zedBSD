<!-- awesome-plan project=zedbsd record=ws106 -->

# WS106: テスト用アプリを userland/tests/ に集約

<!-- awesome-plan-current:start -->
Status: incomplete
Primary Milestone: MG001
Related Milestones: MG006
Parent: [Master](../master.md)
Queue: なし（q540 finished、partial scope cleared）
Resume point: 2026-10-09 P1: p002 は cleared 候補（ime-probe は 2026-10-07 に別の作業で userland/tests へ移っていた、照合と build を記録）。次は p003（全文規約・build/install・最終 boot）。旧: ime-probe の非競合回答後、p002残り2filesを選定。p003は全移動後。
2026-10-02 user:「IME の人間の作業が終わったので、ime-probe を試験の場所へ移す作業を再開してよいです。」→ p002 の ime-probe の移動の判断待ちは解消。p002 を再開候補にする。
Target: **ベータ3**（2026-10-08 ユーザー「ベータ3にします：WS009・026・106 文書・試験の整理・試験アプリの集約、WS139 デスクトップの速さ」）
<!-- awesome-plan-current:end -->

## 目標・決定の出典

base・desktop のテスト用アプリと指定された見本を userland/tests/ に移し、既存の選択・実行・デモの振る舞いを保つ。

2026-10-01 ユーザーの [レビューコメント](../reviews/2026-10-01-review.md)を根拠に計画。
Primary MG001 にこの目標の成果を提供。Related MG006 は技術/配布/検証の supporting 出力。
WS completion と milestone 全体の acceptance は別に確認する。

## 範囲

[対象表](inventory.md)の 30 件（base 21、desktop 9）を対象にする。gpudemo は実行 binary ではなくデモデータ package。
移動先は `userland/tests/<name>/`。`userland/base/test/` は POSIX の test utility なので残す。
host の検証 fixture / plan の履歴 / production app / library はこの移動に含めない。
source、header、Makefile/Makefile.linux、shader、モデル等の付属データを package ごとに移す。
共通 package.mk と grouping、package registry/dependency の経路、config・test runner・Linux include の参照を整える。
元の package 名、config の選択名、install 先、既定の選択状態、デモ S13 を保つ。category の変更は p001 で定義する。
`ime-probe` は WS095 の人間作業と重なるので、実際の移動前に所有・作業停止を確認する。

## WS 自身の完了条件

- T1: 30 件の全ファイルと移動元→先・参照変更を台帳化し、対象外を明記。
- T2: 二重登録や古い source 参照が無く、既存 package/config/install の契約を保つ。
- T3: amd64 zedBSD と既存 Linux 対応 5 app package の clean build/install が warning 0。gpudemo/model/shader のデータも一致。
- T4: 既存の移動に関係する runner が新経路を利用でき、最後の zedBSD boot PNG と全文規約レビューを記録。

## 依存・所有

WS105 の既存 Linux 出力を利用。WS095 の ime-probe は人間の作業と調整。WS107 の browser-probe は public API を使うためエンジン移動と実装依存は無いが、共通 runner 編集は直列。WS108 は最終のテスト配置を取り込む。

## Phase 表（後続は設計案）

| ID / Phase | 目的 | Goal | Status | 依存 |
| --- | --- | --- | --- | --- |
| [ws106p001](phase001/phase.md) | 対象・参照・build 契約を確定 | 台帳と移動手順、実際の build/config/check command を固定。所有が未調整なら ime-probe の実行を選定しない。 | cleared | なし |
| [ws106p002](phase002/phase.md) | source・データと参照を移動 | T1/T2 を満たす。source とデータの move 前後の hash と差分を記録する。 | cleared 候補（2026-10-09 P1） | p001 |
| [ws106p003](phase003/phase.md) | 全文規約・build/install・最終 boot | T1〜T4 と全 Phase の成果を照合。未実施の機種・アプリ実行は区別して記録する。 | planning | p002 |


依存は表の prerequisite → dependent。context は選定された作業ではない。
後続 Phase の detail は p001 の確定設計から作る。表/実 record/Queue を一緒に整える。

## 制約・標準・検証の扱い

[Guardrail](../guardrail.md)、[AGENTS.md](../../AGENTS.md)、[C 規約全文](../coding-style.md)、
[自動化の対応](../standards/automation.md)を適用する。簡約版は無い。コード生成前に全文の該当節を読み、
最後の conformance Phase では本 WS の全 source 変更を全文でレビューする。
clang-format 19 / style-check は補助。無関係な一括整形、`make check`、`.internal/` の参照は行わない。
意味を変えない移動は warning 0 の build と最後の boot を関門にする。API・platform・packaging の変更には意味のある契約検証を追加する。
image build は直列。zedBSD の起動は boot-test.sh と PNG、console/serial log を起動の証拠にしない。
commit は `WIP`、push なし。GitHub 公開・Issue/Project 更新は現在 deferred、local cache/outbox に記録する。

## 現状・再開

29 package＋追加13filesの承認partial scopeはcleared。164filesのhash/mode/参照・registry保存、Linux GCC/Clang clean build/install warning0（ELF24/source331各）、zedBSD28app/POSIX/loader/image warning0、boot-test.sh login PNG確認。ime-probe2filesは非競合回答待ちで未変更。whole p002はuncleared、p003は未実行、WS106はincomplete。
[検証checkpoint](verification-checkpoint.md)。残りime-probeと最終conformance/bootを再開する。WS105の完了範囲は変わらない。

## イベント

2026-10-01 / review-20261001-planning: ユーザーのレビューコメントから WS を新設。
範囲・受け入れ・Phase 案を保存、Master / Outlook と照合した。新規実装の Queue 承認は未取得。
[決定の出典と関連 WS](../reviews/2026-10-01-review.md)。公開時にはこのイベントを WS に届ける（現在 outbox 保留）。


## 2026-10-01 調査による scope 補完（q539）

ユーザー回答「13 ファイルも追加して移動する」を採用。base/tests直下の syscall-smoke.c、posix-r2.c、posix-r2-remaining.c、
susv4-xsi.c、posix-phase5-helper.c、smp-resource-stress.c、dyntest.c、tlstest.c、rpathdep.c、rpathtest.c、versiontest.c、versionuse.c、versiontest.map を userland/tests/ 直下へ。
これに付く grouping Makefile と platform の source/object/map/runner参照も更新する。
対象30 package は保持。T1/T2 の移動/参照台帳は追加13filesを含む。T3/T4もこの追加範囲のbuild/hash/規約reviewへ適用。
[移動手順と検証](../ws106/design.md)、[台帳](../ws106/survey.json)。p001→p002→p003の依存は変更しない。
Event ws106-q539-scope-update: 調査で初期inventoryの漏れを発見し、ユーザーの具体的追加指示と全変更Phase/WSへの影響を記録。
GitHubの各Phase/WS event deliveryはoutbox pending。ime-probe所有確認と既存style扱いの回答を待つ。

2026-10-01 / ws106-q539-style-decision: ユーザー「WS106 は移動に限定し、既存スタイルの維持を認める」。[全文の限定例外](../standards/ws106-relocation.md)を適用。新実装なし、move/hashとpath-only diffを全文review。

2026-10-01 / ws106-q539-cleared: p001 cleared。30 package＋承認追加13files、全166 tracked fileのhash/mode/source→destinationと参照217fileを棚卸し。menu/package/config/install互換とbuild/boot手順をdesign.mdへ確定。既存styleはユーザーの限定例外を記録。ime-probeは人間作業との非競合回答まで選定から外す条件で、他29件と13filesは実行可能。証拠: plan/ws106/survey.json、programs-before.txt、style-before.txt、design.md。

2026-10-01 / ws106-fonts-tracking: 追加した既存menuconfig fixtureは移動前からのFonts分類欠落でFAIL。別条件の[BUG-129](../bugs/BUG-129.md)へ記録。移動固有のregistry比較PASS、全fixture PASSは未主張、修正はWS106の移動scope外。

2026-10-01 / ws106-q540-partial-cleared: 29 package＋追加13filesの承認partial scopeはcleared。164filesのhash/mode/参照・registry保存、Linux GCC/Clang clean build/install warning0（ELF24/source331各）、zedBSD28app/POSIX/loader/image warning0、boot-test.sh login PNG確認。ime-probe2filesは非競合回答待ちで未変更。whole p002はuncleared、p003は未実行、WS106はincomplete。 所有質問は未回答、経過時間を確認済みとは解釈しない。p003のwhole p002依存は保持する。
