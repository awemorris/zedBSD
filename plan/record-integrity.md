# 記録の整合を保つ手順

2026-10-15 ユーザー「記録の整合性を担保する方法をAGENTS.mdにまとめておいてください（plan/のどこかに詳細を書いて、AGENTS.mdにはエッセンスだけ、でもOKです）。」。AGENTS.md の「記録の整合」はこの文書の要点。

## 背景（2026-10-15 に見つかった事）

- Q1 が、失敗した merge（実在しない commit）を成功として記録・報告した。取り消しは 485ad5be6。
- 10-15 04:36 以降の記録が、1 日進んだ 2026-10-16 と書かれた。直しは 7ffd5d230。
- ユーザーの決定を誤って解釈して記録した。WS245 の「5330のみ」を「Tiger Lake を支えない」と書いたが、実際は動作確認済みの機能だった。
- 写し（Master の WS の一覧、ws.md の Phase の表、Queue Board、Bug Board）が本体とずれた。
  - 10-05 に 20〜30 件、10-06〜10-08 の並列の作業の時期に 50 件台に増え、その後は横ばい。
  - 新しい WS・Queue・Bug の行が足されないままになった。
- 詳しくは [audit-20261015/report.md](history/agents-audit-20261015/report.md)。

## 1. 書く前に、出力で確かめる

- **merge**: `merge_one` の出力を読む。「not something we can merge」と「CONFLICT」は失敗。続けて `git log -1 --format='%h %p'` で、親が 2 つの merge の commit ができたことを確かめる。
- **担当の報告**:
  - SHA は `git cat-file -t SHA` で実在を確かめる。
  - 行数・件数・変えた file は、自分で `git show --stat`・`wc`・`grep` で数える。
  - 報告と出力が食い違ったら、説明で片付けずに確かめ直す。
- **試験**: PASS の行を出力で見る。「No such file」や件数 0 は失敗。
- **日付**: host の時計（`date`）で書く。
  - 出来事は今日の日付で書く。予定・期限（公開日など）は未来の日付のままでよい。
  - 直す時は `git blame` で、行が書かれた時刻を見て、出来事か予定かを分ける。
- **記録は事実だけ**。確かめていない事は「未確認」「推測」と書く。

## 2. ユーザーの決定

- 発言の原文は記録しない（2026-10-30 ユーザーの決定、公開の repo に出るため）。決定の要点を書く。Q1 の解釈は「Q1 の解釈」と分けて書く。
- 意味が曖昧な時、既にある事実（動作確認済みの機能など）と食い違う時は、記録の前にユーザーに確かめる。
- WS・Phase に効く決定はその ws.md（と phase.md）に要点で残す。全体に効く決定は AGENTS.md・master-design-policy.md の §11・
  master.md の「今の状態」に、今も有効な形で書く。日付の log（以前の master.md の decisions-log）は 2026-10-27 にユーザーの指示で削除した
  （「master.mdのdecisionは、思い切って削除してしまってOKです。この部分が混乱につながりやすいかもしれないからです。」）。git の履歴に残る。
- 記録同士が食い違う時は壊れた記録として扱い、ユーザーの決定と main の code から正しい形を復元する（多数決・新しい方が正、にしない）。

## 3. 本体と写しを同じ commit で直す

| 本体（owner） | 一緒に直す写し |
| --- | --- |
| phase.md の Status | ws.md の Phase の表の行、Queue Board と担当の lane の attempt の行 |
| ws.md の Status・Resume point | master.md の未完の WS の行（完了・取り下げは workstreams-closed.md へ移す）。新しい WS は作った commit で行を足す |
| Bug の ticket の先頭（再現・処理） | known-bugs.md の行 |
| 担当への割当（Q1 の割当で進める時も） | queue.md の行（Queue ID を取る）、担当の lane、plan/agents/registry.md |
| 担当の起動・終了・merge | master.md の agents・merge の block |
| 次の番号（BUG・WS・q） | master.md の next の block だけに書く。使う前に衝突を確かめる |
| WS の completed | ws.md を完了の形に。Phase の directory と WS だけの試験を消す（今後も使う試験は plan/tools/ へ移して [tools.md](tools.md) に登録）。一覧の行も直す |

## 4. 区切りごとの点検

- 大きな merge の後と、session のラップアップの前に、次を流す。
  - `python3 -I plan/tools/record-audit/status.py | awk '{print $1}' | sort | uniq -c`
  - `python3 -I plan/tools/record-audit/links.py`（相対 path の md の link の切れ）
  - 前回より増えた種類は、その場で直すか、引き継ぎの文書に残す。
  - 2026-10-15 15:55 の基準値: REGDIFF 54・TABLEDIFF 47・WS_NOT_STARTED_BUT_EXEC 19・NOREG 12・PNOSTATUS 35・COMPLETED_WITH_PHASES 12・COMPLETED_WITH_TESTS 17。
  - 2026-10-18 Q1: 全種類 0 に直した。completed の WS で他から参照される Phase・試験を残す時は ws.md の Status の次の行に `<!-- retained-after-completion -->` と理由を書く（点検の道具はこの印の WS を数えない）。
- **ラップアップの順**:
  1. 動いている担当の成果を、確かめてから merge する。
  2. 点検の script を流す。
  3. master.md の「今の状態」の block を今の状態に置き換える。
  4. 引き継ぎの文書を書く。
  5. commit を `git cat-file` で確かめる。
- **引き継ぎの文書**: 実在を確かめた commit だけを挙げる。後から状態が変わったら、古い段落を直す（追記だけで矛盾を残さない）。

## 5. 既知の残り

[audit-20261015/report.md](history/agents-audit-20261015/report.md) の「次の session で直す物」。
