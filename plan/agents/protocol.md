# サブエージェント別 Queue 運用

この文書は[AGENTS.md](../../AGENTS.md)のzedBSD固有規則を具体化する。固定版Awesome Planの「1 project / 1 active Queue / 1 executor」「次Queueを自動開始しない」は、ユーザー指示（2026-10-02）の範囲で置き換える。実装の権限は**各Queueの承認済みscope**に残る。

## 役割と並列数

- **main（Q1）**: `/home/awe/zedBSD-claude1` の `main` で動く単一の Claude Code セッション。共有計画・同期キャッシュの唯一の書き手、Queue ID の割当者、依存/ファイル所有の調整者、成果のレビュー・merge 担当。
- **サブエージェント**: 固定名 **P1〜P8** の8つ（2026-10-02 ユーザー）。Claude Code の Agent tool で起動し、定義は `.claude/agents/`（既定 phase-runner、試行の多い WS は phase-runner-mid）。同じ名前の担当は一度コンテキストを埋めたら、なるべく長く動かし続ける。Q1 はその担当へ次の仕事を絶え間なく依頼・予約する（Queue の終わりで担当を終了させない）。
- **N**: 同時に動かす担当の数。ユーザーが指定する（2026-10-02 N=4。ユーザー「週間利用制限はあってないようなもの」）。P1 から順に使い、N を超える名前は起動しない。利用制限を観測できなければ推測で増減しない。
- 可能なら担当ごとに WS を固定して文脈を継続する。WS 完了後は別 WS へ再配属できる。過去の P1〜P10 の割当・Queue は [history](../history/index.md) の履歴であり、同名の新しい担当の権限や状態にはならない。

## Queue と割当

- [Queue Board](../queue.md)は全担当の索引と全体Outlook。各担当の詳細Queue laneは `plan/agents/P<番号>/queue.md` に作る（未配属なら作らない）。`qNNN`は全体で一意、履歴は `plan/history/queue-qNNN.md`。1担当にactive Queueは最大1、1Queueは原則1Phase/attempt。他の担当は独立したactive Queueを持てる。mainはQueue Boardに担当・dispatch順・承認元・scope・依存・状態・merge checkpointを投影する。担当laneにはmerge request ID、提出SHA、統合SHA、ACK/未統合理由を順に残し、chatだけを台帳にしない。
- 起動指示には担当名、WS、worktree/branch、すぐ実行するQueueのID/承認scope/Phase/時限、読み込む規則、所有path、必要な前提成果と検証、予約済みの次Queue、連絡方法を入れる。Queue未承認、scope変更、未検証依存、競合source、キャンセルがあれば開始せずmainへ返す。
- 予約済みの次Queueはmainの明示的な投入であり、同じ担当が現在Queue終了後も継続できる。ただし次Queue自体の正確な実装承認を再確認する。依存Queueの成果を使う場合はmainによる統合commitと必要な検証/clearanceを確認してから開始する。独立Queueなら前のcommitの統合待ち中も継続可能。Queueが空なら勝手に次Phaseを選ばず、同じ担当のまま main の指示を待つ（待機は新Queueの実行ではない）。
- mainは依存グラフを先に見る。次にfocused goal/WS優先度、WS affinity、利用枠・回帰資源を使って割り当てる。同じPhase attemptを二重割当しない。重なるsource、QEMU/共有build、toolchain等は衝突を調整し、片方を待たせる。
- 承認はuserの具体指示または既存のexact-scope承認を引用する。mainのdispatchは承認済みQueueの担当/順番を指示するもので、新しい製品scopeや例外を自動承認しない。Phase clearanceとQueue item clearanceは成果/試験の証拠で別々に判定する。
- 既読の文脈を毎Queue全文再投入せず、差分の規則と新しいscopeだけを補足する。

## 区切りごとの報告（2026-10-03 ユーザー）

ユーザー「各サブエージェントに、今後は、何に取り組むかを事前にメインエージェントに伝えるように、依頼してください。何が完了したか、何を修正したか、動作確認に入るか、別な修正に取り組むか、などもです。」→ 担当は作業の区切りごとに Q1 へ短く SendMessage する（返事を待たずに続けてよい）: これから取り組むこと（着手の前）、完了したこと、修正したこと（file・要点・commit の SHA）、動作確認に入ること、別の修正に移ること（理由）。起動の指示にもこの規則を入れる。

