<!-- awesome-plan project=zedbsd record=ws211 -->

# WS211: Pi 4 の compositor の描画の削減（App Home の遷移のクロスフェード、draw の束ね）

<!-- awesome-plan-current:start -->
Status: completed（2026-10-27 P3 の精査 q1608。2026-10-13 ユーザー「いや、これ以上詰めなくていいです。」で最適化を終えた）
Completed: 2026-10-27（q1608）
Primary Milestone: MG006
Related Milestones: MG008
Parent: [Master](../master.md)
Queue: なし
Resume point: なし（新しい要求は新しい WS として立てる。この WS は再開しない）
- 2026-11-15 Q1: ユーザーの判断で、残していた phase001・phase003・phase009 の directory を消した。source の comment の `plan/ws211/phaseNNN/phase.md` への参照（compose-bind.c・batch.c・layer-cache.c・damage.c）も外した。
<!-- awesome-plan-current:end -->

## 目標

Pi 4 で App Home の開閉のアニメが 60 fps に近く、App Home を開いたままの操作（hover・ページ）も軽い。i915・Venus の見た目と速さを損なわない。範囲は compositor（userland/desktop/wayland/）の中。

## ユーザーの決定

- 2026-10-12（由来）:「場合によっては、デスクトップ1枚とHome1枚でクロスフェードすることが必要かも。」「App Homeのアイコンは、遷移のスムーズさのために常に保持するしかないと思います。」→ Q1 の (1) クロスフェード (2) draw の束ねの WS の案に「承認します。」
- 2026-10-12（Pi の試し）「めちゃくちゃ速くなりました」「非常に快適です」。
- 2026-10-13「コンポジタ、アプリ、CSE/DCE、すべてが効いて、これまでに見たこともないようななめらかなデスクトップです。」
- 2026-10-13「いや、これ以上詰めなくていいです。」（描画の最適化の終了）

## 結果

- クロスフェード（p002）は main（4c3a56cf9、`glass_draw_crossfade`）。開閉のアニメのコマは絵を使い 1 コマ数十 draw。
- 時計の部分の更新（p005、fab6d4df1）、cursor の present の間隔（p006、1d9c9749e）、dock・undock の拡大縮小（p007、902a5feb2）、client の damage の所だけの描画（p008、db55254d4）、複数の damage の四角（p009、e6e7b1a9b・4ec9f16d1）、1 コマの CPU の仕事の削減（p010、f7873c64f・af2dd3887）、present の copy の command buffer の使い回し（p011、fa6102245）は main の履歴にある（2026-10-28 に `git merge-base --is-ancestor` で確かめた）。
- Pi の実機の確かめはユーザーの目の評価（上の原文）。Phase ごとの Pi の log の判定と T1（i915・Venus）の回帰は未実施のまま閉じた。

## 制限・移管

- 完了の条件（仮）の数値（1 コマの draw の数と時間の目標）は決めずに終えた（2026-10-13 ユーザーの終了）。
- p003 の段 2（床の試し）以降と、p001 の未決（Pi の変換の方式・舞台の 1 枚化・面積の閾値）は終了の時に未着手。
- p004 の規約の全文は WS の外の一括の見直しへ（2026-10-08 ユーザー「規約の整形（各 WS の全文規約の Phase）はベータ3」、2026-10-27 の精査で一括の見直しに読み替え）。

## Phase 一覧

| Phase | 目的 | 結果 |
| --- | --- | --- |
| ws211-p001 | 調査と設計（draw の数え、クロスフェードの絵の持ち方、束ね、Pi の見積もり、試験） | planning（第 4 版）のまま WS を閉じた。段 1・2 は計測用に main へ |
| ws211-p002 | クロスフェードの実装と Pi の確かめ | 実装は main（4c3a56cf9）。Phase の記録は無い |
| ws211-p003 | draw の束ね（文字・tile・影） | 段 1 は実装、段 2 は未着手で終了 |
| ws211-p005 | 時計の 1 分ごとの更新を時計の所だけに | in-progress のまま閉じた（実装は main、Pi の判定は未実施） |
| ws211-p006 | 窓の drag とマウス移動を 60 fps に近づける | in-progress のまま閉じた（cursor の present の間隔の直しは main） |
| ws211-p007 | dock・undock を窓の絵の拡大縮小に | in-progress のまま閉じた（実装は main、Pi の判定は未実施） |
| ws211-p008 | Calendar の hover などを client の damage の所だけ描く | in-progress のまま閉じた（実装は main、ユーザーの「非常に快適です」） |
| ws211-p009 | damage を複数の四角のまま描く | in-progress のまま閉じた（実装は main） |
| ws211-p010 | 1 コマの CPU の仕事（decode の object の探索を O(1) など） | in-progress のまま閉じた（実装は main、host 試験 PASS） |
| ws211-p011 | present の copy の command buffer の使い回し、窓の tile の GPU の費用 | in-progress のまま閉じた（1 は main、host 試験 PASS） |
| ws211-p004 | 規約の全文の見直し | WS の外の一括の見直しへ |

## 記録の所在

Phase の計画・結果・試験は削除する（上の retained の 3 つは Q1 の判断の後）。git の commit `fb1f6afee` の `plan/ws211/` にある。Queue の履歴は [plan/history](../history/index.md)。
