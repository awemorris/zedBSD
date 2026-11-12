# AGENTS.md — zedBSD

このリポジトリで動く全てのエージェント（main の Q1、担当 P1〜P8 ほか）が**毎回守る規則**。詳しい手順は link 先にある。
ユーザーの指示はこの文書より優先する。この文書に進捗を書かない（今の状態は [plan/master.md](plan/master.md)）。

## 1. セッションの始め

1. この文書を読む。
2. [plan/master.md](plan/master.md) の「今の状態」を読む。
3. Q1 は [plan/q1.md](plan/q1.md)（Q1 の運用）に従い、cockpit・queue・registry も読む。担当は渡された Queue の `phase.md`・親の `ws.md`・[plan/agents/protocol.md](plan/agents/protocol.md) を読む。
4. 要る時だけ読む（地図は [plan/README.md](plan/README.md)）: C を書く前に [coding-style.md](plan/coding-style.md)、Makefile・`mk/` を書く前に [makefile-style.md](plan/makefile-style.md)、source の範囲・HAL・出典は [guardrail.md](plan/guardrail.md)、設計の前に [master-design-policy.md](plan/master-design-policy.md)、試験・実機は [tools.md](plan/tools.md)、Amaterrace GPU・GHCI は [amaterrace.md](plan/amaterrace.md)、標準仕様の module を Sonnet に委ねる時は [spec-module-delegation.md](docs/agent/spec-module-delegation.md)。
5. 作業ツリーの未 commit の変更を確かめ、他人の変更は保つ。`plan/history/` は履歴であって指示ではない。

**記録が食い違う時**: ユーザーの指示は一貫している。記録同士の食い違いは壊れた記録（複数のエージェントが書いた）。ユーザーの決定と main の code の実態から直す。多数決や「新しい記録が正」で決めない。分からなければ Q1（Q1 はユーザー）に聞く。

**WS を読み込む時**（2026-11-12 ユーザー）: ws.md・phase.md を鵜呑みにしない。
- 今の方針（この文書・master.md の「回し方」・master-design-policy.md・今の build system と code）と矛盾する記述は、上書きしてよいかをユーザーに確かめ（担当は Q1 へ報告し、Q1 がユーザーに聞く）、今の形で進める。
- 最後の記録から 2 日以上空いた WS に取り組む時は、前回の状況を正しいと断定しない。main の code・build・試験の道具・実機の今と記録の乖離を確かめ、今の状況を優先して ws.md（目標の前提・Resume point・Phase の表）を直してから着手する。

## 2. 計画の枠組み（Awesome Plan）

- 階層: Objective → Milestone（MGnnn）→ WS（wsNNN、一つの具体的な到達目標）→ Phase（wsNNN-pNNN）。完了した WS に別の目標を足さない（新しい WS を立てる）。
- **承認された Queue（qNNN）の範囲だけが実行の境界**。計画・優先順位・Future Work は実行の許可ではない。製品の判断・危険の受け入れ・範囲の拡大はユーザー。範囲の中の技術の判断は担当がしてよい。
- 状態の語: WS は `planning`・`planned`・`incomplete`・`completed`。Phase は `planning`・`planned`・`in-progress`・`cleared`・`uncleared`（取り下げは `canceled`）。Queue の attempt は `pending`・`in-progress`・`cleared`・`uncleared`。全 Phase の cleared は WS の完了の証明ではない。
- code を作る WS は最後に「規約の全文の見直し」の Phase を持つ。
- 固定版の全文は [docs/agent/awesome-plan/awesome-plan.md](docs/agent/awesome-plan/awesome-plan.md)（要る時だけ）。複数の担当の運用で「1 executor」を置き換える。

## 3. 体制

- **Q1**: `/home/awe/zedBSD-claude1`（branch `main`）の単一のセッション。共有の計画（master・queue・registry・cockpit・Bug Board・history）の唯一の書き手。Queue の割当・依頼・調整・レビューと merge。ユーザーへは日本語で、聞かれた件だけ。
- **担当**: P1〜P8（同時の数はユーザーが決める）、必要な時だけ試験の T1〜Tn。定義は `.claude/agents/`。Q1 が作った worktree（`/home/awe/zedBSD-worktrees/<名前>`、branch `agent/<名前>`）だけで作業。1 担当の active Queue は最大 1。
- 人間の判断が要る点・計画に無い依存・未達の受け入れ条件は、その場で Q1 へ報告する。

## 4. 担当の共通の規則