## 試験の担当 T1（2026-10-03 ユーザー）

ユーザー:「テストはテストエージェントT1に依頼します。テストエージェントは、複数のテスト依頼を可能な限りマージして、1つのQEMUインスタンスでまとめて実行します。テストが長くなりすぎないようにします。負荷テスト、耐久テストは別枠で、夜間に回しましょう。」「細かい修正で回帰テストを行うことをやめます。きちんと実装をレビューして、確信が持てたら、WSの最後に、まとまった単位でT1に依頼する。T1も、まとまった単位でテストをマージして実行する。1つしかなければ単体で実行してもよい。T1は、テストが長くなりすぎないように気をつける。ほかをブロックするので。耐久試験はユーザに確認して夜間に実行する。」

- **実装の担当（P1〜P8）**: 細かい修正ごとに回帰を回さない。実装をレビューして確信を持ち、build（warning 0）と変えた所の短い host 試験まで。QEMU・実機の試験は WS の最後（まとまった単位）で T1 に依頼する。依頼には image（commit と build の手順、または image の path）、流す試験と合否の基準、結果の返し先を書く。自分で QEMU を起動しない。
- **T1**: 試験だけを行い、source を直さず、FAIL の解析もしない（解析は依頼主か bug-analyzer）。届いた依頼を可能な限りまとめ、1 つの QEMU のインスタンスで流す。依頼が 1 つなら単体でよい。QEMU は host 全体で同時に 1 つ。1 回の試験が長くなりすぎないようにし、長くなる組は分けて Q1 に順を相談する。結果（PASS/FAIL、PNG、log の場所）を依頼主と Q1 に返す。
- **依頼の後は待たない（2026-10-03 ユーザー）**: 「テスト依頼したあと、依頼元のサブエージェントは、別なWSの作業に移っていいことにします。テストは時間がかかり、いつ終わるかわからないので、待つのではなくて、別な作業をしてスループットを上げます。」→ 依頼主は T1 に依頼したら、その Queue を「実装済み・T1 の試験待ち」で区切り（commit と merge 依頼まで済ませる）、Q1 が投入した次の Queue（別の WS でよい）へ移る。依頼した Phase は T1 の結果が出るまで cleared にしない（Phase は in-progress のまま、Queue の attempt は試験待ちとして記録）。結果は Q1 が受けて判定する: PASS なら Q1 が Phase を cleared にし、FAIL なら Q1 が直す仕事を Queue にして、なるべく元の担当（文脈を持つ）に割り当てる。1 担当の active Queue は最大 1 のまま（試験待ちの Queue は active に数えない）。
- **pipeline（2026-10-03 ユーザー）**: 「P1,P2はどんどんバグを修正して、未確認の状態でテストをT1にポストして予約し、P1,P2は次のバグ修正に移ってください。その次にT1にテストをポストしたときに、結果を取得するのがいいです。」→ 実装の担当は試験を T1 に投げて（予約）すぐ次の修正へ。T1 は結果を台帳に書き Q1 には都度知らせるが、実装の担当へは割り込まず、その担当が次に試験を投げた時の返事でそれまでの結果をまとめて返す（pull）。FAIL は次の区切りで直して再依頼する。
- **試験の直しも同じ（2026-10-03 ユーザー）**: 「試験の修正は行うが、試験実行はT1に依頼することにします。」→ 古い試験が機能と合わない時は、見つけた担当がその場で試験を直す（Bug に回さない）。直した試験を流すのは T1。
- **試験の担当を 2 つに（2026-10-03）**: 2026-10-03 user「T2を立てて、T1に予約してあるがまだ実行されていないテストを、半分移管してください。ホストの空きメモリはあと1つqemuを立てても大丈夫と思います。」 → T2 を T1 と同じ役（定義 `.claude/agents/test-runner.md`、worktree `/home/awe/zedBSD-worktrees/t2`、台帳 `plan/agents/T2/requests.md`）で立てる。QEMU は host 全体で同時に 2 つまで（T1・T2 に 1 つずつ）。image の build は `flock /tmp/zedbsd-image-build.lock` で同時に 1 本にする。
- **負荷・耐久試験**: 別枠。Q1 がユーザーに確かめてから夜間に T1 で流す。
- **Q1**: T1 の受付の順を決める（急ぎ、例えば安定版の image の確認を前に）。T1 の定義は `.claude/agents/test-runner.md`（2026-10-03 作成）、worktree `/home/awe/zedBSD-worktrees/t1`（branch `agent/t1`）、依頼の台帳は `plan/agents/T1/requests.md`。QEMU の道具の host 全体の lock は未実施。

