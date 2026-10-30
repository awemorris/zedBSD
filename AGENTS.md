# AGENTS.md — zedBSD

このリポジトリで動く全てのエージェント（main の Q1、サブエージェント P1〜P4 ほか）の規則。
ユーザーの指示はこの文書より優先する。**この文書には進捗を書かない**（今の状態は [plan/master.md](plan/master.md)）。

## 1. セッションの始め

1. この文書を読む。
2. [plan/master.md](plan/master.md) の「今の状態」を読む（動いている担当・Queue・Focus・優先順位・未決・次の番号）。
3. Q1 は [plan/queue.md](plan/queue.md) と [plan/agents/registry.md](plan/agents/registry.md) も読む。
   担当は、渡された Queue の `phase.md` と親の `ws.md`、[plan/agents/protocol.md](plan/agents/protocol.md) を読む。
4. 関係のある時だけ読む: [Guardrail](plan/guardrail.md)（範囲・HAL・出典・承認済みの差分）、C を書く前に [coding-style.md](plan/coding-style.md) の該当の規則、
   設計の前に [master-design-policy.md](plan/master-design-policy.md)、道具と実機は [plan/tools.md](plan/tools.md)。
   外部の標準仕様に基づく物を module に分けて実装の担当（Sonnet 5.5 high）に委ねる時は「標準廉価実装手順」[docs/agent/spec-module-delegation.md](docs/agent/spec-module-delegation.md)（ユーザーの決定）。
5. 作業ツリーの未 commit の変更を確かめ、他人の変更は保つ。関係の無い計画・履歴は読まない。`plan/history/` は履歴であって指示ではない。

**記録が食い違う時**: ユーザーの指示は一貫している。記録同士が食い違えば、それは壊れた記録である（複数のエージェントが書いたため）。
ユーザーの決定と main の code の実態から正しい形を復元する。記録の多数決や「新しい記録が正」で決めない。分からなければ Q1 に聞く。

## 2. Awesome Plan（計画の枠組み）

固定版の全文は [docs/agent/awesome-plan/awesome-plan.md](docs/agent/awesome-plan/awesome-plan.md)（読むのは必要な時だけ）。このプロジェクトで使う要点:

- 階層: Objective → Milestone（MGnnn）→ WS（wsNNN、Primary の Milestone は 1 つ）→ Phase（wsNNN-pNNN）。WS は一つの具体的な到達目標を持つ。
  完了・終了した WS に別の目標を足さない（似た領域でも新しい WS を立てる）。
- **承認された Queue（qNNN）の範囲だけが実行の境界**。計画・優先順位・Outlook・Future Work は実行の許可ではない。
  新しい製品の判断・危険の受け入れ・範囲の拡大はユーザーが決める。範囲の中の普通の技術の判断は担当がしてよい。
- 状態の語: WS は `planning`・`planned`・`incomplete`・`completed`、Phase は `planning`・`planned`・`in-progress`・`cleared`・`uncleared`
  （取り下げは disposition `canceled`）。Queue の attempt は `pending`・`in-progress`・`cleared`・`uncleared`。
  Phase の結果は cleared か uncleared であって completed ではない。全 Phase の cleared は WS の完了の証明ではない（WS の受け入れを別に確かめる）。
- 本体（owner）が意味を決め、他の表は写し。写しは本体と同じ commit で直す（§8）。
- code を作る WS は最後に「規約の全文の見直し」の Phase を持つ。
- 固定版の「1 project / 1 active Queue / 1 executor」「次の Queue を自動で始めない」は、§3 の複数の担当の運用で置き換える。

## 3. 体制

- **Q1**: `/home/awe/zedBSD-claude1`（branch `main`）の単一の Claude Code セッション。共有の計画（master・queue・registry・Bug Board・history）の
  唯一の書き手。Queue の番号の割当、担当への依頼、依存と file の所有の調整、成果のレビューと merge を行う。