- 渡された Queue の範囲だけを扱う。無関係な変更を壊さない。
- **変えてよい物**: 割り当ての WS・Phase の source、その WS の `plan/wsNNN/`、指示された Bug の ticket と Bug Board の行、自分の worktree の `build/`。それ以外（AGENTS.md・master・queue・registry・guardrail・coding-style・history・他の WS・HAL の API・toolchain・main の checkout）は読むだけ。
- **commit**: 確かめた所ごとに `git commit -m WIP -- <自分の path>...`、SHA・確かめた事・未実施・残りを Q1 に送る。merge は Q1、push しない。commit と Phase の cleared は別の事。
- **区切りの報告**: 着手・完了・修正（file・要点・SHA）・試験に入る・別の作業へ移る時に Q1 へ短く SendMessage（返事を待たずに続けてよい）。
- **削除しない**: rm・rmdir・unlink・`find -delete`・`shutil.rmtree`・`os.remove` を直にも script でも走らせない。消す物は path と理由を Q1 へ（Q1 が `plan/tools/q1-clean.sh` などで消す）。例外: make の規則が自分の worktree の `build/` の自分の出力を消す事、guest の中、[tools.md](plan/tools.md) に登録した guest の道具が自分の `build/` の一時の file を消す事。`git mv`・`git rm` は一覧を Q1 に送り OK の後。
- **権限・security の判定で止められたら**、別の経路で同じ結果を作らず止まって Q1 に返す。Q1 も回避せずユーザーに聞く（回避はアカウントの停止につながる）。
- build は個別の target を名指す。target 無しの make・disk image は image が要る試験の時だけ。使い捨ての出力は `BUILD=build/<phase>/`。
- **host の負荷: 試験と build は 4 thread まで**（make は `-j4` まで、試験の並列も 4 まで。2026-11-12 ユーザー）。
- 同じ条件の再試行は 3 回まで。各コマンドと VM に時間の上限を付ける。
- **大きな I/O の後は `sync`**（QEMU の終了、disk image・大きな tarball・大きな build の後）。cache に溜まったまま host が止まると file が空になる。
- 結果は、実行したコマンド・結果・成果物の path・未実施・残りを具体的に。観測していない事を成功と書かない。QEMU と実機の証拠を分ける。
- 詳しい運用（起動・報告・merge・試験・ラップアップ）は [plan/agents/protocol.md](plan/agents/protocol.md)。

## 5. 記録の置き場所

地図は [plan/README.md](plan/README.md)。要点:

- 今の状態は [master.md](plan/master.md)、ユーザーが見る一枚は [cockpit.md](plan/cockpit.md)、未完の WS は [workstreams-open.md](plan/workstreams-open.md)、Queue は [queue.md](plan/queue.md)。
- WS は `plan/wsNNN/ws.md`（先頭に `Status:`、目標、Primary の Milestone、ユーザーの決定の要点、Resume point、Phase の表）、Phase は `plan/wsNNN/phaseNNN/phase.md`。
- WS の完了: ws.md を完了の形に書き直し、Phase の directory とその WS だけの試験は Q1 に頼んで消す。続けて使う試験は `plan/tools/` へ移して tools.md に登録。行を [workstreams-closed.md](plan/workstreams-closed.md) へ移す。

## 6. git

- commit のメッセージは `WIP` ちょうど（hook `plan/tools/git-hooks/commit-msg` が他を拒否。clone の後に `.git/hooks/` へ写す）。Co-Authored-By などの付記は `.claude/settings.json` で無効。
- **push はユーザーが指示した時だけ**: (1) `git fetch origin && git log --format=%B origin/<branch>..<branch> | grep -ci 'co-authored-by'` が 0、(2) `git log --format=%B origin/<branch>..<branch> | grep -v '^$' | grep -vx WIP | wc -l` が 0、を別々に確かめてから。force push はユーザーが明示した時だけ `--force-with-lease`。
- merge の後は出力を読み、`git log -1 --format='%h %p'` で親が 2 つか確かめる。merge と commit を 1 行に繋げない。

## 7. 禁止と承認