## 2026-10-04 17 時からの試験の集約

user「テストはメインエージェントが集約してサブエージェントT1に送り、T1はキューにあるテストを可能な限りバッチ化して1回のqemu起動に詰めて実行します。P1,P2はテスト中に待機することなく、依存関係が満たされていて取りかかれる作業に移って、作業を進めます。…T1がビジーなとき、T2を立ててオーケーです。」→ 実装の担当は試験の依頼（image の作り方、流す試験、合否の基準）を Q1 に SendMessage し、T1 には直接送らない。Q1 が `plan/agents/T1/requests.md` に積んで T1 に送る。T1 は台帳の未実行の依頼をまとめて 1 回の QEMU の起動で流す。T1 が忙しく依頼が溜まった時は Q1 が T2 を立てる。結果は Q1 が判定し、FAIL は直す Queue にして元の担当へ（担当は次の区切りで受ける）。

2026-10-04 夜 user「T1が空いていますね。いったん終了して、必要なときに起動しましょう。」→ T1 は台帳の未実行が無くなったら終了し、Q1 が依頼を持った時に新しい generation で起動する（待機させない）。

## 頻繁な統合

1. 各担当は独立worktree `/home/awe/zedBSD-worktrees/p<番号>`、branch `agent/p<番号>`（固定）を使う。worktreeはmainが作り、絶対pathを起動指示で渡す。既存の古い worktree（`.claude/worktrees`、旧 `codex/` branch）を消したり再利用したりしない。担当はmain checkoutや所有外ファイルを編集しない。build directory、QEMU port/socketは担当ごとに隔離し、共有build/toolchainを書かない。
2. Phase完了を待たず、意味がまとまり、検証範囲を説明でき、コミット可能になった時点で担当pathだけを `git commit -m WIP -- <path>...` する。未完了のPhaseをclearedに見せない。mainへ担当/Queue/attempt、base SHAと先頭/末尾commit SHA、対象path、行ったcheck/未実施check、現在のPhase状態、残りと次の安全な地点を送る。
3. mainは依頼の順序/重複を照合し、sourceと証拠をレビューする。mainの清潔なcheckoutで提出時点のSHAを固定し、`git merge --no-ff --no-commit <SHA>`で統合する。競合はmainが解決して必要な有限回帰を行い、成功後に`git commit -m WIP`とする。試験失敗時は統合を確定せず原因と再開条件を残す。統合commit/検証/保留を台帳とQueue/Phaseへ記録し、担当にACKを返す。commitしただけではPhase clearanceでも他WSの依存解除でもない。
4. mainが競合解決でbranchとの差を作ったら担当へ伝え、次の依存作業前にmainの結果を取り込ませる。新たなmerge requestは前回統合済みSHAを明記し、同じcommitを二重適用しない。pushはしない。

## ラップアップと障害回復

- **通常**: mainから依頼が届いたら、新しいQueueの開始を止める。現在の小さな作業を安全なコミット可能地点まで進め、担当pathのWIP commit/必要なcheck/証拠/未達/再開点と未着手の予約Queueを返して終了する。Queue基準未達ならmainがattemptをunclearedとして保存し、Queueを証拠付きで終了する。
- **urgent**: 新しい実装や長い検証を始めず、現在の差分をすぐ保存する。未追跡の担当fileは`git add -N`で差分へ含め、`git diff --binary HEAD -- <担当path>`をworktreeの`plan/wsNNN/temp/`へ保存する。base SHA、patch SHA-256、patch path、未追跡asset、実行中process、未実施check、再開手順をmainへ送って終了する。patchは未検証の救出物であり、clearanceや統合commitと扱わない。
- 強制終了/利用制限時はmainがworktree・branch・未commit差分・最後のmerge requestを調べ、重複適用を避けて回収する。中断したattemptの結果と再開条件はmainがQueue/Phase/historyへ残す。急停止を成功扱いしない。
- コンテキスト残量/実行時間が限界ならチェックポイントを作ってmainに知らせ、残りは同じ名前の担当の新しい世代へ引き継ぐ。