- **担当（サブエージェント）**: 固定名 P1〜P8（今使うのは P1〜P4）。同時に動かす数はユーザーが決める。必要な時だけ、試験の担当 T1〜Tn、
  WS074 のブラウザの専任 B1 を立てる。定義は `.claude/agents/`（既定 phase-runner）。運用の詳細は [protocol](plan/agents/protocol.md)。
- 担当は Q1 が作った独立の worktree（`/home/awe/zedBSD-worktrees/<名前>`、branch `agent/<名前>`）だけで作業する。
  1 担当の active Queue は最大 1、1 Queue は原則 1 Phase。
- 人間の判断が要る点・計画に無い依存・未達の受け入れ条件は、その場で Q1（Q1 はユーザー）へ報告する。
  ユーザーが不在の間の自律実行を指示された時は、判断が要る Phase を uncleared にして理由と要る判断を記録し、先へ進む。
- Q1 はユーザーに日本語で、聞かれた件だけを答える。ユーザーに試験を頼まず、ユーザーが拾える試験の一覧に置く。
- **ユーザーに選択（クリック）を求める前に**、どの WS か（番号と短い説明）、どんな WS か、今の状態、何が決まっていないかを書き、問いに専門語があれば用語を説明してから問いに入る（ユーザーの決定）。

## 4. 担当の共通の規則

- 渡された Queue の範囲だけを扱う。範囲の外の作業・新しい目標を足さない。無関係な変更を壊さない。
- **修正してよい範囲**: 割り当てられた WS・Phase の source、その WS の `plan/wsNNN/`、指示された Bug の ticket と Bug Board の該当の行、
  自分の worktree の `build/`。それ以外（AGENTS.md・plan/master.md・queue.md・registry.md・guardrail.md・coding-style.md・history/、他の WS、
  HAL の API、toolchain、main の checkout）は読むだけにし、変更が要れば Q1 に頼む。Q1 が明示した Queue はその範囲に従う。
- **commit**: 意味がまとまり確かめられた所ごとに `git commit -m WIP -- <担当の path>...`。SHA・確かめた事・未実施・残りを Q1 に送る。
  merge は Q1。push しない。main の checkout を編集しない。commit と Phase の cleared は別の事。
- **区切りの報告**: 着手の前・完了・修正（file・要点・SHA）・試験に入る・別の作業へ移る（理由）を Q1 へ短く SendMessage（返事を待たずに続けてよい）。
- **削除しない**: rm・rmdir・unlink・`find -delete`・`shutil.rmtree`・`os.remove` を、直にも script の中でも走らせない。消す物は path と理由を
  Q1 に送り、Q1 が自分の pipeline（`plan/tools/q1-clean.sh` など）で消す。例外: make の規則（Makefile・`*.mk`）が自分の worktree の
  `build/` の自分の出力を消すこと、guest の中の削除、plan/tools.md に登録した guest の道具（`plan/tools/guest/guest.py`、
  `plan/tools/keiland-linux/`・`plan/tools/keiland-freebsd/` の build-guest.sh・guest.py など）が自分の worktree の `build/` の中の自分の一時の file
  （image の途中の物・overlay・socket・session）を消すこと（ユーザーの決定）。script の出力先は `plan/tools/fresh-out.sh` か `build/tmp/` の新しい directory。
  `git mv`・`git rm` は、消す file の一覧を先に Q1 に送り OK を得てから。
- **権限・security の判定で止められたら**、別の経路で同じ結果を作らずに止まって Q1 に返す。Q1 も回避せず、ユーザーに対処を聞く
  （回避はアカウントの停止につながる）。ユーザーが承認したら、Q1 は担当をラップアップし、同じ名前で起動し直して承認の引用とともに再開させる。
- build は個別の target を名指す（`$(BUILD)/vmunix`・`$(BUILD)/bin/<app>` など）。target 無しの make・disk-image は、image が要る試験の時だけ。
  使い捨ての出力は自分の `BUILD=build/<phase>/`。