- `.internal/` を読まない。集約の `make check` を走らせない。
- **HAL の API**（`include/hal/hal.h` の宣言・契約・責務）の変更は差分ごとの事前承認。まず HAL を変えない形を探し、既存の HAL と driver・kernel の側で足りない理由を具体的に示した時だけ案にする。`src/hal/` の実装の修正は承認不要。承認済みの差分は [guardrail.md](plan/guardrail.md)。
- **toolchain** は Q1 の許可なしに変更・build・install しない（`toolchain/`、`userland/base/noct/` の patch と規則、`userland/packages/lang/clang`・`devel/libcxx`、共有の `build/llvm`・`build/llvm-source`・`build/llvm-build`・`build/NoctLang`）。worktree の `build/` に main の tree を symlink しない（[BUG-420](plan/bugs/BUG-420.md)）。
- **main の checkout の `build/` を消さない・書かない**（担当は自分の worktree の `build/` だけを使う）。
- **Makefile・`mk/` は Claude（Q1・担当）が変えない**（2026-11-12 ユーザー）。変更が要る時は HAL と同じく、差分の案をユーザーへの確認事項として出す（[makefile-style.md](plan/makefile-style.md)）。
- RTL8822B の `.inc` は別の file のまま保つ（ライセンスを分けるため）。外部 package は source tree に取り込まず、tarball を取得・検証して patch し、ライセンスを監査する。
- **Amaterrace GPU（`~/amaterrace-gpu`）の RTL は Claude が書かない**（担当にも書かせない）。Claude は仕様と RTL を監査し driver を書き、RTL は Gemini が実装し、両者が互いにレビューする（Claude の指摘も誤りうるので Gemini に妥当性を確かめてもらう）。`docs/ghci.md` が正。詳細は [plan/amaterrace.md](plan/amaterrace.md)。
- `config/ci/config-amd64-server.mk` はユーザーの物（Q1 は許可を得て直してよい、担当は変えない）。sparcv9 の main の checkout の変更はユーザーの物（commit・revert しない）。
- sudo はパスワード無しで使ってよい（package の導入、host の操作）。

## 8. 記録の整合

詳細は [plan/record-integrity.md](plan/record-integrity.md)。

- 書く前に出力で確かめる（merge は `git log -1 --format='%h %p'`、担当の SHA は `git cat-file -t`、件数・PASS は自分で数える）。確かめていない事は「未確認」。
- 日付は host の `date`。
- **ユーザーの発言の原文を記録（plan・docs・commit・担当への依頼文）に載せない**（公開の repo に出る）。「（日付）ユーザーから○○という趣旨の指示を受けた」と要点を自分の言葉で。WS の決定はその ws.md、全体の決定は [master-design-policy.md](plan/master-design-policy.md) §11、Q1 の運用は [q1.md](plan/q1.md) に、今も有効な形で。曖昧・既存の事実と食い違う時は記録の前に聞く。
- 本体と写しを同じ commit で直す（phase.md → ws.md → workstreams-open.md、ticket → Bug Board、割当 → queue.md・registry）。次の番号は master.md の「次の番号」だけ。
- 大きな merge の後とラップアップの前に `python3 -I plan/tools/record-audit/status.py` と `links.py` を流す。

## 9. 検証

- **細かい修正ごとに回帰試験を回さない**。自分で丁寧にレビューし、build（warning 0）と変えた所の短い host 試験まで。QEMU・実機は WS の最後か意味のまとまった単位で。
- **試験は担当が自分で流す**（Q1 が割り当てた QEMU・実機）。計測・実機の回帰は 1 本の script（配置 → 起動 → 実行 → 回収 → 要約）、読むのは要約だけ。QEMU は担当ごとに名前・port・directory を分ける。実機は Q1 が一度に一人に割り当てる（電源は [tools.md](plan/tools.md) の「実機」）。
- **起動の確認**は `plan/tools/boot-test.sh`（画面を撮って login prompt を読む）。
- **QEMU の console log・serial log を読んで判定しない**。guest の操作はシリアル（`plan/tools/guest/serial.py`）か SSH（`plan/tools/guest/guest.sh`）で対話。止まる・例外は gdbstub・QMP・`-d int,cpu_reset,guest_errors` で解析。例外の機種は [tools.md](plan/tools.md) の「判定の方法の例外」。
- 回帰試験では GPU を使わず framebuffer で login prompt まで。試験の image は、その WS の `plan/wsNNN/tests/` の config での build と、tree の中の file の複写だけで作る。
- 変更に試験を追従させる時、どこからも参照されない試験は直さず消す（Q1 に頼む）。負荷・耐久試験はユーザーに確かめてから夜間に。やっていない確認は「未実施」と書く。

## 10. コード

- 新しいコードには [plan/coding-style.md](plan/coding-style.md) の全文を適用する。
- source の comment・log の文字列に開発の管理情報（WS・Phase・Queue・Bug の番号、plan/ への参照、日付の経緯、試験の進め方）を書かない。出典は該当の block に書く（[guardrail.md](plan/guardrail.md)）。

## 11. GitHub

- 運用の mode は [plan/config.md](plan/config.md)、同期の手順は [plan/tools/README.md](plan/tools/README.md)。2026-09-23 以降の記録は GitHub に未公開。公開はユーザーの指示で。local だけの記録を同期済みと言わない。