## 権限で止まったとき（2026-10-02 ユーザー）

ユーザー:「セキュリティや権限で実行できなかった場合は、私に明示的な承認を得て、サブエージェントをラップアップしてから再起動する運用にします。こうすることで、サブエージェントに権限が付与されます。」→ 担当は止まって Q1 に返す。Q1 はユーザーの明示の承認を得る → 担当を通常のラップアップ → 同じ名前の新しい generation を起動し、承認の引用とともに再開させる。Q1 は止められた操作を代わりに行わない。

## 記録の最小項目

[Registry](registry.md)は担当名/世代、WS、worktree/branch、current Queue、ordered next Queues、状態、最終checkpoint、wrap-up指示、最終merge ACKを持つ。Queue laneは承認元/範囲/Phase/attempt/依存と結果を持つ。成果のGit commit、統合、Queue/Phase clearance、WS completionは別イベント。shared cache/outboxはmainだけが書き、GitHub publicationの保留とローカル実行成果を混同しない。

2026-10-03 / toolchain の規則と試験の image: T1 の T1-027（CI の image、config/ci/config-amd64.mk）が T1 の worktree の build/ の中で target の clang の package（userland/packages/lang/clang）を build・stage した。共有の toolchain（build/llvm・llvm-source・llvm-build・NoctLang）は toolchain-lock.sh status で全て書き込み不可のまま、変更なしを Q1 が確認。AGENTS.md の「subagent は toolchain を変更・build・install しない（main の許可が要る）」に対し、Q1（main）の許可: 試験の担当（T1・T2）が自分の worktree の build/ の中で、image に入れる target の clang・libcxx の package を build するのは可（共有の toolchain の tree は lock のまま、source と規則は変えない）。I/O が重いので、同じ BUILD の directory を使い回して作り直しを避ける。

## 2026-10-06 の体制と試験待ちの印

ユーザー（原文）:「このqueueで実行してください。N=2でOKです。P1,P2,T1の構成です。変更点として、テストのサブエージェントのモデルを Sonnet 5.5 Midにしたいので、記録してください。進め方は、ベータ2までの範囲をすべて消化することをゴールにします。UATの指摘事項を優先しながら、ベータ2の未実装部分を次に優先します。UAT指摘事項以外のバグ修正は、ベータ2の実装より優先度を下げます。P1,P2は設計と実装を行い、テストをT1に回します。T1にテストを依頼したら、P1,P2は次の作業に移り、テスト結果を待ちません。テスト中のPhaseにはテスト結果待ちをマークしておきます。T1のテストが終わったら、Phaseのテスト待ちマークがクリアされ、P1,P2の次のスケジューリングで実行可能になります。Q1のメインエージェントが計画の管理、スケジューリング、結果のマージを行います。」

- 体制: P1・P2（phase-runner、high、設計と実装）、T1（test-runner、**Sonnet 5.5 の medium**、`.claude/agents/test-runner.md` の `model: claude-sonnet-5-5`・`effort: medium`）。N=2。Q1 は計画・スケジュール・merge。
- ゴール: ベータ2 までの範囲を全て消化する。順: (1) UAT の指摘（BUG-211〜237 とその設計の Phase）(2) ベータ1・ベータ2 の未実装 (3) UAT 以外の Bug。
- **試験待ちの印**: 担当が T1 に依頼した Phase は、phase.md の Status と queue.md の State に `test-wait（T1-NNN）` と書く。担当は結果を待たず次の Queue へ移る。T1 の結果は Q1 が判定し、印を外す: PASS → cleared、FAIL → 直しを同じ Phase の新しい attempt として次のスケジュールで（なるべく元の担当に）投入する。印のある Phase に依存する Queue は印が外れるまで始めない。
- 試験の依頼は担当から Q1 へ送り、Q1 が `plan/agents/T1/requests.md` に積んで T1 に渡す（2026-10-04 の集約の規則のまま）。T1 は source を直さないので、AAT の runner・試験の helper の直しは P1・P2 が行う。