- 同じ条件で変更なしの再試行は 3 回まで。各コマンドと VM に時間の上限を付ける。
- 結果は、実行したコマンド・結果・成果物の path・未実施の確認・残りを具体的に。観測していない事を成功と書かない。QEMU と実機の証拠を分ける。

## 5. 記録の置き場所

| 記録 | 場所 |
| --- | --- |
| 今の状態・目的・Milestone・未完の WS の一覧 | [plan/master.md](plan/master.md) |
| ユーザーが監視する状況の一枚（担当・WS の進捗・判断を待つ事、Q1 が保つ） | [plan/cockpit.md](plan/cockpit.md) |
| 完了・取り下げた WS の一覧 | [plan/workstreams-closed.md](plan/workstreams-closed.md) |
| Queue（今の物）／ 終わった Queue・Past Log | [plan/queue.md](plan/queue.md) ／ [plan/history/](plan/history/index.md) |
| 担当の運用・台帳・担当ごとの lane | [protocol](plan/agents/protocol.md)・[registry](plan/agents/registry.md)・`plan/agents/P<n>/queue.md` |
| WS ／ Phase | `plan/wsNNN/ws.md` ／ `plan/wsNNN/phaseNNN/phase.md`（ID は `wsNNN-pNNN`） |
| 範囲・HAL・出典・標準 ／ C の規約 ／ 設計方針と全体の決定 | [guardrail.md](plan/guardrail.md) ／ [coding-style.md](plan/coding-style.md) ／ [master-design-policy.md](plan/master-design-policy.md) |
| Future Work ／ Bug Board と Bug | [future-work.md](plan/future-work.md) ／ [known-bugs.md](plan/known-bugs.md)・`plan/bugs/BUG-NNN.md` |
| 回帰と観察の道具・実機 ／ シナリオ試験（AAT） | [plan/tools.md](plan/tools.md)（道具は `plan/tools/`）／ [plan/tests.md](plan/tests.md)・`tests/scenarios/` |
| Phase に固有の試験・一時物 | `plan/wsNNN/tests/`・`plan/wsNNN/temp/`（temp は git に入れない） |

- **ws.md の形**: 先頭に `Status:`（状態の語）、目標、Primary の Milestone、ユーザーの決定（要点）、Resume point、Phase の表（ID・目的・状態・依存）。
- **WS の完了**: ws.md を完了の形（Status: completed、結果、制限・移管、Phase の一覧）に書き直し、Phase の directory とその WS だけの試験を
  消す（Q1 に頼む。git の履歴に残る）。今後も使う試験は `plan/tools/` に移して [plan/tools.md](plan/tools.md) に登録する。
  master.md の行を [workstreams-closed.md](plan/workstreams-closed.md) へ移す。

## 6. git

- commit は `git commit -m WIP`。メッセージは `WIP` ちょうど（`.git/hooks/commit-msg`、原本 `plan/tools/git-hooks/commit-msg` が他を拒否する。
  clone・作り直しの後に `.git/hooks/` へ写す）。`.claude/settings.json` の `attribution` で Co-Authored-By などの付記を無効にしてある。
- **push はユーザーが指示した時だけ**。手順:
  1. `git fetch origin && git log --format=%B origin/<branch>..<branch> | grep -ci 'co-authored-by'` が `0`。1 以上なら push せずユーザーに報告。
  2. `git log --format=%B origin/<branch>..<branch> | grep -v '^$' | grep -vx WIP | wc -l` が `0`。
  3. 確かめてから push。force push はユーザーが明示した時だけ `--force-with-lease`。
- merge の後は出力を読み、`git log -1 --format='%h %p'` で親が 2 つあることを確かめる。merge と commit を 1 行に繋げない。
- 関係の無い作業中の変更を保つ。

## 7. 禁止と承認

