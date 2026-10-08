<!-- awesome-plan project=zedbsd record=ws173 -->
# WS173: AAT（Agent Acceptance Test）— エージェントが素の実機を SSH で操作して受け入れを確かめる枠組み

Status: incomplete（2026-10-08 Q1 の判定（sweep-beta2-rc2 §2）: p001・p002・p003・p006 を cleared。残りは p005（素の 5330 の AAT）。2026-10-08 q910 P2 の照合: p004 cleared（表を直した）、p001・p002（T1-200 PASS）と p003・p006（test-done）は Q1 の判定、p005（素の 5330 の AAT）が残り）（2026-10-05 追加、最優先。p001〜p003 は QEMU で PASS、p004 のシナリオ 75 本・runner・補助と p006 の選択は merge 済みで T1-202 待ち。5330 の AAT はその後）
Master: [master](../master.md)
Primary Milestone: MG006

## 由来（ユーザー、2026-10-05 夕）

Q1 の問い「実機をベアメタル起動したときに、SSH越しにマウスイベントを投げて、スクリーンショットを撮り、それを取得するようなコマンドを作れますか？」への Q1 の案（/dev/input-inject にマウスとキーボードを足す、試験の image だけの画面の撮影、host の道具）に、ユーザー「では、それを実装して、AAT (Agent Acceptance Test)としてください。UATの必須確認項目は今挙げてくれたようなデバイス系と、全体の使用感に絞ります。AATの枠組みが完成した時点で実装されている機能でイメージを作成して、AATを実施、フィードバックを記録してください。UATはそのあとに遅らせます。UATの時刻は追ってお知らせします。」

## 決まったこと

- 実装の承認: `/dev/input-inject` にマウス（相対の移動・button・wheel）とキーボードの種類を足す（UAPI `include/uapi/input-inject.h` の追加、承認済み）。画面の撮影の口は試験の image だけ（製品には入れない）。host の道具。
- UAT の必須の確認は、デバイス系（電源 button・蓋・USB の抜き差し・touchpad の指・YubiKey と NFC の実物・音・Wi-Fi の実機の電波・S0 idle）と全体の使用感に絞る。それ以外は AAT。
- AAT の枠組みができた時点で実装済みの機能で image を作り、AAT を行い、結果（feedback）を記録する。UAT はその後（時刻はユーザーが後で知らせる）。

## Phase

| Phase | 内容 | 担当 | Status | 依存 |
| --- | --- | --- | --- | --- |
| p001 | kernel: `/dev/input-inject` にマウスとキーボード（UAPI の追加、試験の kernel の設定だけ） | P1 | cleared（2026-10-08 Q1 の判定（sweep-beta2-rc2 §2）: T1-200 で aat-p002 PASS。旧: Q1 の判定待ち（2026-10-05 Q1 の注記: T1-200 で aat-p002 PASS（2026-10-08 q910 P2 …） | — |
| p002 | compositor: 試験の image だけの画面の撮影の口と `keiland-shot`（i915 の実機で合成した画面を読み戻して PNG、QEMU でも同じ口） | P1 | cleared（2026-10-08 Q1 の判定（sweep-beta2-rc2 §2）: T1-200 で aat-p002 PASS。旧: Q1 の判定待ち（2026-10-05 Q1 の注記: T1-200 で aat-p002 PASS。phase.md は in-progr…） | — |
| [p003](phase003/phase.md) | host の道具 `plan/tools/aat/`（SSH で click・drag・wheel・key・type・shot の取得・log の行の待ち）と AAT の image の config | P2 | cleared（2026-10-08 Q1 の判定（sweep-beta2-rc2 §2）: T1-200c PASS、その後の AAT の実行で使われている。旧: test-done（2026-10-07: T1-200c 以降の AAT の実行で target で確認、判定は Q1）） | p001・p002 |
| p004 | AAT の項目の一覧（今の実装済みの機能、UAT の項目から機器と使用感を除いた物）と判定の基準 | Q1 | cleared（2026-10-06 Q1 判定: T1-202c の smoke 8 本 pass、full 79 本の判定の一覧（2026-10-08 q910 P2 の照合で phase.md に合わせた。旧: planning） | — |
| p005 | 素の 5330 で AAT を行い、結果を記録 | T1 か専任 | planning | p001〜p004、素の起動の方法（ユーザーの判断） |

- 2026-10-05 Q1: p001・p002（P1）は T1-200 で aat-p002 PASS、p003（P2 の host の道具）は T1-200c で PASS（QEMU、注入・撮影・転送・log の待ち）。AAT の土台は QEMU で動く。残り: p004 の項目と scenarios、素の 5330 での最初の実行（ユーザーが USB で起動）。

## シナリオの設計（2026-10-05 ユーザー）

ユーザー「AATはテストシナリオのドキュメントを元にエージェントが操作できるように設計してください。特殊な設計というわけではないですが。シナリオテスティングのシナリオは、ソースコードと同じように、我々の大きな財産です。tests/以下に整理して管理しましょう。AATにおいて、どのシナリオを実行するかは、考えてやっていきたいですね。OSの機能性ごととか、アプリごとの回帰試験のシナリオとか。新規実装した場合は、シナリオテストの合格を目指す、テスト駆動を取り入れるとか。リグレッションの範囲を考えてシナリオを選んだり、あるシナリオ集合に名前をつけてたまにフル回帰テストをしたり。」

→ まとめは [plan/tests.md](../tests.md)（2026-10-05 ユーザー「plan/tests.md にまとめるのがいいですね。」で docs から移した）、[tests/README.md](../../tests/README.md)。シナリオの項目は 目的・操作・確認事項・正解・確認方法（Markdown か JSON）。シナリオは top の `tests/scenarios/{os,desktop,apps}/` の Markdown（header に id・status（draft・active・retired）・areas・paths・machine・human、本文は Setup・Steps（Expect つき、pixel の座標を書かない）・Pass・Notes）、suite は `tests/suites/<name>.suite`（smoke・full・area・app・hardware）。新しい機能は draft のシナリオを先に書いて合格で active（テスト駆動）、変更は paths で選ぶ、bug は再現のシナリオを足す、定期とリリースの前に full。p004 は plan/tools/aat/scenarios/ でなくこの形で tests/ に作る。p006 として「シナリオの選択の helper（git の範囲 → paths）と suite の実行の runner」。
