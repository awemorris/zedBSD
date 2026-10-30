# 担当（サブエージェント）の運用

[AGENTS.md](../../AGENTS.md) の §3（体制）・§4（担当の共通の規則）・§9（検証）を具体化する。実装の権限は各 Queue の承認された範囲に残る。
担当の台帳は [registry.md](registry.md)、Queue は [queue.md](../queue.md)。

## 1. 役割

- **Q1**: 計画・Queue の番号・担当への依頼・依存と file の所有の調整・レビューと merge。共有の計画の唯一の書き手。
- **P1〜P8**（今は P1〜P4）: 設計と実装と試験。同時に動かす数はユーザーが決める。担当ごとに WS を固定すると文脈が続く。
- **T1〜Tn**（試験の担当）・**B1**（WS074 のブラウザ）: ユーザーの指示か Q1 の判断で必要な時だけ立てる。
- 定義（`.claude/agents/`）の選び方:

| 定義 | 使う時 |
| --- | --- |
| phase-runner（Opus 5.5、medium） | 既定 |
| phase-runner-high（Opus 5.5、high） | kernel・driver・バグの修正 |
| phase-runner-mid（Opus 5.5、medium） | 試行の多い WS（app） |
| phase-runner-low（Sonnet 5.5、high） | 標準廉価実装手順の実装の担当、WS074（Web ブラウザ）の Phase |
| document-writer（Sonnet 5.5、high） | 文書の作成（人間が読む文書に寄ったモデルなので Opus より向く） |
| kernel-and-driver-designer | 設計の Phase（code は書かない） |
| design-reviewer | 設計ごとの敵対的レビュー（設計の担当が呼ぶ） |
| bug-analyzer ／ bug-fixer | Bug の原因の解析だけ ／ 解析を受けた修正 |
| conformance-reviewer | 「規約の全文の見直し」の Phase（書き方だけ） |
| hard-debugger | ユーザーが指定した難しい Phase・debug |
| test-runner | 試験の担当 T |
| browser-runner-b1 | 旧 WS074 の B1（2026-10-30 から WS074 は phase-runner-low） |

## 2. 起動と Queue

- Q1 は worktree を作る: `git worktree add /home/awe/zedBSD-worktrees/<名前> -b agent/<名前> main`。古い worktree・branch を消したり使い回したりしない。
  作った後に `chmod 600 plan/tmp/guest/id_ed25519`（git は mode を追わず 0664 になり、ssh が鍵を拒む。2026-10-29 P2 の measure.sh が「zedBSD did not come up」と誤って出した原因）。
- 起動の指示に入れる物: 担当名、Queue の ID と attempt、Phase、範囲と承認の原文、worktree と branch、所有の path、前提の成果、試験の方法（QEMU の名前・port、実機の割当）、
  予約の次の Queue、報告の規則（§3）。
- 1 担当の active Queue は最大 1。予約の次の Queue は、前の Queue の後に同じ担当が続けてよい（その Queue の承認を確かめる）。
  依存する Queue は、前の成果が main に merge され確かめられてから始める。Queue が空なら次の Phase を自分で選ばず Q1 を待つ。
- Queue の未承認・範囲の変更・未検証の依存・競合する source・取り消しがあれば、始めずに Q1 へ返す。
- Q1 は依存の graph を先に見て、Focus・優先順位・WS の担当・実機の空きで割り当てる。同じ Phase の attempt を二重に割り当てない。
- 返却した（終わった）担当に message を送らない。送るとその担当が再開し、同じ worktree に書き手が 2 つになる。続きは新しい起動で頼む。

## 3. 区切りごとの報告

担当は区切りごとに Q1 へ短く SendMessage する（返事を待たずに続けてよい）: これから取り組むこと（着手の前）、完了したこと、
修正したこと（file・要点・SHA）、試験に入ること、別の作業に移ること（理由）。

## 4. commit と merge

1. 担当は意味がまとまり確かめられた所ごとに `git commit -m WIP -- <担当の path>...`。未完の Phase を cleared に見せない。
   Q1 へ: Queue と attempt、基点と先頭・末尾の SHA、path、行った確かめと未実施、Phase の状態、残りと次の安全な地点。