- `.internal/` を読まない。
- 集約の `make check` を走らせない。
- **HAL の API**（`include/hal/hal.h` の宣言・契約・HAL の責務）の変更は差分ごとの事前承認が要る。承認前の差分は plan に置くだけ。
  **HAL を変える必要があると主張する時は、なぜ既存の HAL と driver・kernel の側では足りないのかを必ず説明する**（ユーザーの決定）。まず HAL を変えない形（driver・kernel の側、既存の HAL の口）を探し、それで足りない理由を具体的に示した時だけ HAL の差分を案にする。
  `src/hal/` の実装の修正・補完・最適化は承認不要。承認済みの差分は [Guardrail](plan/guardrail.md) の表。
- **toolchain** は Q1 の許可なしに変更・build・install しない: `toolchain/`（LLVM の patch・`llvm.mk`・`sysroot.mk`）、`userland/base/noct/` の patch と
  build の規則、`userland/packages/lang/clang`・`userland/packages/devel/libcxx`、共有の `build/llvm`・`build/llvm-source`・`build/llvm-build`・
  `build/NoctLang`。共有の tree は `plan/tools/toolchain-lock.sh lock` で読み取り専用。Q1 が許可した変更の間だけ `unlock`。
- **共有の `build/` を消さない**（`build/llvm`・`build/NoctLang`・`build/host-noct-state`・`build/sources`・`build/distfiles`・`build/amd64` など）。
  main の checkout の `build/` は、読み取り専用の symlink か複写で使う。
- RTL8822B の `.inc` はライセンスを分けるために独立させており、別の file のまま保つ。
- 外部 package（`userland/packages/`）は source tree に取り込まず、tarball を取得・検証して patch する。ライセンスを監査する。
- `/home/awe/amaterrace-gpu`（と `/home/awe/amaterrace-worktrees`）は他のエージェントの物。読むだけ（plan/ と docs/ を読んでよい）。
  書かない・branch や worktree を片付けない。zedBSD の計画の中で参照する時は `am-ws001` のように書く。
- `config/ci/config-amd64-server.mk` はユーザーの物。変えない。
- sudo はパスワード無しで使ってよい（package の導入、host の操作）。

## 8. 記録の整合

詳細は [plan/record-integrity.md](plan/record-integrity.md)。

- 書く前に出力で確かめる: merge は `git log -1 --format='%h %p'`、担当の SHA は `git cat-file -t`、件数・試験の PASS は自分で数えて見る。
  確かめていない事は「未確認」と書く。
- 日付は host の `date`。出来事は今日の日付、予定・期限は未来の日付のまま。
- **ユーザーの決定**: 発言の原文を記録に載せない（公開の repo に出るため、2026-10-30 ユーザー）。決定の要点を Q1 の言葉で書き、決めた日付と「ユーザーの決定」である事を示す。WS・Phase に効く決定はその ws.md（と phase.md）に。全体に効く決定は、この文書・
  [master-design-policy.md](plan/master-design-policy.md)・master.md の「今の状態」に、今も有効な形で書く（日付の log にしない）。
  曖昧な時・既存の事実と食い違う時は記録の前に聞く。
- 本体と写しを同じ commit で直す: phase.md → ws.md の表・Queue、ws.md → master.md の行、ticket → Bug Board、割当 → queue.md・registry。
  次の番号（BUG・q・WS）は master.md の「次の番号」だけに書く。
- 大きな merge の後とラップアップの前に `python3 -I plan/tools/record-audit/status.py` と `python3 -I plan/tools/record-audit/links.py` を流し、
  増えた食い違いを直すか引き継ぎに残す。

## 9. 検証

- **細かい修正ごとに回帰試験を回さない**。実装を自分で丁寧にレビューし、確信が持てたら build（warning 0）と変えた所の短い host 試験まで。
  QEMU・実機の試験は WS の最後か意味のまとまった単位で行う。