## 削除は Q1 の pipeline、security の判定は回避しない（2026-10-06 ユーザー）

ユーザー:「rmはQ1で実行するパイプラインにしてください。セキュリティ回避はアカウントbanにつながるので自動では行わないで、私に対処を聞いてください。」（P1 が `plan/tools/files/host-run.sh` の中の `rm -rf` で Claude Code の安全の判定に 2 回止められた件への回答）
- 担当（P・T）は rm を走らせない。担当が使う script にも rm を入れない（例: host-run.sh の `--fresh` は新しい `run.*` の directory を作る形に変え、古い物は Q1 が `plan/tools/files/host-clean.sh WORKTREE` で消す）。消す物がある時は path と理由を Q1 に送る。guest の中（QEMU の中の `/tmp` など）の rm は host の削除ではないので対象外。
- security・権限の判定で止められたら、担当も Q1 も別の経路で回避しない。Q1 がユーザーに内容と対処の案を示して指示を待つ（2026-10-02 の「権限で止まったとき」の再起動の手順は、ユーザーがそれを選んだ時だけ）。
- 範囲（2026-10-06 ユーザー、クリック「make の規則の rm は可」）: make の規則が自分の worktree の build/ の中の自分の出力を消すのは対象外。担当が直に打つ rm と試験の script の中の host の rm は禁止のまま。script は `plan/tools/fresh-out.sh`（`fresh_out NAME`）か `build/tmp/` の mktemp を使い、Q1 が `plan/tools/q1-clean.sh WORKTREE` で古い物を消す。

## 削除はメインエージェントの仕事（2026-10-07 ユーザー）

- ユーザー（2026-10-07、T1 が host で `rm -f /dev/null` を誤って打ち、続く script が auto-mode で拒否された件の後）:「T1は再起動します。削除コマンドの実行は禁止として、メインエージェントにaskして実行してもらうパイプラインを作って対策します。削除自体はメインエージェントの仕事にします。」
- 全ての担当（P1〜P8・T1・B1）は削除の command（rm・rmdir・unlink・find -delete・shutil.rmtree・os.remove など、script の中も）を一切実行しない。消す必要がある物は SendMessage で Q1 に path と理由を送り（「削除の依頼」）、Q1 が中身を確かめて自分の pipeline で消し、結果を返す。
- 担当は削除を待たずに続けてよい（新しい出力先は fresh_out か build/tmp の新しい directory）。

## 試験の image は依頼の tree で作り直す（2026-10-08 Q1）

T1-314 の FAIL（apps.photos.browse）は、使い回しの AAT の image（build/aat-t1202）の photos が依頼の tree より古かったため（`--import` を知らない使い方の行）。q872 の P2 の調べ（ws157-p005）。→ T1 は image を依頼の tree（main の SHA）で build し直して作り、使い回す時は全ての package が依頼の tree で作り直されたことを確かめる。結果には image の tree の SHA を書く。app の新しい option を試す依頼では、guest で使い方の行（例 `/bin/photos --help`）を確かめてから流す。Q1 は FAIL を直す Queue にする前に、同じ試験の後の T1 の行（再試験の PASS）を確かめる。

## 担当の build は target を名指す（2026-10-08 Q1）

P1 が worktree で target 無しの make（既定の goal は disk-image）を流し、target の clang・libcxx・openssh・openssl の package の build を始めた（約 10 分で自分で止めた）。共有の toolchain は lock のまま（Q1 が `toolchain-lock.sh status` で確かめた）、worktree の中の途中の出力は Q1 が消した。→ 実装の担当（P）は個別の target（`$(BUILD)/vmunix`・`$(BUILD)/bin/<app>`・`$(BUILD)/dynamic/<lib>.so` など）を名指して make し、target 無しの make・disk-image を流さない（image を作るのは T1 か Q1）。