2. Q1 は SHA を `git cat-file -t` で確かめ、diff の file の一覧を見て（捨てた変更が残っていないか）、main の checkout で `git merge --no-ff --no-commit <SHA>`。
   競合を解いて `git commit -m WIP`、`git log -1 --format='%h %p'` で確かめる。merge・試験の出力は読んでから記録する。
3. Q1 が競合の解決で branch と差を作ったら担当に伝え、次の依存の作業の前に main を取り込ませる。同じ commit を二重に当てない。push しない。
4. commit・merge と、Phase の cleared・Queue の cleared・WS の completed は別の事。

## 5. 試験

- 担当は自分で試験を流す（[AGENTS.md](../../AGENTS.md) §9）。計測・実機の回帰は 1 本の script（配置 → 起動 → 実行 → 回収 → 要約）。
- QEMU は担当ごとに名前・port・directory を分ける。image の build が重なる時は `flock /tmp/zedbsd-image-build.lock`。
- 実機は Q1 が一度に一人に割り当てる（[tools.md](../tools.md) の「実機」）。電源の入れ直しは原則 Q1 が `plan/tools/switchbot.py`（担当が自分で入れ直してよい機械は [tools.md](../tools.md) の「実機」）（ユーザーから委任済み）。
  入れ直しても戻らなければ Q1 がユーザーに知らせる。
- 試験の担当 T を使う時: 実装の担当は試験の依頼（image の作り方、流す試験、合否の基準）を Q1 に送り、Q1 が `plan/agents/T<n>/requests.md` に積む。
  実装の担当は結果を待たず次の Queue へ移り、その Phase の Status に `test-wait（T<n>-NNN）` と書く。Q1 が結果で印を外す（PASS → cleared、FAIL → 直す attempt）。
  T は source・試験の script を直さず、FAIL の解析もしない。image は依頼の tree の SHA で作り、結果に SHA を書く。
  T は自分の worktree の `build/` で image に入れる target の clang・libcxx の package を build してよい（共有の toolchain は lock のまま）。

## 6. ラップアップと障害

- **通常**: Q1 の依頼で新しい Queue を始めない。今の作業を commit できる地点まで進め、WIP commit・確かめ・未達・再開点・未着手の予約を返して終了する。
  受け入れが未達なら Q1 がその attempt を uncleared として残す。
- **urgent**: 新しい作業を始めず差分をすぐ保存する。未追跡の file は `git add -N`、`git diff --binary HEAD -- <担当の path>` を `plan/wsNNN/temp/` へ。
  基点の SHA・patch の SHA-256 と path・実行中の process・未実施・再開の手順を Q1 に送って終了。patch は未検証の救出物。
- 強制終了・利用の上限の時は、Q1 が worktree・branch・未 commit の差分・最後の merge の依頼を調べて回収する。急な停止を成功と扱わない。
- コンテキストの残りが少なければ checkpoint を作って Q1 に知らせ、残りは同じ名前の新しい起動へ引き継ぐ。

## 7. 権限と削除

- 権限の確認・security の判定で止められたら、担当は止まって Q1 に返す。Q1 も別の経路で回避せず、ユーザーに内容と対処の案を示す。
  ユーザーが承認したら、Q1 は担当を通常のラップアップで終え、同じ名前で起動し直し、承認の引用とともに再開させる。Q1 は止められた操作を代わりに行わない。
- 削除は Q1 の pipeline（[AGENTS.md](../../AGENTS.md) §4）。担当は path と理由を Q1 に送り、削除を待たずに続けてよい（新しい出力先は `fresh_out` か `build/tmp/`）。

## 8. 記録

- [registry.md](registry.md): 担当、定義、Queue、worktree と branch、状態（running・returned）、最後の merge。
- [queue.md](../queue.md) と担当の lane（`plan/agents/P<n>/queue.md`）: Queue ごとの承認・範囲・Phase・attempt・結果。chat だけを台帳にしない。
- 共有の同期（`.sync/`、GitHub）は Q1 だけが書く。GitHub の公開の保留と local の成果を混同しない。