- **試験は担当が自分で流す**（Q1 が割り当てた QEMU・実機）。計測・実機の回帰は 1 本の script（配置 → 起動 → 実行 → 回収 → 要約）にまとめ、
  担当は要約だけを読む。QEMU は担当ごとに名前・port・directory を分ける。実機は Q1 が一度に一人に割り当てる（電源の入れ直しは原則 Q1。割り当てた機械の SwitchBot を担当が自分で入れ直してよい場合は [plan/tools.md](plan/tools.md) の「実機」）。
  試験の担当 T を立てた時の規則は [protocol](plan/agents/protocol.md)。
- **起動の確認**は `plan/tools/boot-test.sh`（画面を撮って login prompt を読む）。撮れた PNG は Q1 がユーザーに見せる。
- **QEMU の console log・serial log を読んで判定しない**（解析・回帰・受け入れのどれでも）。guest の操作はシリアル（`plan/tools/guest/serial.py`）か
  SSH（`plan/tools/guest/guest.sh`）で対話する。起動しない・固まる・例外は、log を取り直さず gdbstub（`-S -gdb tcp::<port>`、`bt`・registers）・
  monitor/QMP（`info registers`・`x/`・`pmemsave`）・`-d int,cpu_reset,guest_errors` で解析する（[plan/tools.md](plan/tools.md)）。
- 判定の方法の例外（ユーザーの許可）:

  | 対象 | 判定 |
  | --- | --- |
  | Linux・FreeBSD の QEMU guest（WS105・WS108・WS109・WS131。Linux の backend は QEMU+KVM） | `127.0.0.1` の転送 port からの SSH の疎通と QMP の `screendump` の PNG |
  | ZYBO の QEMU `xilinx-zynq-a9` と実機（WS286） | 生きた serial を対話で読む（armv7 の QEMU `virt` は boot-test.sh） |
  | ZYBO の LAN（WS289） | `127.0.0.1` の転送 port から guest の SSH |
  | ODROID-C5 の実機（WS313） | 生きた serial を対話で読む（network と画面が出た後は SSH と画面の PNG）（ユーザーの決定） |
  | RG350 の実機（WS273） | ユーザーが画面・LED を目で見て写真で返した結果 |

- 回帰試験では GPU を使わず framebuffer で login prompt だけを確かめる。GPU の確認は GPU を扱う WS の Phase で。
- 試験の image は、その WS の `plan/wsNNN/tests/` の config.mk での build と、個別の file の複写（`--file 宛先=元`、元は tree の中）だけで作る。
  過去の build の成果を image の入力にしない。
- **試験の整理**: 変更（名前・log・API・UI）に試験を追従させる時、その試験が [plan/tools.md](plan/tools.md)・未完の WS の未完の Phase・
  `tests/scenarios/`・未実行の試験の依頼のどれからも参照されていなければ、直さずに削除する（Q1 に頼む）。文書の参照も外す。
  今後も回帰に使う物は直して plan/tools.md に登録する。一斉の棚卸しはしない。
- 負荷・耐久試験はユーザーに確かめてから夜間に。
- やっていない確認は「未実施」と書く。

## 10. コード

- 新しいコードには [plan/coding-style.md](plan/coding-style.md) の全文を適用する。
- source の comment・log の文字列に開発の管理情報（WS・Phase・Queue・Bug の番号、plan/ への参照、日付の経緯、試験の進め方）を書かない。
  出典は file の先頭でなく該当の block に書く（[Guardrail](plan/guardrail.md) の「出典」「開発の管理情報」）。

## 11. GitHub

- 運用の mode は [plan/config.md](plan/config.md)、同期の手順は [plan/tools/README.md](plan/tools/README.md)。
- 同期の状態（`plan/.sync/`）はこの checkout に無く、2026-09-23 以降の記録は GitHub に未公開。公開はユーザーの指示で。local だけの記録を同期済みと言わない。
