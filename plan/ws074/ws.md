<!-- awesome-plan project=zedbsd record=ws074 -->

# WS074: zedBSD の Web ブラウザ（`userland/desktop/browser`）

<!-- awesome-plan-current:start -->
Status: incomplete（2026-10-13 Acid3 得点100/100・pixel差0達成。ユーザーが次に WPT Interop 2025 全件PASS 100%を目標として指定）
Primary Milestone: MG006
Related Milestones: MG002
Objectives: O1, O2
Parent: [Master](../master.md)
Owner: Q1（プロジェクト全体）。2026-10-30 ユーザー「Codexは解約しました。WS074はClaudeの管理下に移ります。」→ Codex のセッションは終わり、WS074 は Q1 が Queue を管理し Claude の担当に割り当てる（旧: Codex のセッションが単独で管理）。
担当の定義: 2026-10-30 ユーザー「WS074はphase-runner-lowで実行することにします。」→ WS074 の Phase は `.claude/agents/phase-runner-low.md`（Sonnet 5.5・high）で起動する（旧: B1 の browser-runner-b1、Sonnet 5.5・medium）。
自走（2026-10-30）: ユーザーから、P8 が Interop 2025 で Chromium が PASS する項目の 100% をゴールに自走する、という趣旨の指示を受けた。P8 が返却したら Q1 は同じ目標で起動し直す（q1727 から、attempt を重ねる）。
Queue: [ws074-q20261019-55](queue.md)（2026-10-18、Codex単独で開始。2026-10-30 Codex の解約で止まった。2026-10-30 ユーザー「WS328に関しては、WS074を止めてWS328を先に実装する、です。」で WS328 の後に再開）→ 2026-10-30 13:4x ユーザー「P8でWS074（libbrowserの改善）を実行してください。」で [q1708](../queue.md)（P8、phase-runner-low、p260）として再開。ユーザーが100%までの自走を指示。[契約](autonomy.md)。

Event ws074-p260-start-20261027: ユーザー「WPT Interop 2025で、chromeが成功している項目に限り、100%を目指しましょう。」により [p260](phase260/phase.md) を立てた。分母は同じ runner で Chromium が PASS する ID。WPT を全体の checkout に直し（sparse で harness 1,072/static 1,536 に欠けていた）、Chromium の adapter と比較の道具を足した。基準の測定は実行中。

Event ws074-cloud-setup-20261027: ユーザーの指示で、Claude Code on the web の container に host の build と Chromium（Chrome for Testing 153 の headless shell）との描画の比較を立てた。手順・結果・制限は「[クラウド環境での構築と Chromium との比較](#クラウド環境での構築と-chromium-との比較2026-10-27)」。ライブのサイトとの比較は network の方針で未達。

Latest verified（q54）: actual global Proxy/revocable公開partial clear。public42 native/genuine一致・private866/Chrome864既知huge2、direct installer/revoker実GC/graph回収。全eligible3530 current pair・1482/3805・held0/未attempt275、harness whole494維持/旧PASS loss0・original IDL setup2修復/新14PASS28FAIL（net+16PASS）。source6b6be56e0 main統合・master編集無し。Reflect/whole/p100/full target/100%未達。[証拠](phase258/evidence/20261019-q54/README.md)。共有投影pending。

Event ws074-q14-percentage-scope-reconciled-20261018: precode Chrome17条件でzero/cancelled %型の依存を確認。p251をi01、p250をi02へ改訂しglobal1491/static1561照合へ拡張、初期snapshot保存/60分deadline維持。

Event ws074-p250-q14-selected-20261018: [p250](phase250/phase.md)でcomputed fit boundを60分scopeへ選定。新metadataでnative geometryを変えず、既存whole PASSと独立computed基準を確認。callback全遷移は記録しknown animation未達を維持する。

Event ws074-p189-q13-cleared-20261018: [証拠](phase189/evidence/20261018-q13/README.md)。追加36全実行/34 FAIL・2 multiple completions、候補1491/未対応292、PASS増分0。native query/旧plain5対照同値でrunner部分cleared、whole/Native autofocus等未達。次はcomputed fit-content bound候補。

Event ws074-p189-q13-start-20261018: q12完了後、URL variant80中、既存HTTP/source条件を満たす36 IDをrunnerへ追加する45分scopeを選定。固定分母/未対応残りとq11等の未達を保持。

Event ws074-q12-finished-20261018: [証拠](phase247/evidence/20261018-q12/README.md)。p189 retention partial/p247 own zero glyph cleared。全harness1455 whole489/callback11534・全static1561 whole839維持、後退/欠損0。native56/56/独立14/14/DOM30/30、zero ink15→0・positive差0。公式whole増分0、補助0→2。次はplain variant36、marginはp249。全3,805/runner/target/p100未達。

Event ws074-q12-native-and-planning-20261018: p247 native56/56・DOM30/30・zero ink15→0、positive画素差0、全static1561の839 PASS維持/後退0。harness全件比較は継続中。残るmargin差をp249へ計画し、次のrunner候補はp189 plain variant36。q12 scopeは不変。

Event ws074-q12-retention-clear-font-start-20261018: [retention証拠](phase189/evidence/20261018-q12-i01/README.md)。actual1 ID raw/gzip一致、i01部分cleared。i02 zero fontを開始。p189 whole/q11未達は保持。

Event ws074-q12-selected-20261018: runner retentionをp189で先に直し、verified output後に独立p247 zero glyphを進める最大90分scope。全eligible harness/staticをq11 before/afterで比較し、q11 animation未達は保持。

Event ws074-p246-q11-uncleared-p248-added-20261018: [証拠](phase246/evidence/20261018-q11/README.md)。Grid whole248維持、74 callback改善/24後退でq11 uncleared。static227→228/後退0、独立22/22、DOM29/29、host/target warning0、Acid非増加。24はnative animation欠落、p248計画を追加。次は独立zero font/runner retention。

Event ws074-p247-planned-20261018: [p247](phase247/phase.md)にzero fontのglyph advance/inkを後続候補として追加。p185のstrut clearanceは維持。q11とは別Queueで選定してから実装する。

Event ws074-p246-computed-floor-design-correction-20261018: existing repeat/vertical consumerのfloorを保つため、computed_maximumのscalar copyでunfloored upperを保持する設計へ確定。cascade floor移動案は撤回しp221/p243/p244/p246に反映。

Event ws074-p246-q11-scope-reconciled-20261018: grid-autoがintrinsic/minmaxを早期拒否するため、自走契約でq11範囲を共通track-size parserへの統一へ改訂。初期snapshotを保存しp221/p243へ反映。timebox/非悪化criteriaは維持、repeat/line-name/fit sizing残りあり。

Event ws074-p246-q11-selected-20261018: [p246](phase246/phase.md)を追加し、intrinsic maximum metadata/computed fallbackとhorizontal single-span min-content contributionを有限Queueへ選定。p221/p243 interfaceと検証へ反映、general/row/vertical等whole残りは維持。

Event ws074-p245-q10-cleared-20261018: [証拠](phase245/evidence/20261018-q10/README.md)。no-box Phase cleared、固定174 whole107→109・callback989→993・後退0、独立19/19・unknown維持、DOM28/28、host/target warning0、Acid3差0/Acid2差873。次はintrinsic track keyword/sizing候補。WS全体/target/p100未達。

Event ws074-p245-q10-start-20261018: [p245](phase245/phase.md)のno-box auto minimumを有限Queueへ選定。primary/childの実layout proofとunknownを区別し、固定174 ID/独立対照で検証する。

Event ws074-q09-finished-20261018: [全502証拠](phase243/evidence/20261018-q09-i02/README.md)。whole247維持、callback6,410→6,419・後退0、q08の10後退回復。両partial cleared、p243/p244 whole残り保持。次はp245。

Event ws074-p244-q09-i01-clear-i02-start-20261018: [証拠](phase244/evidence/20261018-q09/README.md)。target10後退回復・対象callback9改善/後退0、DOM27/27、static227/529維持。verified metadata/eligibility output後、p243が全502を再検証する。general minimum/target等wholeは未達。

Event ws074-p244-q09-start-20261018: minimum ruleをparse/cascade/layoutへ保持し、guard eligibilityを修正する次の有限Queueを開始。p244 verified output後にp243が新attemptで全502を再検証する。

Event ws074-p244-q08-invalidated-minimum-redesign-20261018: [i02証拠](phase243/evidence/20261018-q08-i02/README.md)。full502の10 callback後退でi01初期clearanceをinvalidate、q08 uncleared。p244のminimum ruleをparse/cascade/layoutで保持する設計へ変更し、p221にinterface影響を記録。修正後p243が新attemptで再検証する。q07/q08 history/whole条件は維持。

Event ws074-p245-planning-20261018: [p245](phase245/phase.md)を後続候補に追加。display:none/祖先noneでboxが無い場合のauto minimum resolved 0pxを、geometry unknownと区別して実装する計画。q08のscopeは不変。

Event ws074-p244-q08-i01-clear-i02-start-20261018: [p244証拠](phase244/evidence/20261018-q08/README.md)。fixed-only host部分cleared、Chromium7/7・DOM27/27・静的Grid225→227/529・後退0。q07対象callback回復をp243の全502検証で確定する。p244 general minimum/autoとtarget等は残る。

Event ws074-p244-q08-start-20261018: q07の既知1件を[p244](phase244/phase.md)でfixed-only spanの有限修正とし、verified出力後にp243が新attemptで全502/全callbackを再検証する。q07 historyとwhole条件は維持。

Event ws074-p243-q07-uncleared-p244-added-20261018: [p243証拠](phase243/evidence/20261018-q07/README.md)。Grid whole245→247、433 callback改善/1後退。実track queryが固定行spanの既存solver不備を露出しq07 uncleared。新[p244](phase244/phase.md)で修正し、新p243 partial attemptで全502を再検証する。scope/whole/歴史を縮めない。

Event ws074-p243-q07-start-20261018: q06の後退0確認後、[p243](phase243/phase.md)のprimary horizontal used-track CSSOMを最大90分で開始。owned vectorへcopyし、一時treeへのpointerを外へ渡さない。検証runnerのretainも範囲内。

Event ws074-p190-q06-partial-clear-20261018: [証拠](phase190/evidence/20261018-q06/README.md)。静的1,561件836 PASS/725 FAIL、error/timeout0。全FAIL725はsession開始前にもFAILで後退0。未対応413/crash待機確認を残す。p243の前にrunner retainを修正。

Event ws074-p243-planning-20261018: [p243](phase243/phase.md)を後続候補として追加。Grid resolved track CSSOMを実solverの値からcopyし、childの一時arenaからpointerを返さない設計。q06のscopeとp221の既存acceptanceは不変。

Event ws074-p190-q06-start-20261018: 固定reftest1,974件の静的候補1,561件を最新固定binaryで測定開始。未対応413/crash48保持。次の描画修正候補を選ぶ。

Event ws074-p242-q05-partial-clear-20261018: [証拠](phase242/evidence/20261018-q05/README.md)。observer24項目Chromium一致・DOM25/25、補助18/31 callback PASS、公式whole増分0。observer部分cleared、whole未達。次は公式reftest全静的census。

Event ws074-p242-q05-start-20261018: p242のsource removal/observer batchを次の最大90分で開始。通常insertの契約は維持しprivate sequenceと明示rootを追加する。

Event ws074-p242-q04-partial-clear-20261018: [p242証拠](phase242/evidence/20261018-q04/README.md)。基本交換39項目・DOM24/24、official cleanup13件は後続検査へ進んだがwhole PASS増分0。observer等whole未達。次はbatch/source removal。共有投影pending。

Event ws074-p242-q04-start-20261018: q03診断後、[p242](phase242/phase.md)のbasic partialを90分で開始。ParentNode交換とvalidation、GC/native通知を実装、observer完全batchingは後続となる場合whole未達を残す。

Event ws074-p189-q03-partial-clear-20261018: [p189証拠](phase189/evidence/20261018-q03/README.md)。全適格1,455 IDで485 PASS、旧481 PASSの後退0。未対応328保持。p189 whole uncleared。次はp242 DOM cleanup。共有投影pending。

Event ws074-p242-planning-20261018: q03のanchor/@scope群でcleanupのreplaceChildren未定義を確認し、[p242](phase242/phase.md)を後続候補として追加。テストを改変せず一般DOM操作を補う案。q03の現行測定・p189/p173の受け入れは変更しない。

Event ws074-p189-latest-census-20261018: [p189](phase189/phase.md)の最大90分scopeで、固定testharness1,783件の既存適格1,455 IDを最新binaryで全再測定する。未対応328を分母に保持し、p241までの局所改善を全体100%と混同しない。

Event ws074-p241-host-partial-clear-20261018: [p241](phase241/phase.md)でstretch cross sizeをratioへ渡し、補助WPT149→152/257・後退0、40/40 geometry、Acid3差0/Acid2差873。host部分cleared、production link/targetはpending。257件は公式2025の分母外と確認したため、次はp189/p190の最新censusで全体の失敗群を測る。

Event ws074-autonomy-p241-20261018: ユーザー「では、当分の間、100%を目指して自走をお願いします。」を受け、固定Interop2025全3,805IDの100%を目指して有限Queueを継続する。[p241](phase241/phase.md)を追加し、p240の残るstretch cross sizeを処理する。Master/共有Queue/GitHubへの投影はpending。

Event ws074-p240-host-partial-clear-20261018: [p240](phase240/phase.md)でflex ratioの自動最小サイズを修正。[証拠](phase240/evidence/20261018/README.md)。同じWPT257 IDで142→149 PASS、後退0、独立29/29、DOM23/23、amd64 build warning0、Acid3差0/Acid2差873。部分cleared、Phase wholeはuncleared（target・stretch cross size・basis区別などが残る）。Chromiumとの28一致/1仕様差を記録。Master/共有Queue/GitHubの投影はpending。

Event ws074-p240-start-20261018: ユーザー「では、WS074を続けてください。」により [p240](phase240/phase.md) を追加。p234/p235 の残りの flex item の aspect-ratio を最大90分の [ws074-q20261018-01](queue.md) で調査・修正する。Codex 単独、Master/共有 Queue/GitHub の projection は pending。全件100%と WS acceptance は未達のまま。

Event ws074-p232-yahoo-feed-20261014: [p232](phase232/phase.md) B1。yahoo.co.jp のニュース feed（見出しと画像）が出る。原因は fetch の不足と、`Object.freeze` が配列の要素で EPERM を漏らして bundle が止まっていたこと、core-js が Promise を差し替えて microtask が動かなかったこと。fetch・Headers・Request・Response・Blob・FormData・TextEncoder・非同期 XHR（POST・header・status）・focus を実装。host の fetch 試験 38/38（blocking と --async）。

Event ws074-p231-google-script-20261014: [p231](phase231/phase.md) B1。google.com の script（BigInt・型付き配列・currentScript・defer・URL・dynamic link ほか）と Grid の高さ（item の max-height と grid item の auto margin）。live の ink 一致 23.47%→73.28%（全体 97.30%→99.36%）、script errors 2→0。固定 WPT 1,455 ID は 483 pass のまま後退 0。

Event ws074-p230-yahoo-20261014: [p230](phase230/phase.md) B1。yahoo.co.jp の top: gzip の body、for の initializer の中の関数の `in`、SVG の画像（stroke・clipPath・use・角丸 rect）、CSS `transform`（translate）、画像の Accept（png・jpeg のみ）、「…」の font fallback。header の一致 24%→53%（ink）。Acid2 差 1,865→873、固定 WPT 後退 0。

Event ws074-p228-uat-20261014: [p228](phase228/phase.md) B1。UAT の指摘: inline svg の描画（google.com のロゴと icon が出る）、既定の URL を google.com に、アドレスの箱の幅と repeat は compositor 側（根拠を記録、Q1 へ）。

Event ws074-p227-q1202-partial-clear-20261014: [p227](phase227/phase.md) q1202-i01 (B1) で flex の align-content・wrap-reverse・inline-flex・blockify・overflow 2値・sideways・safe。同条件 1,455 ID で whole PASS 458→481（後退 0）、静的 reftest 682→815（PASS→FAIL 4 件を記録）。

Event ws074-p227-q1201-partial-clear-20261014: [p227](phase227/phase.md) q1201-i01 (B1) で flex absolute 子の static position と align キーワードの構文。同条件 1,455 ID で whole PASS 427→458、後退 0。

Event ws074-p226-q1200-partial-clear-20261014: [p226](phase226/phase.md) q1200-i01 (B1) で auto-fill/auto-fit・縦書き in-flow 子・置換要素 stretch・computed の align 名の誤りを修正。同条件で whole PASS 379→427、後退 0（21 件は enum のずれによる computed 試験の回復）。

Event ws074-p226-q1199-partial-clear-20261014: [p226](phase226/phase.md) q1199-i01 (B1) で水平 Grid の content distribution・auto track stretch・absolute 子の grid line containing block を実装。[証拠](phase226/evidence/20261014-q1199/run.md)。1,455 ID で whole PASS 359→379、後退 0、Acid3 差 0/Acid2 差 1865。q1198-i01 は B1 が cleared と判定（[README](phase217/evidence/20261014-q1198/README.md)）。

Event ws074-p217-q1197-partial-clear-20261014: [p217](phase217/phase.md) q1197-i01でframed水平LTR/RTL Gridのall-auto縦lr absolute先頭子を修正。[証拠](phase217/evidence/20261014-q1197/README.md)。独立2矩形Chromium一致、固定012水平2 callbacks先頭子全PASS、2番目子でfile whole FAIL継続。旧WPT/host/Acid3差0維持。次は2番目縦rl子、p217/p189/p173 whole未達、共有投影/GitHub publication保留。

Event ws074-p217-q1196-partial-clear-20261014: [p217](phase217/phase.md) q1196-i01で固定012の親/子frame・all-auto insetを独立4矩形とrawで分類。[証拠](phase217/evidence/20261014-q1196/README.md)。水平親は縦inline経路と静的X、縦親はstatic Y/Xが不一致。次は水平親の先頭子、p217/p189/p173 whole未達、共有投影/GitHub publication保留。

Event ws074-p217-q1195-partial-clear-20261014: [p217](phase217/phase.md) q1195-i01で固定水平LTR/RTL Gridのmargin/border/padding複合縦absolute3配置に縦inline経路を適用。[証拠](phase217/evidence/20261014-q1195/README.md)。独立6矩形Chromium一致、固定011 whole PASS/4 callback PASS。旧WPT/host/Acid3差0維持。次は012親frame/auto inset、p217/p189/p173 whole未達、共有投影/GitHub publication保留。

Event ws074-p217-q1194-partial-clear-20261014: [p217](phase217/phase.md) q1194-i01で固定水平LTR/RTL Gridのpadding-only縦absolute3配置に縦inline経路を適用。[証拠](phase217/evidence/20261014-q1194/README.md)。独立6矩形Chromium一致、固定010 whole PASS/4 callback PASS。旧WPT/host/Acid3差0維持。次は011複合frame、p217/p189/p173 whole未達、共有投影/GitHub publication保留。

Event ws074-p217-q1193-partial-clear-20261014: [p217](phase217/phase.md) q1193-i01で固定水平LTR/RTL Gridのborder-only縦RTL absolute4番目子に縦inline経路を適用。[証拠](phase217/evidence/20261014-q1193/README.md)。独立2矩形Chromium一致、固定009 whole PASS/4 callback PASS。旧WPT/host/Acid3差0維持。次は010 padding子、p217/p189/p173 whole未達、共有投影/GitHub publication保留。

Event ws074-p217-q1192-partial-clear-20261014: [p217](phase217/phase.md) q1192-i01で固定水平LTR/RTL Gridのborder-only縦rl absolute2番目子に縦inline経路を適用。[証拠](phase217/evidence/20261014-q1192/README.md)。独立2矩形Chromium一致、固定009水平2 callbacks先頭2子全PASS、4番目子でfile whole FAIL継続。旧WPT/host/Acid3差0維持。次は4番目縦RTL子、p217/p189/p173 whole未達、共有投影/GitHub publication保留。

Event ws074-p217-q1191-partial-clear-20261014: [p217](phase217/phase.md) q1191-i01で固定水平LTR/RTL Gridのborder-only縦lr absolute先頭子に縦inline経路を適用。[証拠](phase217/evidence/20261014-q1191/README.md)。独立2矩形Chromium一致、固定009水平2 callbacks先頭子全PASS、2番目子でfile whole FAIL継続。旧WPT/host/Acid3差0維持。次は2番目縦rl子、p217/p189/p173 whole未達、共有投影/GitHub publication保留。

Event ws074-p217-q1190-partial-clear-20261014: [p217](phase217/phase.md) q1190-i01で固定non-static009–012 rawと原本を分類。[証拠](phase217/evidence/20261014-q1190/README.md)。009–011は水平Grid縦absolute子のframeによる縦inline経路拒否、012は親frame/auto inset static位置差も追加。次は009 borderのみ独立最小比較、p217/p189/p173 whole未達、共有投影/GitHub publication保留。

Event ws074-p189-q1189-partial-clear-20261014: [p189](phase189/phase.md) q1189-i01で固定Grid502同条件再測定。[証拠](phase189/evidence/20261014-q1189/README.md)。189→192 whole PASS、313→310 FAIL、16 callback改善、後退0、未出力0。次はnon-static009–012など残FAIL切り分け、p189/p173全3,805件100%未達、共有投影/GitHub publication保留。

Event ws074-p217-q1188-partial-clear-20261014: [p217](phase217/phase.md) q1188-i01で固定縦lr Grid直接横RTL absolute子row2/col2 left/top insetにstatic areaを適用。[証拠](phase217/evidence/20261014-q1188/README.md)。独立矩形Chromium一致、固定non-static007 whole PASS/4 callback PASS。旧2 WPT/host/Acid3差0維持。次はGrid subset再測定、p217/p189/p173 whole未達、共有投影/GitHub publication保留。

Event ws074-p217-q1187-partial-clear-20261014: [p217](phase217/phase.md) q1187-i01で固定縦rl Grid直接横RTL absolute子row2/col1 top insetにstatic areaを適用。[証拠](phase217/evidence/20261014-q1187/README.md)。独立矩形Chromium一致、固定non-static007縦rl callback whole PASS、水平2 callback PASS、縦lrは4番目子でfile whole FAIL継続。旧WPT/host/Acid3差0維持。次は縦lr4番目子、p217/p189/p173 whole未達、共有投影/GitHub publication保留。

Event ws074-p217-q1186-partial-clear-20261014: [p217](phase217/phase.md) q1186-i01で固定縦lr/rl Grid直接横LTR absolute子へrow1/col2 static areaとjustify-center Yを適用。[証拠](phase217/evidence/20261014-q1186/README.md)。独立2矩形Chromium一致、固定non-static007縦2 callback先頭2子全PASS、lrは4番目子・rlは3番目子でfile whole FAIL継続。旧WPT/host/Acid3差0維持。次は縦rl Gridの3番目RTL横子、p217/p189/p173 whole未達、共有投影/GitHub publication保留。
Resume point: q54 partial finished。次はfixed-corpus residualからCSS構文/数値serialize/CSSOM・URLPattern等の既存owner/依存/一次仕様を確認してfinite scopeを選定。IDL missing real APIsも保持、Reflectを根拠無しに次fixと仮定しない。全3530 current pair・未attempt275/whole/p100/full target/100%保持。
<!-- awesome-plan-current:end -->

Event ws074-p217-q1185-partial-clear-20261014: [p217](phase217/phase.md) q1185-i01で固定縦lr/rl Grid直接横LTR absolute子へrow1/col1 static areaとjustify-right Yを適用。[証拠](phase217/evidence/20261014-q1185/README.md)。独立2矩形Chromium一致、固定non-static007縦2 callback先頭子全PASS、2番目子でfile whole FAIL継続。旧WPT/host/Acid3差0維持。次は縦Grid2番目横子、p217/p189/p173 whole未達、共有投影/GitHub publication保留。

Event ws074-p220-q1184-partial-clear-20261014: [p220](phase220/phase.md) q1184-i01で水平RTL固定Grid直接vertical-rl/RTL absolute子へrow2 trackのinline roomとstatic areaを適用。[証拠](phase220/evidence/20261014-q1184/README.md)。独立矩形Chromium一致、固定non-static007水平LTR/RTL両callback PASS、縦2 callbackでfile whole FAIL継続。旧WPT/host/Acid3差0維持。次は縦Grid横子static area、p220/p217/p189/p173 whole未達、共有投影/GitHub publication保留。

Event ws074-p220-q1183-partial-clear-20261014: [p220](phase220/phase.md) q1183-i01で水平RTL固定Grid直接縦左/縦右RTL absolute子へrow1 trackのinline roomとstatic area/Y alignmentを適用。[証拠](phase220/evidence/20261014-q1183/README.md)。独立2子矩形Chromium一致、固定non-static007水平RTL callback先頭2子全PASS、4番目縦RTL子でwhole FAIL継続。旧WPT/host/Acid3差0維持。次は同第4子と縦Grid横子、p220/p217/p189/p173 whole未達、共有投影/GitHub publication保留。

Event ws074-p220-q1182-partial-clear-20261014: [p220](phase220/phase.md) q1182-i01で水平固定Grid直接vertical-lr/RTL absolute子へrow2 trackの縦inline roomを適用。[証拠](phase220/evidence/20261014-q1182/README.md)。最小矩形Chromium一致、固定non-static007水平LTR callbackの4子全PASS、他3 callbackでwhole FAIL継続。旧WPT/host/Acid3差0維持。次は水平RTL親と縦Grid横子、p220/p217/p189/p173 whole未達、共有投影/GitHub publication保留。

Event ws074-p220-q1181-partial-clear-20261014: [p220](phase220/phase.md) q1181-i01で水平固定Grid直接vertical-rl absolute子へrow-track inline roomとY alignmentを適用。[証拠](phase220/evidence/20261014-q1181/README.md)。最小矩形Chromium一致、固定non-static007水平LTR2番目子はwidth30/height70/offset通過、4番目vertical-lr/RTL子でwhole FAIL継続。旧WPT/host/Acid3差0維持。次はRTL子、p220/p217/p189/p173 whole未達、共有投影/GitHub publication保留。

Event ws074-q1176-q1180-time-correction-20261014: 2026-10-14 13:32 JSTにq1176–q1180の記録時刻をGit commit時刻と照合し、先行していた分表示を訂正。各[Queue履歴](../history/queue-q1180.md)に根拠を追記。承認範囲・検証結果・コードに変更なし。

Event ws074-p220-q1180-partial-clear-20261014: [p220](phase220/phase.md) q1180-i01で水平固定Grid直接vertical-lr absolute子へrow-track inline roomを適用。[証拠](phase220/evidence/20261014-q1180/README.md)。最小矩形Chromium一致、固定non-static007水平LTR先頭子はwidth10/height60/offset通過、2番目子width差でwhole FAIL継続。旧WPT/host/Acid3差0維持。次はvertical-rl子、p220/p217/p189/p173 whole未達、共有投影/GitHub publication保留。

Event ws074-p217-q1179-partial-clear-20261014: [p217](phase217/phase.md) q1179-i01で固定11 FAILのorthogonal positioned Gridを分類。[証拠](phase217/evidence/20261014-q1179/README.md)。全11件水平親・縦子の初回width+50、縦親・横子のstatic offset差を確認。次は水平固定Grid直下の縦absolute子のinline room限定修正。読取のみ、p217/p189/p173 whole未達、共有投影/GitHub publication保留。

Event ws074-p217-q1178-partial-clear-20261014: [p217](phase217/phase.md) q1178-i01で親frameあり縦Grid同writing-mode absolute子のall-auto static areaを修正。[証拠](phase217/evidence/20261014-q1178/README.md)。固定margin-border-padding005 whole FAIL→PASS、最小lr/rl Chromium一致、旧004/non-static006/host/Acid3差0維持。次は同系列orthogonal子/auto line、p217/p189/p173 whole未達、共有投影/GitHub publication保留。

Event ws074-p189-q1177-partial-clear-20261014: [p189](phase189/phase.md) q1177-i01で固定Grid502全件同条件再測定。[証拠](phase189/evidence/20261014-q1177/README.md)。189 PASS/313 FAIL、q1172比6 whole・27 callback改善、後退0。同一ID/hash/checkpoint/raw確認。残る上位系列はmargin/border/paddingとstretch各12件、p189/p173全3,805件100%未達。共有投影/GitHub publication保留。

Event ws074-p217-q1176-partial-clear-20261014: [p217](phase217/phase.md) q1176-i01でborder/padding親の縦Grid明示inset areaを修正。[証拠](phase217/evidence/20261014-q1176/README.md)。固定006 whole FAIL→PASS、001–006の6件24 callback全PASS。最小lr/rl Chromium一致、host/Acid3差0維持。次は固定Grid502再測定。p217/p189/p173 whole未達、共有投影/GitHub publication保留。

Event ws074-p217-q1175-partial-clear-20261014: [p217](phase217/phase.md) q1175-i01で縦Grid明示insetの有効領域基準を修正。[証拠](phase217/evidence/20261014-q1175/README.md)。固定001–004は4/4 whole・16/16 callback PASS、隣接005もPASS。最小lr/rlはChromium一致、旧all-auto代表/host/Acid3差0維持。親frameあり006は縦callback FAIL、次Queue候補。p217/p189/p173 whole未達、共有投影/GitHub publication保留。

Event ws074-p217-q1174-uncleared-20261014: [p217](phase217/phase.md) q1174-i01で水平Grid明示insetのGrid領域 containing block と未指定軸staticを実装。[証拠](phase217/evidence/20261014-q1174/README.md)。独立4矩形Chromium一致、固定001–006の水平12 callbacks PASS、縦12 FAILでwhole0/6。q1174 uncleared、水平修正保持、次は縦Grid明示inset。host/Acid3差0維持、p217/p189/p173 whole未達、共有投影/GitHub publication保留。

Event ws074-p217-q1173-partial-clear-20261014: [p217](phase217/phase.md) q1173-i01固定Grid319 FAIL分類をcleared。[証拠](phase217/evidence/20261014-q1173/README.md)。alignment135が最多、上位3系列各12件の原因経路を確認。次は水平positioned Grid片軸明示insetと他軸static。p217/p189/p173 whole未達、共有投影/GitHub publication保留。

Event ws074-p189-q1172-partial-clear-20261014: [p189](phase189/phase.md) q1172-i01固定Grid502再測定は183 PASS/319 FAIL、q1160比25 whole・50 subtest改善、後退0。[証拠](phase189/evidence/20261014-q1172/README.md)。positioned列/行軸22件に加えmargin/border/padding3件もPASS。p189/p173全3,805件100%未達、共有投影/GitHub publication保留。

Event ws074-p217-q1171-partial-clear-20261014: [p217](phase217/phase.md) q1171-i01でCSS align-self:self-start/endを保持し、逆writing-mode縦Grid absolute子の物理端へ写す。[証拠](phase217/evidence/20261014-q1171/README.md)。column010/011 whole FAIL→PASS、column/row001–011の22件88 callbacks PASS。computed値・host・warning0 build・Acid3差0維持。p217/p189/p173 whole未達、共有投影/GitHub publication保留。

Event ws074-p217-q1170-partial-clear-20261014: [p217](phase217/phase.md) q1170-i01でRTL子のjustify-self:self-start/endを物理縦端へ反転。[証拠](phase217/evidence/20261014-q1170/README.md)。row010/011 whole FAIL→PASS、row001–011の11件44 callbacks PASS。host・warning0 build・Acid3差0維持。column010/011 static areaとRTL字形視覚、p217/p189/p173 whole未達。共有投影/GitHub publication保留。

Event ws074-p220-q1169-partial-clear-20261014: [p220](phase220/phase.md) q1169-i01で同writing-mode固定2×2縦Grid absolute子のRTLを限定縦inline寸法経路へ通す。[証拠](phase220/evidence/20261014-q1169/README.md)。row010/011幅60→10、残るoffsetTop差へ進む。旧6件/host/Acid3差0維持。字形視覚・static areaは別、p220/p217/p189/p173 whole未達、共有投影/GitHub publication保留。

Event ws074-p220-q1168-uncleared-20261014: [p220](phase220/phase.md) q1168-i01はcolumn010/011逆writing-mode子の幅60→10を達成したが、row010/011は逆direction子と判明しcriteria未達のためattempt uncleared。[証拠](phase220/evidence/20261014-q1168/README.md)。改善コードは保持、旧6件/host/Acid3差0維持。次の有限QueueはRTL縦文字寸法、その後static area。p220/p217/p189/p173 whole未達、共有投影/GitHub publication保留。

Event ws074-p217-q1167-partial-clear-20261014: [p217](phase217/phase.md) q1167-i01で同writing-mode固定2×2縦Grid absolute子のjustify-self:self-start/endをstatic areaへ写す。[証拠](phase217/evidence/20261014-q1167/README.md)。row-axis008/009の2件whole FAIL→PASS、column/row-axis001–009の18件72 callbacks PASS。host・warning0 build・Acid3差0/Acid2差1865維持。p217/p189/p173 whole未達、共有投影/GitHub publication保留。

Event ws074-p217-q1166-partial-clear-20261014: [p217](phase217/phase.md) q1166-i01は固定positioned006–011の12件を診断。[証拠](phase217/evidence/20261014-q1166/README.md)。6 whole PASS/6 FAIL、row008/009のself-start/end static未対応を分離。訂正（2026-10-14 12:45 JST）: column010/011は逆writing-mode、row010/011は逆directionの子。p217/p189/p173 whole未達。読取のみ、共有投影/GitHub publication保留。

Event ws074-p217-q1165-partial-clear-20261014: [p217](phase217/phase.md) q1165-i01で固定2×2縦Grid absolute子のleft/rightと無効align-selfのnormal fallbackをstatic areaへ写す。[証拠](phase217/evidence/20261014-q1165/README.md)。固定column/row-axis003/004の4件whole FAIL→PASS、旧6件含む10件40 callbacks PASS。host・warning0 build・Acid3差0/Acid2差1865維持。p217/p189/p173 whole未達、共有投影/GitHub publication保留。

Event ws074-p217-q1164-partial-clear-20261014: [p217](phase217/phase.md) q1164-i01で固定2×2縦Grid直接absolute子のend/center self-alignmentを物理static areaへ写す。[証拠](phase217/evidence/20261014-q1164/README.md)。固定column/row-axis002/005の4件whole FAIL→PASS、旧001を含む6件24 callbacks PASS。host・warning0 build・Acid3差0/Acid2差1865維持。p217/p189/p173 whole未達、共有投影/GitHub publication保留。

Event ws074-p217-q1163-partial-clear-20261014: [p217](phase217/phase.md) q1163-i01で固定2×2縦Grid直接absolute子の明示span1/start areaを物理y/xへ写す。[証拠](phase217/evidence/20261014-q1163/README.md)。独立lr/rl全8矩形Chromium 153一致、固定column-axis001/row-axis001は2/2 whole FAIL→PASS。旧縦Grid32+縦横2、host・warning0 build・Acid3差0/Acid2差1865維持。p217/p189/p173 whole未達、次は他alignment/auto lines。共有投影/GitHub publication保留。

Event ws074-p220-q1162-partial-clear-20261014: [p220](phase220/phase.md) q1162-i01で固定px縦Grid直接absolute ASCII子を縦inline/shrink-to-fit/paintへ通し、最小lr/rlは10×36px・hit/CPU-GPU一致。[証拠](phase220/evidence/20261014-q1162/README.md)。positioned代表3件の縦初回幅/高さ差は解消したがstatic area位置差でwhole FAIL。旧縦Grid32+縦横2、host・warning0 build・Acid3差0/Acid2差1865維持。p220/p217/p189/p173 whole未達、次はp217縦Grid static area。共有投影/GitHub publication保留。

Event ws074-p217-q1161-partial-clear-20261014: [p217](phase217/phase.md) q1161-i01で固定positioned列軸/行軸34件を再分類。[証拠](phase217/evidence/20261014-q1161/README.md)。全whole FAIL、33件の縦初回width期待10/実60はabsolute rootの縦inline拒否、縦Grid static areaも独立未対応。行軸017の水平RTL差は別問題。次はbounded absolute縦文字geometry、p217/p189/p173 whole未達、共有投影/GitHub publication保留。

Event ws074-p189-q1160-partial-clear-20261014: [p189](phase189/phase.md) q1160-i01で固定Grid502全件同条件再測定。142→158 whole PASS、360→344 FAIL、縦右Grid stretch001–016の16 subtest改善、旧PASS/subtest後退0。502 ID/hash/checkpoint/gzip raw一致。[証拠](phase189/evidence/20261014-q1160/README.md)。p189/p173全件100%未達。共有投影/GitHub publication保留。

Event ws074-p225-q1159-partial-clear-20261014: [p225](phase225/phase.md) q1159-i01で2×2縦右implicit auto Gridの物理行右→左と子縦文字の確定列高・frame寄与を実装。固定vertical-rl stretch009–016が8/8 whole FAIL→PASS、独立border/padding各4矩形はChromium 153一致。[証拠](phase225/evidence/20261014-q1159/README.md)。旧rl8/lr16/縦横2、host・warning0 build・Acid3差0/Acid2差1865維持。p225/p189/p173 whole未達、次は固定Grid502再測定。共有投影/GitHub publication保留。

Event ws074-p225-q1158-partial-clear-20261014: [p225](phase225/phase.md) q1158-i01で明示固定px縦右Gridの行を物理右→左へ配置し、子縦右文字の固定列roomを解決。固定vertical-rl stretch001–008が8/8 whole FAIL→PASS、独立2×2空/文字ページはChromium 153と全矩形一致。[証拠](phase225/evidence/20261014-q1158/README.md)。旧縦左16+縦横2、host・warning0 build・Acid3差0/Acid2差1865維持。p225/p189/p173 whole未達、次は縦右暗黙auto track。共有投影/GitHub publication保留。

Event ws074-p220-q1157-partial-clear-20261014: [p220](phase220/phase.md) q1157-i01で非Grid・確定px content boxのASCII vertical-rl/LTRを右始まり縦inline/paintへ通す。[証拠](phase220/evidence/20261014-q1157/README.md)。単一/2行DOM矩形・hit/dump、F字形8px移動画素完全一致、CPU/GPU同一、旧WPT18 PASS、host・warning0 build・Acid3差0/Acid2差1865維持。縦右Grid対照はFAIL不変でp225物理行反転が次。p220/p225/p189/p173 whole未達、共有投影/GitHub publication保留。

Event ws074-p220-q1156-partial-clear-20261014: [p220](phase220/phase.md) q1156-i01は非Grid vertical-rl単一/2行文字を独立比較。Chromiumは右始まり縦進行、browserは左始まり横進行でCPU/GPUは同じ横描画。[証拠](phase220/evidence/20261014-q1156/README.md)。次はp220 bounded縦右文字geometry/paint、その後p225 Grid物理行反転。p220/p225/p189/p173 whole未達、共有投影/GitHub publication保留。

Event ws074-p189-q1155-partial-clear-20261014: [p189](phase189/phase.md) q1155-i01で固定Grid残360 FAILを全件分類。alignment176が最大、vertical-rl stretch16件全FAILの初回width差を確認。[証拠](phase189/evidence/20261014-q1155/README.md)。Gridとinlineの両方が縦右向け限定経路を持たないため、次は非Grid縦文字geometryをp220で診断。p189/p173全件100%未達、共有投影/GitHub publication保留。

Event ws074-p189-q1154-partial-clear-20261014: [p189](phase189/phase.md) q1154-i01で固定Grid502全件同条件再測定。136→142 whole PASS、366→360 FAIL、縦Grid stretch010/011/012/014/015/016の6 subtest改善、旧PASS/subtest後退0。502 ID/hash/checkpoint/gzip raw一致。[証拠](phase189/evidence/20261014-q1154/README.md)。p189/p173全件100%未達。共有投影/GitHub publication保留。

Event ws074-p225-q1153-partial-clear-20261014: [p225](phase225/phase.md) q1153-i01で子content-boxの非負px border/paddingを縦Grid auto track寄与と内容高へ反映。固定011/012/015/016が4/4 whole FAIL→PASS。独立border/paddingページはChromium 153と各4矩形一致、旧WPT14件・host CSS51/JS14/Grid48・warning0 build・Acid3差0/Acid2差1865維持。[証拠](phase225/evidence/20261014-q1153/README.md)。p225/p189/p173 whole未達、次はGrid502全再測定。共有投影/GitHub publication保留。

Event ws074-p225-q1152-partial-clear-20261014: [p225](phase225/phase.md) q1152-i01の非負px marginを縦auto track内容寄与・子の利用可能高・配置に反映し、固定010/014が2/2 whole FAIL→PASS。独立2ページはChromium 153と全box一致、旧WPT12件・host CSS51/JS14/Grid48・warning0 build・Acid3差0/Acid2差1865維持。[証拠](phase225/evidence/20261014-q1152/README.md)。p225 wholeはborder/padding等が残りuncleared、p173全3,805件100%未達。共有投影/GitHub publication保留。

Event ws074-p189-q1151-partial-clear-20261014: [p189](phase189/phase.md) q1151-i01全固定Grid502再測定は134→136 whole PASS、368→366 FAIL、vertical-lr stretch009/013の2 subtest改善、旧PASS・subtest後退0。[証拠](phase189/evidence/20261014-q1151/README.md)。q1149残件分類をmargin010/014・border011/015・padding012/016へ訂正。p189 wholeとp173全3,805件100%は未達、共有投影/GitHub publication保留。

Event ws074-p225-q1150-partial-clear-20261014: [p225](phase225/phase.md) q1150-i01は2×2 implicit auto tracksの物理寸法寄与と縦論理軸配置を修正し、固定009/013が2/2 whole FAIL→PASS。250×250と300×240不等内容の最小ページはChromium 153と全box一致。旧WPT10件、host CSS51/JS14/Grid48、warning0 build、Acid3差0/Acid2差1865維持。[証拠](phase225/evidence/20261014-q1150/README.md)。p225 wholeはmargin variant等が残りuncleared、p173全件100%未達。共有投影/GitHub publication保留。

Event ws074-p225-q1149-partial-clear-20261014: [p225](phase225/phase.md) q1149-i01診断をcleared。固定009–016全8件FAIL、初回幅は縦子期待20/実125・115、横子期待80/実125・115。最小2×2をChromium 153と照合し、旧Gridの論理列物理x配置、強制stretch、auto-track縦文字room不足を共通原因と確認。[証拠](phase225/evidence/20261014-q1149/README.md)。次はmargin無し009/013を有限実装、margin有り6件は後続。p225/p173 whole未達、共有投影/GitHub publication保留。

Event ws074-p220-q1148-partial-clear-20261014: [p220](phase220/phase.md) q1148-i01の限定縦sideways paintをCPU/GPUで実装し、F回転画素完全一致・clip込みGPU差0、固定旧WPT10/10 PASS、warning0 build・host CSS51/JS14/Grid48・Acid3差0/Acid2差1865維持。[証拠](phase220/evidence/20261014-q1148/README.md)。p220 wholeはupright/CJK・別writing-mode・wrap/inline/装飾が残りin-progress、p173全3,805件100%未達。共有投影/GitHub publication保留。

Event ws074-p173-interop-resume-20261013: ユーザーが「WPTに進みます。2025を100%」と指示。既存の p173 Interop 2025 公式 focus area 全件 PASS 100% を次の優先対象にした。p173 と p100 の依存関係を、p188 の main 統合済み Acid3 検証出力に限定して整理。p100 whole の Acid2 exact/target gate は未達のまま。p173 の [候補選定と q1012 範囲](phase173/selection-snapshot.md)を記録、ユーザーが 23:49 JST 承認して開始。p101・p174〜p176 の受け入れは維持。Master の focused goal / WS priority 投影は、このセッションの編集禁止により Q1 へ pending。GitHub publication保留。

Event ws074-p173-q1012-outcome-and-split-20261013: q1012-i01 は [固定manifest・runner inventory・sample証拠](phase173/evidence/20261013-q1012/README.md)を完成し、partial scope cleared。p173 whole は全件baseline未取得で uncleared。3,805 ID中 testharness1,783、crashtest48、scripted reftest/reference407、variant6がrunner未対応。静的候補1,561中の選定sample35件は5 PASS/30 FAIL。結果取得を[p189](phase189/phase.md)、残るreftest/crashtestを[p190](phase190/phase.md)に分割。両者は p173 の固定manifest出力を前提に独立し、p173 whole は両出力と全件baselineを待つ。p100/p101/p174〜p176 の条件は維持。共有 Master/Queue/GitHub projection は別セッション所有のため pending、`master.md` は変更しない。

Event ws074-p189-q1013-outcome-20261014: [p189](phase189/phase.md) の q1013-i01 は WPT callback を単一公式選定ページで機械可読化し partial cleared。12 subtestを4 PASS/8 FAIL、completion OKと正しく記録。[証拠](phase189/evidence/20261014-q1013/README.md)。p189 whole1,783件とp173全件baselineは未達のまま。次の小さい runner Queue が必要。ブラウザ本体・Masterは未変更、GitHub publication保留。

Event ws074-p189-q1014-outcome-20261014: [p189](phase189/phase.md) の q1014-i01 は [19領域から最大1件の選定と実測](phase189/evidence/20261014-q1014/README.md)を完了、partial cleared。15 IDの結果は0 PASS/12 FAIL/1 harness ERROR/1 completion無し/1無出力。4領域は選定条件上未実行。p189 whole・p173全件baselineと100%は未達。browser/WPT/Master無変更、GitHub publication保留。

Event ws074-p189-q1015-outcome-20261014: ユーザーは Interop 2025全件100%までの後続有限Queueを事前承認。単独で最大90分ずつscopeを記録して続ける。[p189](phase189/phase.md) q1015-i01 はtextdecoration全16 IDを測定し4 PASS/12 FAIL、全completion取得。`CSS` global欠落とCSSOM不足が主な候補。[証拠](phase189/evidence/20261014-q1015/README.md)。p189 wholeと全件100%未達、GitHub publication保留。`master.md` は変更しない。

Event ws074-p191-design-and-q1016-outcome-20261014: q1015で測ったtextdecoration全16件のbrowser側互換を扱う[p191](phase191/phase.md)を追加。p189 runner責務・p173分母は不変。q1016-i01で `CSS.supports` を実装しpartial cleared、全16件は4 PASS/12 FAILと数不変だが `CSS is not defined` を解消。[証拠](phase191/evidence/20261014-q1016/README.md)。Acid3差0維持。p191 whole未達、GitHub publication保留。

Event ws074-p191-q1017-outcome-20261014: [p191](phase191/phase.md) q1017-i01はtext-decoration-lineの値解析とcomputed値のcanonical化を行いpartial cleared。対象WPT18/18 subtest、固定全16件は5 PASS/11 FAIL、subtest79 PASS/100 FAIL。[証拠](phase191/evidence/20261014-q1017/README.md)。Acid3画素差0、Acid2差1865維持。p191 whole/p173全件100%未達、GitHub publication保留。

Event ws074-p191-q1018-outcome-20261014: [p191](phase191/phase.md) q1018-i01はinline CSSOMのtext-decoration-line複合値をparser結果からcanonical化しpartial cleared。対象WPT67/67 subtest、固定全16件は6 PASS/10 FAIL、subtest128 PASS/51 FAIL。[証拠](phase191/evidence/20261014-q1018/README.md)。Acid3画素差0、Acid2差1865維持。p191 whole/p173全件100%未達、GitHub publication保留。

Event ws074-p191-q1019-outcome-20261014: [p191](phase191/phase.md) q1019-i01はstyle/skip-inkの値・初期値・継承・computed CSSOMを追加しpartial cleared。対象4 WPT16/16 subtest、固定全16件は10 PASS/6 FAIL、subtest144 PASS/35 FAIL。[証拠](phase191/evidence/20261014-q1019/README.md)。Acid3画素差0、Acid2差1865維持。p191 whole/p173全件100%未達、GitHub publication保留。

Event ws074-p191-q1020-outcome-20261014: [p191](phase191/phase.md) q1020-i01はtext-decoration-colorの値と遅延currentcolorを追加しpartial cleared。対象2 WPT6/6 subtest、固定全16件は12 PASS/4 FAIL、subtest150 PASS/29 FAIL。[証拠](phase191/evidence/20261014-q1020/README.md)。Acid3画素差0、Acid2差1865維持。p191 whole/p173全件100%未達、GitHub publication保留。

Event ws074-p191-q1021-outcome-20261014: [p191](phase191/phase.md) q1021-i01はtext-decoration shorthand全文法検証とlonghand展開を行いpartial cleared。invalid WPT3/3 subtest、固定全16件は13 PASS/3 FAIL、subtest153 PASS/26 FAIL。[証拠](phase191/evidence/20261014-q1021/README.md)。Acid3画素差0、Acid2差1865維持。p191 whole/p173全件100%未達、GitHub publication保留。

Event ws074-p191-q1022-outcome-20261014: [p191](phase191/phase.md) q1022-i01はshorthand inline CSSOMのcanonical値と4 longhand getterを追加しpartial cleared。valid17/17・shorthand5/5 subtest、固定全16件は15 PASS/1 FAIL、subtest165 PASS/14 FAIL。[証拠](phase191/evidence/20261014-q1022/README.md)。Acid3画素差0、Acid2差1865維持。p191 whole/p173全件100%未達、GitHub publication保留。

Event ws074-p191-q1023-clear-20261014: [p191](phase191/phase.md) q1023-i01はcomputed shorthandとthickness cascadeを実装し、固定全16/16 test・179/179 subtest PASS。host build warning0・CSSOM12/12・rule model44/44、Acid3画素差0、Acid2差1865維持。[証拠](phase191/evidence/20261014-q1023/README.md)。p191 whole cleared。p173全3,805件は未達、p189/p190を続ける。GitHub publication保留。

Event ws074-p189-q1024-outcome-20261014: [p189](phase189/phase.md) q1024-i01で単一HTTP shard100件を実測し、9 PASS/68 FAIL/3 harness error/7 no completion/13 no output、4領域候補0。[証拠](phase189/evidence/20261014-q1024/README.md)。p189 partial cleared、whole1,783件とp173全件100%は未達。GitHub publication保留。

Event ws074-p192-design-and-q1025-outcome-20261014: [p192](phase192/phase.md)を追加し、HTML Window named propertiesを担当。q1025-i01で一意HTML id live lookupを実装して局所fixture・host回帰・Acid3差0を確認。[証拠](phase192/evidence/20261014-q1025/README.md)。2対象WPTの先頭ReferenceErrorは消えたが別の未実装でPASS未達。p192 wholeはuncleared、p189/p190/p173の受け入れは不変。GitHub publication保留。

Event ws074-p193-design-and-q1026-outcome-20261014: [p193](phase193/phase.md)を追加しdetails/hidden until-foundを担当。q1026-i01はHTMLDetailsElement.openの専用interface・boolean反映を実装、fixture/hostとAcid3差0を確認。[証拠](phase193/evidence/20261014-q1026/README.md)。対象WPTはhidden until-found値でPASS未達、p193 wholeはuncleared。p189/p173分母と受け入れは不変、GitHub publication保留。

Event ws074-p193-q1027-outcome-20261014: [p193](phase193/phase.md) q1027-i01でhiddenの3状態IDL反映を実装、固定hidden IDL2 WPTは18/18 subtest PASS。[証拠](phase193/evidence/20261014-q1027/README.md)。2025 WPTと2026 living Standardの`undefined`差を明示。beforematchはfragment navigation未完でno output、p193 whole未達。Acid3差0、Acid2差1865維持、GitHub publication保留。

Event ws074-p193-q1028-outcome-20261014: [p193](phase193/phase.md) q1028-i01は同一文書fragment navigationと同期beforematch/revealを実装、局所fixtureで順序・中断を確認。固定WPTはcompletion付きFAILへ進み、次の欠落は`Element.attributes`。[証拠](phase193/evidence/20261014-q1028/README.md)。p193 whole未達、Acid3差0/Acid2差1865維持。GitHub publication保留。

Event ws074-p194-design-20261014: [p194](phase194/phase.md)を追加し、q1028固定WPTで露出した`Element.attributes`/Attrを担当。q1029はSameObject/live collectionの読み取り面に限定し、p193のnavigation責務とp189/p173の分母を維持。GitHub publication保留。

Event ws074-p194-q1029-outcome-20261014: [p194](phase194/phase.md) q1029-i01でSameObject/live属性collectionの読み取りを追加し、固定beforematch WPT1/1 PASS。[証拠](phase194/evidence/20261014-q1029/README.md)。別WPTはreveal順序差でFAIL、p194/p193 wholeとp173全件100%は未達。Acid3差0、Acid2差1865維持、GitHub publication保留。

Event ws074-p193-q1030-outcome-20261014: [p193](phase193/phase.md) q1030-i01でhidden/openのatom同一性を修正、固定中断2 WPT PASS。[証拠](phase193/evidence/20261014-q1030/README.md)。別WPTはtoggle通知待ちでp193 whole未達。Acid3差0、Acid2差1865維持、GitHub publication保留。

Event ws074-p193-q1031-outcome-20261014: [p193](phase193/phase.md) q1031-i01で非同期toggle通知と同task内coalescingを追加、固定hidden/details順序1 WPT PASS、先行beforematch PASS維持。[証拠](phase193/evidence/20261014-q1031/README.md)。p193 wholeとp173全件は未達。Acid3差0、Acid2差1865維持、GitHub publication保留。

Event ws074-p193-q1032-outcome-20261014: [p193](phase193/phase.md) q1032-i01で固定details testharness全18件を測り6 PASS/10 FAIL/2 no_output。[証拠](phase193/evidence/20261014-q1032/README.md)。25 reftest/3 crashtestはp190待ち、p193 wholeとp173全件100%未達。コード無変更、GitHub publication保留。

Event ws074-p190-q1033-outcome-20261014: [p190](phase190/phase.md) q1033-i01で選定静的details reftest10件を監査。既存runnerの4 provisional PASS/6 FAIL中PASS1件はreference chain無視のfalse pass。[証拠](phase190/evidence/20261014-q1033/README.md)。p190 wholeとp173全件100%は未達、runner修正が次。browser/Master無変更、GitHub publication保留。

Event ws074-p190-q1034-outcome-20261014: [p190](phase190/phase.md) q1034-i01で静的match-only参照連鎖をrunnerが辿るようにし、固定10件は3 PASS/7 FAILへ訂正。[証拠](phase190/evidence/20261014-q1034/README.md)。局所graph4/4・Python compile・diff check PASS。p190 wholeとp173全件100%は未達、runner coverage拡大が次。browser/Master無変更、GitHub publication保留。

Event ws074-p193-q1035-outcome-20261014: [p193](phase193/phase.md) q1035-i01でfirst summaryのUA display既定値をlist-itemへ変更し、固定details-displayは8/8 subtest PASS。[証拠](phase193/evidence/20261014-q1035/README.md)。host build/CSS回帰、Acid3/Acid2差画素非増加。p193 whole/p173全件100%未達、WPT/Master/公開ABI無変更、GitHub publication保留。

Event ws074-p193-q1036-outcome-20261014: [p193](phase193/phase.md) q1036-i01でfragment reveal候補をevent前に固定。固定beforematch-infinite-loopは1/1 PASS、先行4件PASS維持。[証拠](phase193/evidence/20261014-q1036/README.md)。element-removal002 no_output維持。host build/関連回帰、Acid3/Acid2差画素非増加。p193 whole/p173全件100%未達、WPT/Master/公開ABI無変更、GitHub publication保留。

Event ws074-p190-q1037-outcome-20261014: [p190](phase190/phase.md) q1037-i01で固定details rootの静的reftest21件を測定し、12 local PASS/9 FAIL/0 error。[証拠](phase190/evidence/20261014-q1037/README.md)。focus残り4 reftestは別directory未実行。p190 whole/p173全件100%未達、browser/Master無変更、GitHub publication保留。

Event ws074-p190-q1038-outcome-20261014: [p190](phase190/phase.md) q1038-i01でhidden directory固定reftest4件中静的2件を測定、0 local PASS/2 FAIL。[証拠](phase190/evidence/20261014-q1038/README.md)。scripted2件未実行。details focus25件中23件測定で12 local PASS/11 FAIL。p190 whole/p173全件100%未達、browser/Master無変更、GitHub publication保留。

Event ws074-p190-q1039-outcome-20261014: [p190](phase190/phase.md) q1039-i01で固定details crashtest3件をlocal rendererで測定し、3 PASS/0 crash/0 timeout。[証拠](phase190/evidence/20261014-q1039/README.md)。残り45 crashtest未測定。p190 whole/p173全件100%未達、browser/Master無変更、GitHub publication保留。

Event ws074-p190-q1040-outcome-20261014: [p190](phase190/phase.md) q1040-i01で固定crashtest全48 IDをlocal rendererで測定し、48 local PASS/0 crash/0 timeout。[証拠](phase190/evidence/20261014-q1040/README.md)。p190 whole/p173全件100%はreftest coverage等が残り未達。browser/Master無変更、GitHub publication保留。

Event ws074-p190-q1041-outcome-20261014: [p190](phase190/phase.md) q1041-i01で固定anchor-positioning reftest77 IDのうち静的21件を実測、0 local PASS/21 FAIL、scripted56件は未実行。[証拠](phase190/evidence/20261014-q1041/README.md)。p190 whole/p173全件100%未達、browser/Master無変更、GitHub publication保留。

Event ws074-p190-q1042-outcome-20261014: [p190](phase190/phase.md) q1042-i01で固定reftest1,974 IDを静的適格1,561/script関連407/variant6に分類し、広域100件を測定して28 local PASS/72 FAIL。[証拠](phase190/evidence/20261014-q1042/README.md)。p190 whole/p173全件100%未達、browser/Master無変更、GitHub publication保留。

Event ws074-p190-q1043-uncleared-20261014: [p190](phase190/phase.md) q1043-i01は静的全1,561 ID中380件のstatusを記録し、中断。[部分証拠](phase190/evidence/20261014-q1043/README.md)。遅いXHTMLと途中保存不足が次のrunner改善点。残り1,181 ID未測定、p190 whole/p173全件100%未達。browser/Master無変更、GitHub publication保留。

Event ws074-p190-q1044-outcome-20261014: [p190](phase190/phase.md) q1044-i01でcase別5秒上限/逐次checkpointを付け、固定静的reftest全1,561 IDを測定、500 local PASS/1,051 FAIL/10 timeout。[証拠](phase190/evidence/20261014-q1044/README.md)。script関連407/variant6未対応。p190 whole/p173全件100%未達、browser/Master無変更、GitHub publication保留。

Event ws074-p195-addition-q1045-20261014: [p190](phase190/phase.md) q1044固定baselineのflexbox XHTML10 timeoutを一般欠陥として切り分ける[p195](phase195/phase.md)を追加。q1045-i01は代表2件の有界診断/単一最小修正と10件再測定に限定。p190 runner基準とp173全件目標は変えない。共有Master/Queue/GitHub publication保留。

Event ws074-p195-q1045-uncleared-and-p196-added-20261014: [p195](phase195/phase.md) q1045-i01は最上位XHTMLをHTML parserへ渡す根因を確定したが、単一flex修正範囲を超えるためuncleared。[p196](phase196/phase.md)を追加して最上位MIME/XML接続を担当させ、q1046でscript-free XHTMLを有限に試す。p190/p173の受け入れと依存は不変。[証拠](phase195/evidence/20261014-q1045/README.md)。共有Master/Queue/GitHub publication保留。

Event ws074-p196-q1046-outcome-and-p195-later-clearance-20261014: [p196](phase196/phase.md) q1046-i01で最上位XHTMLをXML解析に接続、固定timeout10→0、1 local PASS/9 FAIL。host/Acid回帰を満たしpartial cleared、wholeにはscript付きXHTMLが残る。[証拠](phase196/evidence/20261014-q1046/README.md)。先行[p195](phase195/phase.md)は後続証拠でwhole cleared、q1045-i01のuncleared履歴は維持。p190/p173全件100%未達、共有Master/Queue/GitHub publication保留。

Event ws074-p197-addition-q1047-20261014: [p196](phase196/phase.md) q1046後の固定9 Flexbox描画FAILを一般的なmain-axis overflow配置として切り分ける[p197](phase197/phase.md)を追加。q1047は横方向2件の負余白の最小修正と固定10件・Acid回帰へ限定。p190 runnerとp173全件の受け入れは不変。共有Master/Queue/GitHub publication保留。

Event ws074-p197-q1047-outcome-20261014: [p197](phase197/phase.md) q1047-i01は横方向2 IDをlocal PASSに改善しpartial cleared。固定10件は3 PASS/7 FAIL、残り縦方向/writing-modeがwhole受け入れに残る。[証拠](phase197/evidence/20261014-q1047/README.md)。Acid3差0/Acid2差1865不変。p190/p173全件100%未達、共有Master/Queue/GitHub publication保留。

Event ws074-p198-addition-q1048-20261014: [p197](phase197/phase.md) q1047後の縦方向参照誤描画をfloat BFCの親/先頭子margin相殺という一般欠陥として分離し、[p198](phase198/phase.md)を追加。q1048はこの単一規則、代表比較と固定10件・Acid回帰に限定。p197/p190/p173の受け入れは不変。共有Master/Queue/GitHub publication保留。

Event ws074-p198-q1048-outcome-20261014: [p198](phase198/phase.md) q1048-i01はfloat BFC親/子margin非相殺を適用しpartial cleared。固定10件は3 PASS/7 FAILのままだがvert001a/006差画素減、vert001bは残るtest側差で増。[証拠](phase198/evidence/20261014-q1048/README.md)。p198/p197 wholeとp190/p173全件受け入れ未達。共有Master/Queue/GitHub publication保留。

Event ws074-p199-addition-q1049-20261014: [p198](phase198/phase.md) q1048後のvert001aに、`justify-content:right`をcolumn main axisのendと誤解釈する別欠陥を確認。[p199](phase199/phase.md)で物理left/rightとcolumn start fallback、row RTL/reverse/gridを有限に扱う。p197/p198/p190/p173受け入れは不変。共有Master/Queue/GitHub publication保留。

Event ws074-p199-q1049-outcome-20261014: [p199](phase199/phase.md) q1049-i01で物理left/rightを分け、固定10件は3→5 local PASS、残り5 FAIL。host fixture12/12、build warning0、Acid3画素差0/Acid2差1865を確認。[証拠](phase199/evidence/20261014-q1049/README.md)。p199 whole、p197/p190/p173全件100%は未達。共有Master/Queue/GitHub publication保留。

Event ws074-p200-addition-and-q1050-clear-20261014: [p199](phase199/phase.md)後の端数描画3件を扱う[p200](phase200/phase.md)を追加。q1050-i01で正のFlex分配余白の丸めを修正し、対象3件すべてpixel exact PASS、固定10件は8 PASS/2 FAIL。[証拠](phase200/evidence/20261014-q1050/README.md)。p200 whole cleared。p197/p198/p199/p190/p173の残る受け入れは不変。Acid3差0/Acid2差1865、共有Master/Queue/GitHub publication保留。

Event ws074-p201-addition-q1051-20261014: q1050後のvert001bでは親のmin-height:200pxが子配置計算のavailableに入らない。折り返し無し縦Flexのpixel min-heightを扱う[p201](phase201/phase.md)を追加。p197/p198/p199/p200/p190/p173の受け入れは不変。共有Master/Queue/GitHub publication保留。

Event ws074-p201-q1051-clear-20261014: [p201](phase201/phase.md) q1051-i01で固定vert001bをpixel exact PASSへ、固定10件は9 PASS/1 FAIL。内容がmin-heightを超える場合も縮めない局所2ケース、host/CSS/build/Acid回帰を確認。[証拠](phase201/evidence/20261014-q1051/README.md)。p201 whole cleared。残るwriting-modeとp190/p173全件100%は未達。共有Master/Queue/GitHub publication保留。

Event ws074-p202-addition-q1052-20261014: 固定10件最後のwmvertはwriting-mode:vertical-lr未登録でflex rowを物理横軸としている。[p202](phase202/phase.md)でCSS登録/継承、Flex縦main axis、float幅を有限診断し、一般的な小修正のみ試す。p197/p198/p199/p200/p201/p190/p173受け入れは不変。共有Master/Queue/GitHub publication保留。

Event ws074-p202-q1052-clear-and-p197-p199-later-clearance-20261014: [p202](phase202/phase.md) q1052-i01でvertical-lr Flex軸・float幅・line-left/rightを修正し固定10件10/10 local pixel exact PASS。[証拠](phase202/evidence/20261014-q1052/README.md)。host21/21・CSS rule44/44・CSSOM12/12・mutation12/12・warning0 build・Acid3差0/Acid2差1865。先行[p197](phase197/phase.md)・[p198](phase198/phase.md)・[p199](phase199/phase.md)の残るlocal pixel条件と関係回帰も満たしwhole cleared、過去attemptのpartial outcomeは維持。p196/p189/p190/p173全件100%は未達。共有Master/Queue/GitHub publicationとIssue close保留。

Event ws074-p190-q1053-selection-20261014: q1052固定Flexbox10/10 local pixel PASSの影響を[p190](phase190/phase.md) q1044静的1,561件baseline全体で同条件再測定する。p190全2,022件coverageとp173全3,805件目標は維持。共有Master/Queue/GitHub publication保留。

Event ws074-p190-q1053-outcome-20261014: [p190](phase190/phase.md) q1053-i01で固定静的1,561 IDを全再測定、518 local PASS/684 FAIL/359 error/0 timeout。[ID別遷移とfocus集計](phase190/evidence/20261014-q1053/README.md)。全359 errorはDOCTYPE付きXHTMLの未対応、旧PASS→FAIL8件も残る。p190 whole/p173全3,805件100%は未達。共有Master/Queue/GitHub publication保留。

Event ws074-p203-addition-q1054-20261014: q1053の359 error全てにDOCTYPE付きXHTMLのparser ENOTSUPを確認。XML外部識別子を外部取得せず構文だけ受け入れ、内部subset/未知実体を拒否する[p203](phase203/phase.md)を追加。p196/p190/p173の受け入れは不変。共有Master/Queue/GitHub publication保留。

Event ws074-p203-q1054-outcome-20261014: [p203](phase203/phase.md) q1054-i01で外部ID DOCTYPEを副作用なしで受理。359旧error中312件は読み込み成功、122 local PASS/190 pixel FAIL/47 load error。[証拠](phase203/evidence/20261014-q1054/README.md)。残り47件は`&nbsp;`未解決でp203 whole uncleared。host XML243/243・固定Flex10/10 PASS・Acid3差0/Acid2差1865。p190/p173全件受け入れは未達、共有Master/Queue/GitHub publication保留。

Event ws074-p204-addition-q1055-20261014: q1054の残り47 errorは全てXHTML1 Strict公開DTD識別子と`&nbsp;`の組。DTD限定の既知実体値U+00A0を[p204](phase204/phase.md)に分離し、一般XML/別DTDの未知実体拒否を保持。p203/p190/p173受け入れは不変、共有Master/Queue/GitHub publication保留。

Event ws074-p208-q1061-outcome-20261014: [p208](phase208/phase.md) q1061-i01はwhole cleared。base/hypotheticalを分けwrapとmin/max freezeを修正し、q1060新規失敗1件は差5000→0。[証拠](phase208/evidence/20261014-q1061/README.md)。guard3件を含む4/4・固定Flex10/10 pixel exact PASS、Acid3差0。p190/p173全件100%は未達、次Queueで広域census。共有Master/Queue/GitHub publication保留。

Event ws074-p208-addition-q1061-20261014: q1060で新規PASS→FAILとなったflex-wrap1件はmin-width100/flex-basis0を早期凍結し、各行の成長を止めている。[p208](phase208/phase.md)でbase/hypotheticalを分ける。p190/p173受け入れは不変、共有Master/Queue/GitHub publication保留。

Event ws074-p190-q1060-outcome-20261014: [p190](phase190/phase.md) q1060-i01で固定静的1,561 IDを再集計、660 local PASS/901 pixel FAIL/0 error/0 timeout。[証拠](phase190/evidence/20261014-q1060/README.md)。旧8回帰は全件PASS、q1056比21 FAIL→PASS/1 PASS→FAIL（flexbox-flex-wrap-flexing）。q1060部分cleared、p190/p173 whole未達、共有Master/Queue/GitHub publication保留。

Event ws074-p207-q1059-outcome-and-p206-followup-20261014: [p207](phase207/phase.md) q1059-i01はwhole cleared。font shorthandのline-height resetとtextarea描画端pixel丸めで最後の旧回帰1 ID差198→0。[証拠](phase207/evidence/20261014-q1059/README.md)。旧8回帰3件3/3・Flex10/10 local pixel exact PASS、Acid3差0。p206も後続証拠でwhole cleared、q1058当時のpartial結果は保持。p190/p173全件100%未達、共有Master/Queue/GitHub publication保留。

Event ws074-p207-addition-q1059-20261014: q1058残る縦textarea1件198差画素は文字と小数border境界へ局在。[p207](phase207/phase.md)で一般的な描画幾何を切り分ける。p190/p173受け入れは不変、共有Master/Queue/GitHub publication保留。

Event ws074-p206-q1058-outcome-20261014: [p206](phase206/phase.md) q1058-i01でauto主軸reverseとFlex min/max凍結後再配分を実装。固定3件は0→2 local pixel exact PASS、縦textarea1件198差画素。[証拠](phase206/evidence/20261014-q1058/README.md)。固定Flex10/10・Acid3差0維持。q1058部分cleared、p206 whole uncleared、p190/p173全件100%未達、共有投影/GitHub publication保留。

Event ws074-p206-addition-q1058-20261014: 旧PASS→FAIL8件のうちgap5件は回復。残るvertical row1件とtextarea2件を[p206](phase206/phase.md)へ分離し、画像/幾何で原因を調査する。p190/p173受け入れは不変、共有Master/Queue/GitHub publication保留。

Event ws074-p205-q1057-outcome-20261014: [p205](phase205/phase.md) q1057-i01はwhole cleared。4論理margin longhandと2 shorthandをcomputed flowで物理側へ写像し、固定vertical-lr gap5件5/5 local pixel exact PASS。[証拠](phase205/evidence/20261014-q1057/README.md)。host CSS51/51・固定Flex10/10 PASS・Acid3差0/Acid2差1865。残る旧8回帰中3件、p190/p173全件100%未達、共有投影/GitHub publication保留。

Event ws074-p205-addition-q1057-20261014: q1056旧PASS→FAIL8件のうち5件はvertical-lr `gap`と論理margin reference。現parserのhorizontal-ltr物理aliasを[p205](phase205/phase.md)で計算後写像へ変更し、物理宣言とのcascade順位を保つ。p190/p173受け入れは不変、共有Master/Queue/GitHub publication保留。

Event ws074-p190-q1056-outcome-20261014: [p190](phase190/phase.md) q1056-i01は静的1,561 ID再集計を完了、640 local PASS/921 pixel FAIL/0 error/0 timeout。[証拠](phase190/evidence/20261014-q1056/README.md)。q1053旧359 errorは全て描画に進み、122 PASS/237 pixel FAIL。旧8 PASS→FAILは未回復。q1056部分cleared、p190/p173 whole未達。共有Master/Queue/GitHub publication保留。

Event ws074-p204-q1055-outcome-and-p203-followup-20261014: [p204](phase204/phase.md) q1055-i01はwhole cleared。旧47 load errorは0、47 pixel FAILで描画に進んだ。[証拠](phase204/evidence/20261014-q1055/README.md)。host XML249/249・固定Flex10/10 PASS・Acid3差0/Acid2差1865。旧359 load errorの解消で[p203](phase203/phase.md) wholeも後続判定cleared、q1054 attempt結果は保持。p190/p173の画素・coverage・100%は未達、共有Master/Queue/GitHub publication保留。

## クラウド環境での構築と Chromium との比較（2026-10-27）

ユーザーの決定（原文、2026-10-27）: 「あなたはWebブラウザの改良、ws074を担当します。master.mdには書き込まず、ws074/ws.mdと下位のphaseにのみ書き込めます。
環境構築が必要だと思います。クラウド環境でのlibbrowserやそのテスト用シェルの構築など。比較用のChromeのインストールなどもあります。まずはそれらの構築を進めて、手順はws.mdに記録してください。Chrome/Chromiumの描画結果と、libbrowserの描画結果を、比較できる状態まで持っていってください。」

解釈: Claude Code on the web の container（Ubuntu 24.04、4 core、checkout は `/home/user/zedBSD`）を、host の build と Chromium との描画の比較ができる状態にする。
書いてよいのは `plan/ws074/` の下だけ（master.md・queue.md・registry は書かない）。このセッションは Q1 の環境（`/home/awe/`、worktree、実機、QEMU の割当）を持たない。

### 手順（新しい container ごとに）

```sh
cd /home/user/zedBSD
sh plan/ws074/tests/cloud-setup.sh                 # apt の package、chromium の wrapper、fonts.conf
CC=clang sh plan/ws074/tests/host-build.sh plain   # build/ws074-host/plain/browser と host-NAME
python3 plan/ws074/tests/render-compare.py plan/ws074/tests/pages/blocks.html   # 1 頁を比べる
sh plan/ws074/tests/fetch-suites.sh wpt            # WPT の固定 commit を build/ws074-suites/wpt へ（約 30 秒、425 MB）
python3 -I plan/ws074/tests/run-acid-tests.py      # Acid2・Acid3（結果は build/ws074-acid/report.json）
# scripted reftest の client（libbrowser.so を公開 API で使う）
clang -std=gnu11 -O1 -g -Wall -Wextra -Werror -D_GNU_SOURCE -Ibuild/ws074-host/include -Iuserland/desktop/include \
    plan/ws074/tests/wpt-ready-render.c -Lbuild/ws074-host/plain -lbrowser -Wl,-rpath,'$ORIGIN' \
    -o build/ws074-host/plain/wpt-ready-render
# Interop 2025 を Chromium と browser で測って比べる（p260）
python3 plan/ws074/tests/interop2025-chrome-gap.py measure --kind chrome --out build/ws074-cmp/chrome
python3 plan/ws074/tests/interop2025-chrome-gap.py measure --kind ours --out build/ws074-cmp/ours
python3 plan/ws074/tests/interop2025-chrome-gap.py compare --chrome build/ws074-cmp/chrome --ours build/ws074-cmp/ours
```

- WPT は全体を取る（2026-10-27 に fetch-suites.sh の sparse を外した。sparse では Interop の html/ などが欠けた）。

- `cloud-setup.sh` が入れる物: `clang`・`libvulkan-dev`・`mesa-vulkan-drivers`（lavapipe）・`fontconfig`・`python3-pil`。
- **Chrome**: dl.google.com は network の方針で 403、Ubuntu の `chromium` は snap で使えない。container に Playwright が入れた
  Chrome for Testing 153.0.8010.12 があるので、それを使う。ただし full の `chrome --headless --screenshot` は D-Bus が無く終わらない（120 秒の timeout）ので、
  同じ版の `chrome-headless-shell`（`/opt/pw-browsers/chromium_headless_shell-*/`）を使う。
  runner は `chromium` という名前で呼ぶので、`cloud-setup.sh` が wrapper `build/ws074-chrome/bin/chromium` を作り、`/usr/local/bin/chromium` をそれへの symlink にする。
  runner が使う option（`--headless`・`--screenshot`・`--window-size`・`--dump-dom`・`--virtual-time-budget`・`--user-agent` ほか）は headless shell で通る。
- **compiler**: Ubuntu の gcc は `_FORTIFY_SOURCE` を既定で付けるので、`net/loader.c` の `(void)write(...)` が `-Werror=unused-result` で止まる。`CC=clang`（clang 18.1.3）なら warning 0 で通る。
- **font**: `chrome-fonts.sh` が `build/ws074-chrome/fonts.conf` を書き、Chromium にも browser と同じ font（Mahora・JetBrains Mono・Droid Sans Fallback）で描かせる。
- 比べ方: `render-compare.py` は browser の `--render`（CPU）と Chromium の screenshot を 800×600 で撮り、`build/ws074-render/NAME-{ours,chrome,side}.png` を書く。
  side は「ours | Chromium | 差（4 倍）」。どの channel の差も 16 以下の pixel を一致とする（design.md §14.5）。

### 確かめた結果（2026-10-27 18:39 UTC、この container）

| 確認 | 結果 |
| --- | --- |
| `CC=clang sh plan/ws074/tests/host-build.sh plain` | rc 0、warning・error 0（gcc では上の 1 件で止まる） |
| render-compare（pages/ の 6 頁） | blocks 96.89%・flex 98.74%・grid 98.77%・floats 96.76%・decoration 98.65%・first 96.57%。blocks の side を目で見て、layout は一致、差は字形の antialias |
| fetch-suites.sh wpt | 2d66b9b7998b、licence 確認済み |
| run-acid-tests.py | Acid3 100/100・pixel 差 0、Acid2 99.27%（差 873 pixel）。記録の「Acid3 差 0/Acid2 差 873」と同じ |

### 制限（未解決）

- **ライブのサイトとの比較はできない**: container の外への HTTPS は proxy（`HTTPS_PROXY`）を通る。browser の loader は proxy を使わない（`https://example.com/` が No route to host）。
  curl を使う `site-capture.py` も、proxy が example.com への CONNECT を 403 で拒む（GitHub は通る）。
  使うには、ユーザーが環境の Network access で対象の host を許可する必要がある。加えて、live-compare.py に保存した capture（file の path）を渡すか、browser に proxy の対応を足す（範囲の外なので要判断）。
- asan の build（`host-build.sh asan`）・試験の runner の全部（run-interop2025-*、run-wpt-reftests.py ほか）は、この container では未実施。
- 環境は消えるので、`build/` は残らない。新しい container では上の手順をもう一度流す。

## Q1 向けの整理（2026-10-05、Q1）

ユーザー（2026-10-05）「Codexのリミットが足りないので、Codexは非常に難しいドライバのバグを最先端モデルで解決するために温存します。WS074の制御をあなたに戻します。ブラウザのレンダリングを改善するのはきりがない作業なので、それは指示があるまでやらなくていいです。WS107, WS121はあなたが作業します。…WS074のws.mdをあなた向けに整理してほしいです。Acid3は100%クリアになったものの、ピクセル単位の比較ではfailしているので、ピクセル単位でも100%合格を目指す、みたいなステータスでしかかり中だったと記憶しています。」

### 今の状態

| 項目 | 状態 | 根拠 |
| --- | --- | --- |
| 構成 | engine は `userland/desktop/libbrowser/`（WS107 で移した、165 file。2026-10-05 Q1 照合: p172 の取込みの後は `.c` が 167）、shell は `userland/desktop/browser/`（main・shell・app の data）。browser と独立の client は公開の `browser.h` で `libbrowser.so` を動的に link | [WS107](../ws107/ws.md) completed |
| JS | 自前の engine（ES2024 の構文、ES2015 の class・async・generator・Symbol・Map 等、RegExp、Date）。test262 は p025〜p087 の時点の数（全体の最新の計測は未） | 下の Phase の表 |
| デモの目標 | amazon.co.jp のトップと検索（2026-09-28 ユーザー、Google から変更） | [amazon-goal.md](amazon-goal.md) |
| Acid3 | **現行 q1011: 得点100/100、pixel一致100%（差0/480000）**。参照背景画像、得点位置、iframe 子誤描画、hidden input の行高を修復。q1000 の差302056から全302056解消。歴史的な q579 の37.04%は取込み途中の別 source | [p179](phase179/phase.md)、[p178](phase178/phase.md)、[p180](phase180/phase.md)、[p181](phase181/phase.md)、[p182](phase182/phase.md)、[p183](phase183/phase.md)、[p184](phase184/phase.md)、[p185](phase185/phase.md)、[p186](phase186/phase.md)、[p187](phase187/phase.md)、[p188](phase188/phase.md) |
| Acid2 | ~~exact pixel は不合格（p097 の時点で 90.56%）~~ → 2026-10-05 Q1 照合で訂正: **exact pixel 合格**。p099（q507）で 90.56% → 100.00%、p172 の取込み後も plain・ASan で 120000 画素一致（[checkpoint96](phase172/import/checkpoint96/README.md)）。90.56% は p099 の前の値 | [p099](phase099/phase.md)、[p097](phase097/phase.md) |
| p172（Codex の branch の取り込み） | whole cleared（2026-10-03、209/209 の review・hash、host・ASan・ABI・Acid2、target の boot と Venus） | [phase172/import/checkpoint98](phase172/import/checkpoint98/README.md) |
| p100（Acid3 の pixel 完全一致・fail 0） | **planned、しかかり**。q1011 の現行値は得点100/100・pixel一致100%。Acid3 のユーザー目標は達成。Acid2 exact 回帰・target gate により p100 whole は未達 | [p100](phase100/phase.md) |

### Acid3 後の優先対象（2026-10-13 ユーザー更新）

Acid3 得点と pixel 完全一致のユーザー目標は達成。次は p173 の Interop 2025 全対象PASS 100%を優先する。p101 WPT CSS2 reftest 5904件、p174 File System Access API、p175 OPFS、p176 Test262、p098、p036 は独立の未開始候補。p100 whole は別の残余関門として保持。WS priority と focused goal の Master 投影は本セッションの `master.md` 編集禁止のため Q1 に pending。

### 既知の bug（Bug Board）

- [BUG-181](../bugs/BUG-181.md) URL 欄の選択のはみ出し: resolved（compositor の title bar の欄、QEMU T1-189c、実機は UAT）。
- [BUG-182](../bugs/BUG-182.md) タップがクリックにならず HTML の button を押せない: tracking（shell の touch の変換、WS107 の libbrowser の入力）。

### 専任の担当 B1（2026-10-05 ユーザー）

ユーザー「WS074を担当する専用のサブエージェントを設計します。Sonnet 5.5 Midで実行することにします。B1という名前にします。」→ 定義 `.claude/agents/browser-runner-b1.md`（Sonnet 5.5、effort medium）。最初の仕事は [ws074-p178](phase178/phase.md)（Acid3 の 1 領域の pixel 一致の試行と、このモデルで作業できるかの記録）。起動はユーザーの指示の後。

### 次の候補（ユーザーの指示が出たら）

1. BUG-182（タップ）。レンダリングの改善ではないので、指示があれば先に。
2. p100 の残り（Acid3 の pixel 一致）。
3. WS121（browser の動画）は別の WS（下）。

### 関係する WS

- [WS107](../ws107/ws.md): completed（libbrowser の分離）。
- [WS121](../ws121/ws.md): browser の動画の再生（Q1 の担当、2026-10-05）。
- [WS081](../ws081/ws.md): touch の慣性の scroll。

### 記録の照合（2026-10-05 Q1 records reconciliation）

2026-10-05 Q1 records reconciliation: ユーザーの「ブラウザ関連の進捗の記録が整合していない」への対応。git と証拠で確かめられた事だけを書く。

- **Queue ID の衝突と対応表**: Codex の browser3 の branch（2026-10-02〜03）は main の Queue と同じ番号を使った。main の q590（ws004-p051）・q591（ws099-p020）・q592（ws114-p007）・q593（ws095-p012）・q594（ws129-p009）・q595（ws127-p001）・q596（ws005-p018）・q597（ws115-p001）・q598（ws033-p001）は [Queue](../queue.md) の別の作業である。WS074 の記録の中だけ、browser3 の物を次の名で読む（過去の記録の本文は書き換えない）。

  | browser3 の記録の ID | 読み替え | 中身 | 結果 |
  | --- | --- | --- | --- |
  | q590 / q590-i01 | b-q590 | p172 の残り 112 件の全文 review | finished / uncleared（[q590-wrap](phase172/browser3/q590-wrap/README.md)） |
  | q591〜q593（予約のみ） | b-q591〜b-q593 | 使われていない | — |
  | q594 / q594-i01 | b-q594 | p172 の 5 file の review | finished / cleared（部分、whole は uncleared、[README](phase172/browser3/q594/README.md)） |
  | q595 / q595-i01 | b-q595 | p172 の 10 file の review | finished / cleared（部分、whole は uncleared、[README](phase172/browser3/q595/README.md)） |
  | q596 / q596-i01 | b-q596 | p172 の残り 59 件の review（3h） | finished / uncleared（170/209 で期限、[README](phase172/browser3/q596/README.md)） |
  | q597 / q597-i01 | b-q597 | p172 の 39 件の review（3h） | finished / uncleared（204/209、ユーザーの再起動で停止、[README](phase172/browser3/q597/README.md)） |
  | q598 / q598-i01 | b-q598 | p172 の残り 5 件と whole の最終の gate | finished / cleared（p172 whole cleared、[README](phase172/browser3/q598/README.md)） |

  q579（P10）と q584（A1）は main の Queue（[history/queue-q579.md](../history/queue-q579.md)、[queue-q584.md](../history/queue-q584.md)）で、衝突しない。その前の origin/browser2 の branch の q508〜q591 は [main-reconciliation](phase172/import/main-reconciliation.md) で `browser2:qNNN` として既に分けてある。
- **main に入っているか**: main の木（99e7a4b2）で、p172 の最終の review の 209 件（[checkpoint95 の inventory](phase172/import/checkpoint95/review-inventory.json)、source 92・build 1・試験 116）の sha256 を照合し、207 件が一致した。違う 2 件（`plan/ws074/tests/run-acid-tests.py`・`run-dom-tests.py`）は、作り直した main の最初の commit 846464d1 では一致し、2026-10-04 の main の 4628f83e（`build/ws035-fonts` を font の探し先から外した、試験の image の方針）で変わった物で、取込みの後退ではない。追加で直した `libbrowser/bind/input.c` も checkpoint98 の hash と一致した。2026-10-03 のリポジトリの作り直しで git の履歴は 846464d1 に始まるので、いつ・どの merge で入ったかは git では示せない（`git log -- userland/desktop/libbrowser userland/desktop/browser` は 846464d1 と a3b12775（2026-10-05、shell/window.c の 12 行）だけ）。build・試験は今回は流していない（hash の照合だけ）。
- **main に無い記録**: browser3 の記録が指す `plan/agents/browser3/`（ID の依頼・main の merge の衝突の両版）と branch の Queue の archive（`history/queue-q594.md`・`queue-q595.md`・`queue-q596.md`・`queue-q598-browser3.md`）は main に無い（別セッションの記録、main には入れない）。main の `plan/history/` に同じ名の file を作ると main の別の Queue と取り違えるので、phase172/browser3 の README の link を文字に変え、注記した。
- **済んだ gate**: p172 は 2026-10-03 に whole cleared（b-q598）。下の「Blocking gate」は満たされた。後続の Phase は前提では止まっていないが、2026-10-05 のユーザーの指示でレンダリングの改善を止めているので選ばない。
- **古くなった記述**: 下の「2026-09-30 ラップアップと次の作業」の次の作業の順、専任 P10 の枠、「Agent A の ID の照合待ち」はどれも履歴。今の担当・次の候補は上の表。

以下は 2026-10-05 までの記録（Codex の作業を含む履歴）。

2026-10-02 ユーザー更新: ブラウザ専任のP10枠を固定。p172の取込が実際に統合・検証された後は、p100でAcid3の100/100とpixel完全一致・fail 0を目指す。追加指示によりFile System Access API・OPFS・Interop 2025の100%・JavaScript Test262をこの専任枠に積む。p101のCSS2全件目標は保持する。最新のN=3実行指示でp172/q579をP10に投入。後続目標はp172のwhole-Phase clearance後に有限Queueへ選定する。

## デモの目標（2026-09-28 ユーザー、同日に amazon.co.jp へ変更）

**変更**: ユーザー「…amazon.co.jpに変更しましょう。」→ デモの目標は amazon.co.jp（トップの page と検索、限定的な CSS・基本的な JS、WebGL・動画なし）。以下の Google の記述は経緯として残す。

- 調査（[p067](phase067/phase.md)、2026-09-28）: 目標の版・足りない機能・Phase の列は [amazon-goal.md](amazon-goal.md)。私たちの UA に
  desktop の版が challenge なしに返り、配置は CSS だけで決まる（JS なしの Chromium でもほぼ同じ）。検索は普通の GET の form。
  **デモの順**: p032 → p068 → p061 → p069 → p035 → p060 → p062 → p070 → p071 → p037 → p072 → p027・p065 の一部。下の Google の順と
  「実行の順」より優先する。


「Googleの検索トップページと検索が、レイアウトを崩さずに表示できたら、ゴールにしましょう！限定的なCSSと、基本的なJS、WebGLなし、ビデオなしです。」
- 完了の目安: Google の検索のトップの page と検索の結果の page が、レイアウトを崩さずに表示される。検索の box に文字を入れて検索でき、結果の link を開ける。
- 範囲: 限定的な CSS、基本的な JS。WebGL・動画は無し。インタラクション: link・戻る/進む・scroll・検索の box と form への入力・JS を動かす button。
- touch の慣性の scroll 等は [WS081](../ws081/ws.md)。
- 注意（main）: Google は User-Agent・cookie（同意の画面）・JS の有無で返す HTML を変える。p031・p032 の前に、今の browser が受け取る
  HTML と足りない機能（form・CSS・JS の API）を調べる Phase を置く。
- 調査（[p059](phase059/phase.md)、2026-09-28）: 目標の版・足りない機能・Phase の列は [google-goal.md](google-goal.md)。home は私たちの UA に返る
  基本の HTML の版、結果は SearchGuard の JS の challenge を通った後の page（基本の HTML の結果はもう無い）。**デモの順**:
  p032 → p060 → p037（最小）→ p027 → p065 → p066 → p035 → p061 → p062 → p063 → p031 → p064（Google の目標の順。amazon.co.jp への変更で上の順に置き換えた）。

## 目標

2026-09-27 ユーザー:「新しいWSを作ります。Webブラウザを作成します。userland/desktop/browserです。
HTML5のレイアウトエンジンを大まかに作ったあと、JavaScript実行エンジンをあまり最適化にこだわらないで作成し、接続します。そのあと、CSSの互換性を、
標準準拠テストで100%に近づけつつ、-webkitの拡張などはChromeと実際のレンダリング結果を比較しながら100%に近づけていきます。ただし、レイアウトを
100％準拠にするのは無理だし、Chromeと100％互換にするのも無理ですので、目標値を徐々に上げるのがいいと思います。また、JavaScript実行エンジンは、
Wasm実行エンジンと共通化することで、あとでJITコンパイルを導入するときにやりやすくなると思います。libpngは直接使わず、libpng-compatを使います。
JPEGもlibjpeg-compatを作りましょう。WebPはあとで対応します。SSLは、まずはOpenSSLのライブラリでいいですが、最終的には、libsslやlibcryptoの
互換品をbase/に入れます。動画再生はまだドライバがないので後回しでいいです。音声はogg vorbisくらいなら互換ライブラリをlibvorbis-compatとかとして
作れそうですが、これも後回しでいいです。」

zdesktop（Wayland）の上で動く、zedBSD 自前の Web ブラウザ。HTML5 の parser と DOM、CSS、layout と描画、network、画像、JavaScript と
Wasm を base の中に自前で書く（外部の browser engine は取り込まない）。

## 方針（ユーザーの指示から）

1. **順序**: HTML5 の layout engine を大まかに → JavaScript の実行 engine（最適化にこだわらない）を作って接続 → CSS の互換性を上げる。
2. **CSS の互換性**: 標準の準拠試験（WPT の CSS の部分集合など）の通過率を 100% に近づける。`-webkit-` の拡張などは Chrome の実際の描画と
   比べて近づける。100% は無理なので、**目標値を段階的に上げる**（Phase ごとに通過率・一致率の目標を決める）。
3. **JavaScript と Wasm の実行 engine を共通化**（共通の中間表現・bytecode・値の表現・GC・呼び出し規約）し、後の JIT の導入を容易にする。
4. **画像**: PNG は libpng を直接使わず `libpng-compat`（WS035 p041 の範囲、WS071 p010 で作る予定）。JPEG は `libjpeg-compat` を新しく作る。
   WebP は後。
5. **TLS**: 最初は OpenSSL のライブラリ（既存の package）。最終的に base に libssl・libcrypto の互換品を入れる（別の Phase か WS）。
6. **後回し**: 動画（driver が無い）、音声（ogg vorbis なら `libvorbis-compat` で作れそう）、WebP、JIT。

## 完了の条件（p001 で確定）

- zdesktop で browser が起動し、URL を開き、HTML・CSS・画像・JavaScript を含む一般的な静的・動的な page を描いて操作できる
  （使える page の目安は [design.md](design.md) §15）。
- CSS の準拠試験と Chrome との描画の比較で、段階 M4 までの目標値（design.md §15）を満たす。M5 以降の数は後の WS か Phase で決める。
- JavaScript と Wasm が共通の実行 engine（値・GC・bytecode・interpreter・呼び出し規約、design.md §11）の上で動く。
- `libjpeg-compat` が base の library として入り、browser が使う。
- 変更した source の規約の全文の確認（最後の Phase）。

**追加の互換性目標（2026-10-02 ユーザー）**: origin/browser2の統合後、Acid3の100/100に加え、固定条件の参照画像と出力画像をpixel単位で完全一致させ、fail・crash・timeoutを0にする。[File System Access API](phase174/phase.md)と[OPFS](phase175/phase.md)を実装する。Interop 2025の公式focus areaに属するWPTを固定commitと選定manifestで測り、全対象テストのPASS 100%、fail・error・crash・timeout・未説明のskipを0にする。調査項目はfocus areaの得点対象と混同しない。JavaScriptの[Test262](phase176/phase.md)を全suiteで計測し段階的に改善する（今回の指示で最終達成率は未指定）。p173でInteropの対象分母と実行環境を確定するまでは達成を主張しない。既存のWS受け入れとp101 CSS2目標を削除しない。

## 制約と依存

- 新しい code は `plan/coding-style.md` の全文。外部の試験 suite（WPT、html5lib-tests、test262、Wasm の spec test）は source tree に
  取り込まず、commit の SHA を固定して取得・検証して使う（ライセンスを確認、design.md §14.2）。Chrome の参照の描画は host の Debian の
  `chromium`（2026-09-27 に導入、153.0.8010.52）。
- 画面・窓: zdesktop の titlebar（WS070 の CONTROLS、ws070-p011 の後は TABS）。提示は wl_shm（design.md §8.4）。
- 画像: `libpng-compat`・`libz-compat`（ws071-p010、ws035-p040）、`libjpeg-compat`（この WS）。
- 関係: WS035 の p029〜p032（Chromium の port の計画）とは別の到達目標（自前の engine）。両方を残す。

## 判断が要る点（既定で進めた）

design.md §17 の D1〜D11（process の構成、TLS の `dlopen`、titlebar、仕様・Unicode の表の commit、GIF の置き場、wl_shm、
libpng-compat の `from_memory`、libtruetype の拡張、libjpeg-compat の API、既定の font、段階の目標値）。どれも戻せる既定で進める。
2026-09-28 ユーザー:「ブラウザは、現在の方法で進めてください。文字の表は、生成した表をコミットしていいてす。」→ D1〜D3・D5・D9〜D11 は
今の既定のまま。D4 は生成した表を commit する（2026-09-28 に切り替えた、p030 の phase.md）。

## 2026-09-30 ラップアップと次の作業

（2026-10-05 Q1 records reconciliation: この節は履歴。p100 以下の順は 2026-10-05 のユーザーの指示で止めている。今の次の候補は上の「Q1 向けの整理」。）

- [p097](phase097/phase.md) と q506 は cleared。固定WPT commit `2d66b9b7998bb58c336138c178323ddee857b586`で、
  CSS2 reftestの固定sampleは44/100、Acid2はexact pixel不合格・90.56%、Acid3は9/100・pixel agreement 40.35%。
  数値は [results/acid.txt](results/acid.txt) と [results/wpt-reftest.txt](results/wpt-reftest.txt) に残した。
- Amazonのscript有効なguest版は、plain objectの疎な数値keyをdense arrayの`length`として扱っていたVMの不具合を直し、
  70秒後までprocess生存・追加SIGSEGVなしを確認した。最後の実装修正はcommit `6184fb12`。
- q507 で [p099](phase099/phase.md) を cleared。Acid2 は同じ固定条件で
  90.56% から byte-identical な 100.00% になり、plain/ASan と focused
  regression が通った。blocking p172のq579は部分統合・検証を保存してuncleared。全体clear/実出力確認後にp100→p101とbranch側Phaseを再評価し、
  一つずつQueueへ選ぶ。

1. **ws074-p099: Acid2 100% — cleared（q507）**。p097 と同じ harness、viewport、font、固定 WPT 版で
   `exact-pixel-pass 1`、pixel agreement 100.00%、crash・timeout 0。差分を一般化した focused regression を残した。
2. **ws074-p100: Acid3 100%とpixel完全一致** — p172の後。p099はcleared。履歴的Acid3が画面上で100/100を返し、
   固定参照画像との差が0 pixelで、Uncaught・fail・crash・timeoutが無いことを完了条件にする。参照/viewport/font/runnerは着手時に固定する。
3. **ws074-p101: WPT CSS2 reftest 100%** — p172とp100の後。固定commitで現runnerが列挙するscriptなしCSS2 reftest全5904件を
   対象にし、sample 100件ではなく全件でpass 100%、error 0を完了条件にする。manifestと分母を着手時に固定し、
   WPT metadataに基づかない任意の除外はしない。長時間実行はshard・途中再開・失敗cluster別のreportを使う。
4. **ws074-p174: File System Access API** — p172とp100の後。仕様/WPTと権限・picker・shell/engine境界を確定し、機能群を分けて実装する。
5. **ws074-p175: OPFS** — p174の共有handle/権限契約の実出力の後。origin分離した保存領域を仕様/WPTで確かめる。
6. **ws074-p173: Interop 2025 100%** — 公式focus areaの対象WPTを固定版で列挙し、runnerの対応とbaselineを記録する。
   その結果を基に小さな後続Phaseを設計して全対象PASSへ進める。p101は既存の独立したCSS2全件目標として保持する。
7. **ws074-p176: Test262** — 既存runnerをp172の成果と照合して全suiteを固定/計測し、段階的な改善Phaseを作る。最終達成率は未指定。

4〜7は専任P10の投入候補順。独立項目に不要な技術的依存は作らず、各項目の実行Queueはmainが承認と前提を確認して別途投入する。

既存のp098（ES module script）は削除・再採番せずp101の後へ延期する。p036のCSS2全件部分はp101へ分け、
p036はその後のflexbox・backgrounds・values・selectorsの拡張suiteに絞る。p047（DOM testharness）も今回の3 Phaseの後まで候補のまま残す。

## Phase 一覧

**Blocking gate（2026-10-02）**: [p172](phase172/phase.md)のwhole-Phase clearanceと実際の統合出力を、全ての未実行WS074 source/runner/互換性Phaseの共通の前提にする。下表のplanned行の依存欄に明示した。旧cleared成果に遡及しない。branch側phase100・phase102〜171の同じ論理IDは取込時に証拠を照合し、勝手にrenumber/clearしない。

2026-10-05 Q1 records reconciliation: p172 は 2026-10-03 に whole cleared（b-q598）なので、この gate は満たされた。下の表の依存欄の「p172」は満たされた前提として読む。branch 側の 71 Phase（p100・p102〜p171）は [semantic-index](phase172/import/semantic-index.json) で全て「historical evidence only」とし、この表には写していない（同じ ID の二重の行は無い）。後続の Phase はユーザーの指示まで選ばない。

p001 で分け直した（2026-09-27。p002〜p013 の案は実行前の案だったので、同じ番号を新しい分割に使う）。各 Phase は 1〜3 時間を目標にし、
着手の時に大きすぎれば分ける。

**実行の順**（2026-09-27 ユーザー「正常系でワンパス通すのを優先する」、design.md §18）: p005 → p007 → p010 → p011 → p012 → p014（窓に
実際の page）→ p045（URL の欄と link）→ p022 → p023 → p024 → p025 → p026 → p046 → p030（JS の接続）→ p013 → p048 → p049 → p015 → p016 → p017 → p019 → p020 → p051 → p021 → p053 → p052 → p050 → p058 → p054 → p055 → p056 → p057 → p006 → p008 →
p009 → p018 → p027 → p028 → p029 → p047 → p031 → p032 → p033 → p034 → p035 以降。各 Phase は最小の範囲で通し、残りは phase.md の「後回し」へ。

| Phase | 内容 | Status | 依存 |
| --- | --- | --- | --- |
| [ws074-p001](phase001/phase.md) | 全体の設計（[design.md](design.md)）、Chromium の導入、Phase の分割 | cleared | — |
| [ws074-p002](phase002/phase.md) | 骨組み: directory、build の登録（amd64）、`base/`（arena・配列・文字列 buffer・UTF-8/16・hash）、headless の mode の入口、host の build（ASan の組）、suite の取得の script（固定 SHA とライセンスの確認） | cleared | p001 |
| [ws074-p003](phase003/phase.md) | GC heap の核（非移動の mark-sweep、大きさの class の block、保守的な stack の走査、trace）、VM の string と atom | cleared | p002 |
| [ws074-p004](phase004/phase.md) | HTML tokenizer（全状態、文字参照の表の生成）、html5lib の tokenizer の runner。目標 ≥ 98% | cleared | p003 |
| [ws074-p005](phase005/phase.md) | DOM の核（GC の cell の Node・Element・Text・Comment・Document・DocumentType、属性）と tree builder 1（initial〜in body、adoption agency）、html5lib の tree の runner | cleared | p004 |
| ws074-p006 | tree builder 2（table・select・template・frameset・foreign content）、fragment parsing、serializer。目標 script-off ≥ 90% | planned | p005、p172 |
| [ws074-p007](phase007/phase.md) | CSS の最小（ワンパス）: tokenizer・parser、selector（type・class・id・子孫・子・属性の基本）、cascade（origin・specificity・継承）、約 30 の property、UA stylesheet、`<style>`・`style` 属性 | cleared | p003 |
| ws074-p008 | CSS 2（後回し）: selector の残り（構造・状態・`:is`/`:not`/`:has`、pseudo-element）、rule の索引 | planned | p006、p007、p172 |
| ws074-p009 | CSS 3（後回し）: property の表の拡張、`var()`・`calc()`、`@media`、file の `<link>`、shorthand の全部 | planned | p008、p172 |
| [ws074-p010](phase010/phase.md) | font と text の最小: font の一覧（Inter、日本語は Droid の fallback）、libtruetype（関数を足すなら main に先に伝える）、advance、空白と CJK での改行 | cleared | p002 |
| [ws074-p011](phase011/phase.md) | layout の最小: box tree（anonymous box）、block（幅・高さ・margin の基本）、inline（line box・text run・baseline）、`--dump=layout` | cleared | p009、p010 |
| [ws074-p012](phase012/phase.md) | 描画の最小: display list（背景・border の solid・text）、CPU の参照の描画、`--render`（PPM → PNG）、画面の撮影 | cleared | p011 |
| [ws074-p013](phase013/phase.md) | layout 2a: position（relative・absolute・fixed）、offset、z-index の描画の順と hit test（2026-09-28 に float を p048、overflow を p049 へ分けた。list と marker は p011、単位は p007、replaced の大きさは p021） | cleared | p012 |
| [ws074-p048](phase048/phase.md) | layout 2b: float と clear（行の箱を float の横で短くする、block formatting context） | cleared | p013 |
| [ws074-p049](phase049/phase.md) | layout 2c: overflow と clip（display list の clip、CPU と GPU の描画）、overflow が作る block formatting context | cleared | p013 |
| [ws074-p014](phase014/phase.md) | 窓: Wayland と Vulkan（swapchain、display list の GPU の描画: instance の四角と glyph の atlas）、scroll、guest で実際の page を表示。GPU と CPU の描画の比較の試験（2026-09-27 に URL の欄と link を p045 へ分けた） | cleared | p012（p013 は後回しの順） |
| [ws074-p015](phase015/phase.md) | URL（WHATWG）、`data:`、WPT の urltestdata の runner（896/896、data-urls 72/72） | cleared | p002 |
| [ws074-p016](phase016/phase.md) | HTTP/1.1 の同期の client（chunked、redirect）、cookie、http の page と script、host の test server、guest の http（2026-09-28 に非同期の loader・resolver の thread・持続接続・memory の cache を p050 へ分けた） | cleared | p014、p015 |
| [ws074-p017](phase017/phase.md) | TLS（OpenSSL の `dlopen`、D2）、https、自前の CA の host の server、guest で実在の site（BUG-083 の rtld の dlopen の修正を含む） | cleared | p016 |
| ws074-p018 | encoding: 判定（BOM・HTTP・meta の prescan）、UTF-16・legacy の single-byte、Shift_JIS・EUC-JP・ISO-2022-JP（表の生成、D4） | planned | p006、p172 |
| [ws074-p019](phase019/phase.md) | `libjpeg-compat` 1: baseline（huffman、任意の subsampling、restart、grayscale・YCbCr）、library の登録、host の試験（Pillow と比較）。2026-09-28 に base の group・全 platform、libpng-compat の header を `compat/png/` へ | cleared | p002 |
| [ws074-p020](phase020/phase.md) | `libjpeg-compat` 2: progressive、CMYK/YCCK、`jpeg_save_markers`（EXIF の向き）（`JCS_EXT_BGRA` は p019 で済み） | cleared | p019 |
| [ws074-p051](phase051/phase.md) | `libgif-compat`（2026-09-28 ユーザー、D5 の変更）: `userland/base/libgif-compat`、`include/libc/compat/gif_lib.h`（giflib 5.2 の decode の部分集合）、全 platform | cleared | p002 |
| [ws074-p021](phase021/phase.md) | browser の画像: `<img>`（replaced box）、JPEG・PNG・GIF（base の compat の library）、画像の表、CPU と GPU の描画（2026-09-28 に CSS の背景画像を p052 へ分けた） | cleared | p016、p020、p051、ws071-p010 |
| [ws074-p052](phase052/phase.md) | CSS の背景画像（p021 から分けた）: `background-image: url()`、repeat、position、size、`background` の shorthand、canvas の背景画像 | cleared | p021 |
| [ws074-p053](phase053/phase.md) | 部品としての browser の設計（design.md §19）: engine と shell・Wayland の依存の棚卸し、C API（create・destroy・load・resize・呼ぶ側の VkImage への描画・入力の event・callback）の header の案（[browser_view.h](phase053/browser_view.h)）。.so の分割はまだしない。shell の bind・net への直接の依存を page の API に | cleared | p021 |
| [ws074-p022](phase022/phase.md) | VM の核 2: 値（NaN-boxing）、object と shape、配列の elements、関数、realm の骨組み | cleared | p003 |
| [ws074-p023](phase023/phase.md) | 共通の bytecode と interpreter、呼び出し規約、例外の unwind、native 関数（手で組んだ JS 型と Wasm 型の命令の試験） | cleared | p022 |
| [ws074-p024](phase024/phase.md) | JS の lexer と parser（ES2024 の構文 → AST）、test262 の構文の試験（parse だけ。46876/47792） | cleared | p023 |
| [ws074-p025](phase025/phase.md) | JS の compiler（ES5 の核）、`--js` の shell、test262 の runner。最初の数（4992/47792、ES5 1457/8087） | cleared | p024 |
| [ws074-p026](phase026/phase.md) | 組み込み 1a: Object・Function（bind・Function の構築子）・Error の類（engine の誤りも object に）・Boolean・Number（自前の最短の十進表記と十進の読み取り、toFixed 等）・Math・global の関数、native の構築子（2026-09-28 に Array・String・JSON を p046 へ分けた。9327/47792、ES5 4822/8087） | cleared | p025 |
| [ws074-p027](phase027/phase.md) | RegExp の engine と String の regex の method | cleared（2026-09-29。自前の backtracking の engine、RegExp と String の match・replace・replaceAll・search・split。test262 14255 → 15762、ES5 6786 → 7592。v flag・`\p`・modifier・matchAll・Symbol の差し替えは残り） | p026 |
| ws074-p028 | ES2015 の意味 1（2026-09-29 に let・const・TDZ・arrow・template を p078 へ、destructuring・default・rest・spread・optional chaining を p079 へ、class を p085 へ、Symbol・iterator・for-of・Map・Set・Weak* を p087 へ分けた）: 残りは WeakMap・WeakSet の弱さ（collector の ephemeron）、Set の ES2025 の method、iterator helper | planned | p026、p087、p172 |
| ws074-p029 | ES2015 の意味 2（2026-09-29 に generator・Promise・async function を p086 へ分けた）: async generator と for await、`yield*`、Proxy・Reflect、TypedArray・ArrayBuffer・DataView、BigInt | planned | p028、p086、p172 |
| [ws074-p030](phase030/phase.md) | DOM の binding（interface の表、生成器は後回し）、window・document・Node・Element・Event の基本、console、`<script>` の実行、timer、event loop と microtask の queue（2026-09-28 に WPT の testharness の runner を p047 へ分けた） | cleared | p014、p046（p029 から縮めた: microtask の queue はこの Phase で作った） |
| [ws074-p031](phase031/phase.md) | Amazon の script が要る DOM の API（2026-09-29 に絞った）: querySelector・querySelectorAll・matches・closest、getBoundingClientRect・getClientRects・DOMRect・client/offset/scroll の大きさ・scrollX/Y（問われた時に layout）、classList・dataset、inline style（CSSStyleDeclaration）。getComputedStyle・innerHTML・offsetTop/Left・scrollTo は残り | cleared（2026-09-29。Amazon top の DOM の Uncaught 4 → 0、DOM の試験 4 つを追加、guest 12/12） | p030 |
| [ws074-p032](phase032/phase.md) | form（2026-09-28 に範囲を絞った。fetch・XHR・Location・History・localStorage は p064 へ。p067 で `<select>` の最小と `box-sizing` を足した）: `<input>`（text・submit・hidden・button・checkbox）・`<button>`・`<textarea>`・`<select>` の最小の描画、focus と caret、文字の入力と編集、Enter と submit による送信（GET・POST の urlencoded、page の encoding）、script が有効なときの `noscript` を隠す | cleared | p017、p056（focus と key の入力。p031 は要らない） |
| ws074-p033 | Wasm: decoder・validator・共通 bytecode への compiler、JS API、spec test の runner（wabt の wast2json） | planned | p029、p172 |
| ws074-p034 | Wasm の MVP の後: bulk memory、reference types、multi-value、sign-ext、非 trap の変換、SIMD。**M2 の計測** | planned | p033、p032、p172 |
| [ws074-p035](phase035/phase.md) | flexbox（最小） | cleared（2026-09-29。ASan・guest（QEMU、live の Amazon の検索）を通し、入れ子の flex の測定で % の幅が測定の幅になる不具合を直した） | p013 |
| ws074-p036 | CSS の段階 M3-1（2026-09-30にCSS2全件をp101へ分割）: WPT flexbox・backgrounds・values・selectorsの拡張suiteを測り、失敗の多い塊を直す | planned（p101後） | p034、p035、p101、p172 |
| [ws074-p037](phase037/phase.md) | table の layout（最小。CSS2 の auto layout、border-collapse は近似） | cleared（2026-09-29、amazon-goal の「最小」の範囲。test page 60/73 box、Amazon のトップの footer が Chromium と同じ 4 列。rowspan・fixed・collapse の解決・column の box は残り） | p036（デモの列の判断で先に最小） |
| ws074-p038 | transform（2D）、transition・animation、gradient、box-shadow、角丸の clip、opacity、`@font-face`（TTF/OTF） | planned | p036、p172 |
| ws074-p039 | grid | planned | p036、p172 |
| ws074-p040 | Chrome との比較の拡大（corpus と実在の site の保存、box と画素の指標）、`-webkit-` の表（別名、`-webkit-box`、`-webkit-line-clamp` 等）。**M3 の計測** | planned | p037〜p039、p172 |
| ws074-p041 | shell 2: TABS の titlebar と窓の中の toolbar、System Menu、context menu、履歴、ページ内検索、zoom、view-source | planned | p032、ws070-p011、p172 |
| ws074-p042 | CSS の段階 M4: 失敗の塊から機能を選んで直す | planned | p040、p172 |
| ws074-p043 | JS・Chrome の段階 M4: test262 と比較の失敗の塊を直す。**M4 の計測** | planned | p042、p172 |
| ws074-p044 | 変更した source の規約の全文との照合、fuzz（時間を区切って）、回帰、boot test（最後） | planned | 全て、p172 |
| [ws074-p046](phase046/phase.md) | 組み込み 1b（p026 から分けた）: Array・String（正規表現の要らない method、UCD 16.0.0 から生成する大文字・小文字の表）・JSON（14255/47792、ES5 6786/8087） | cleared | p026 |
| [ws074-p045](phase045/phase.md) | 窓 2（p014 から分けた）: CONTROLS の titlebar の URL の欄、link の click（`file:`）、戻る・進む・再読み込み | cleared | p014 |
| [ws074-p050](phase050/phase.md) | 非同期の loader の核（p016 から分けた）: non-blocking な socket と TLS、resolver の thread、redirect、page の画像と shell の navigation の非同期化、Esc の中止。部品化の手順 4（`page_net_*`、p053）。2026-09-28 に持続接続と cache を p058 へ分けた | cleared | p016、p017、p053 |
| [ws074-p058](phase058/phase.md) | 持続接続と memory の cache（p050 から分けた）: host ごとの接続の pool（6 本まで）、長さ・chunked での応答の終わり、Cache-Control（max-age・no-store）、ETag と If-None-Match の再検証、304 | cleared | p050 |
| [ws074-p054](phase054/phase.md) | 部品化 1（p053 の手順 1）: view の型。scroll・履歴・timer・題名の変化を shell から engine の view へ、shell と main の headless の mode は view の API だけを呼ぶ。headless の settle・CPU の描画・dump も view に（2026-09-28） | cleared | p053、p050 |
| [ws074-p055](phase055/phase.md) | 部品化 2（p053 の手順 2）: GPU の描画を `browser_target` と `_record`・`_draw` の形に（view 内の image view ごとの framebuffer）、present.c は swapchain と同期だけ。`--render-gpu` は engine の offscreen に `_draw` | cleared | p054 |
| [ws074-p056](phase056/phase.md) | 部品化 3（p053 の手順 3）: 入力を DOM の key・code・text の形に（evdev の変換は shell）、scroll・link・focus の既定の動作を engine へ。KeyboardEvent・WheelEvent・FocusEvent、Tab の focus と ring。form の欄は無い（p032） | cleared | p055 |
| [ws074-p057](phase057/phase.md) | 部品化 4（p053 の手順 5）: `libbrowser.so` への分割、公開の header `include/libc/browser.h`、2 つ目の使い手 `browser-probe`（Wayland なしで PPM に描く） | cleared | p056 |
| ws074-p047 | WPT の testharness の runner（p030 から分けた: testharness.js は arrow・let・const・class と Promise を使う）、WPT dom/nodes の計測（M2 の目標 ≥ 40%） | planned | p028、p029、p030、p172 |
| [ws074-p059](phase059/phase.md) | Google の調査（デモの目標の前提）: 返る HTML の版（UA ごと）、Chromium との比較、足りない機能、目標の版と Phase の列（[google-goal.md](google-goal.md)） | cleared | p057 |
| [ws074-p060](phase060/phase.md) | `inline-block` を atomic な inline に（shrink-to-fit、baseline、`vertical-align` の top・middle・bottom・baseline） | cleared（2026-09-29。検索の結果が 4 列の card に。`direction: rtl` の不足を発見） | p032 |
| [ws074-p061](phase061/phase.md) | CSS の値と selector（p008・p009 から Google が使う部分）: `@media`、`var()`、`calc()`、`!important`、`:not()`・`:hover`・`:active`・`:focus`・`:visited`、`::before`・`::after`、`box-sizing`、`-webkit-` の別名、`text-overflow`、`letter-spacing`、`text-transform` | cleared（値の部分: var・calc・@media・@supports・論理 property。selector と pseudo-element は p069、`box-sizing` は p032） | p007 |
| [ws074-p062](phase062/phase.md) | 描画（p038 から）: `border-radius`（CPU と GPU）、`opacity`、`box-shadow`、`outline` | cleared（2026-09-29。display list の矩形で表し CPU と GPU が一致、`clip-path: inset()` を足した。gradient・object-fit は残り） | p014 |
| ws074-p063 | （デモの列から外した: Amazon は sprite の PNG）inline の SVG の最小（`svg`・`path`・`circle`・`rect`、`viewBox`、`fill`・`currentColor`） | planned | p012、p172 |
| ws074-p064 | fetch・XHR（same-origin・CORS）、Location・History、localStorage（p032 から分けた） | planned | p031、p032、p172 |
| ws074-p065 | （Google の目標のため。デモの列から外した、2026-09-28）Google の challenge と ES5 bundle の JS の環境（2026-09-29 に `Date` を p076 へ、`navigator`・`screen`・`performance`・`location` の読み取り・`document.cookie` を p077 へ分けた）: `Date`・`Promise`・`Symbol`・`Map`・`Set`・`WeakMap`・typed array の最小、`encodeURIComponent` の類、`Error.stack`、`atob`・`btoa`、`navigator`・`screen`・`performance`・`sessionStorage`・`CustomEvent`、`document.cookie` の書き込み、`location.replace`。保存した challenge の page が `SG_SS` を置いて開き直すまで | planned | p027、p172 |
| ws074-p066 | （Google の目標のため。デモの列から外した）Google の結果の page: 保存（`build/` だけ）、Chromium との比較、足りない CSS と DOM の直し（取得後に分ける） | planned | p065、p172 |
| [ws074-p067](phase067/phase.md) | amazon.co.jp の調査（デモの目標の変更）: UA ごとの HTML、Chromium との比較、足りない機能、Phase の列（[amazon-goal.md](amazon-goal.md)） | cleared | p059 |
| [ws074-p068](phase068/phase.md) | 外の stylesheet（`<link rel=stylesheet>`・`@import`、非同期の loader、読み終えてからの再計算）と rule の索引（最右の id・class・tag） | cleared | p032 |
| [ws074-p069](phase069/phase.md) | selector と pseudo-element: `:not()`・`:is()`・`:where()`・`:has()` の最小、`:root`、構造の pseudo-class、`:hover`・`:focus`・`:disabled`・`:checked`、`::before`・`::after` の `content` | cleared | p068 |
| [ws074-p070](phase070/phase.md) | `@font-face`（WOFF、libz-compat）と Amazon Ember | cleared（2026-09-29。WOFF・TrueType の web font、WOFF2 は残り） | p068 |
| [ws074-p071](phase071/phase.md) | 大きな page の速さ（1.4 MB の HTML、2 MB の CSS）: 測って直す | cleared（2026-09-29。class の atom と計算した style の cache、sheet の到着をまとめる。guest の live の検索が 90 s → 30 s で揃う） | p068、p061 |
| [ws074-p072](phase072/phase.md) | grid の最小（`repeat(N,1fr)`、`grid-column`） | cleared（2026-09-29。test page 44/44 box、トップ 34.16% → 34.72%） | p035 |
| [ws074-p073](phase073/phase.md) | direction の最小（rtl の flex row と `text-align: start`）。2026-09-29 main の判断で追加（Amazon の検索の本体の行が rtl） | cleared（2026-09-29。検索 画素 68.83% → 77.16%、ink 20.06% → 34.68%） | p035 |
| [ws074-p074](phase074/phase.md) | block の幅の intrinsic の keyword（`max-content`・`min-content`・`fit-content`）。2026-09-29 main の判断で追加（Amazon のトップの carousel `.gwm-window-wrapper`） | cleared（2026-09-29。calc の入れ子の clamp、並ぶ float の max-content も。トップ 画素 34.72% → 79.12%、ink 24.43% → 75.06%） | p060 |
| [ws074-p075](phase075/phase.md) | container query の単位（`container-type: inline-size` の container の幅で cqi・cqw 等、要れば `@container` の最小）。2026-09-29 main の判断で追加（Amazon のトップの card の `143cqi`） | cleared（2026-09-29。前の layout の container の大きさで解決し、変われば layout をやり直す。トップ 画素 79.12% → 80.40%） | p061 |
| [ws074-p076](phase076/phase.md) | JS の `Date`（p065 から分けた。amazon-goal の 12 の一部、Amazon の script の最初の blocker）: 構築子・`now`・`parse`（ISO と legacy の形）・`UTC`、local と UTC の getter と setter、文字列の形、`toJSON`、Annex B | cleared（2026-09-29。test262 15762 → 16393、ES5 7592 → 7732、`built-ins/Date` 480/618。Amazon の top の `Date is not defined` 24 → 0。`Symbol.toPrimitive`・Temporal・Intl は残り） | p026、p046 |
| [ws074-p077](phase077/phase.md) | Amazon が要る window の環境の最小（p065 から分けた）: Uncaught の位置（bytecode の位置の表）、`navigator`・`screen`・`performance`・`location`（読むだけ）・`Image` と `HTMLImageElement`・`document.cookie`・`URL`・`hidden` 等、window の plain な property | cleared（2026-09-29。AUI の `P` が定義され、Amazon の top の Uncaught 49 → 10、search 44 → 15、search 画素 72.92% → 76.04%。location の移動は p064） | p030、p076 |
| [ws074-p078](phase078/phase.md) | ES2015 の構文 1a（p028 から分けた）: let・const（block scope、TDZ、反復ごとの binding、script 間の global の lexical な record）、arrow function、template literal（tagged を含む）、Uncaught の早期の誤り（二重の宣言） | cleared（2026-09-29。test262 16393 → 17190、Amazon の Uncaught top 10 → 7、search 15 → 9） | p025、p077 |
| [ws074-p079](phase079/phase.md) | ES2015 の構文 1b（p028 から分けた）: destructuring（宣言・代入・引数・catch・for-in）、default・rest の引数、spread（配列・呼び出し・new・object）、optional chaining。反復は配列・arguments・文字列だけ（Symbol.iterator は p028） | cleared（2026-09-29。test262 17190 → 18558。Amazon の default・rest・optional chaining の誤り 0、次は async・for-of・class） | p078 |
| [ws074-p080](phase080/phase.md) | Encoding API（TextEncoder・TextDecoder、UTF-8）と Web Storage（Storage・sessionStorage・localStorage、origin ごと、localStorage は file に保存、5 MiB の上限）。bind/・page/ の側（p031 の後の Amazon の blocker） | cleared（2026-09-29。Amazon の TextEncoder・sessionStorage の Uncaught 0、guest の live の top は Uncaught 0、DOM の試験 2 つを追加） | p030、p031 |
| [ws074-p081](phase081/phase.md) | HTML の断片の parse と直列化（innerHTML・outerHTML・insertAdjacentHTML・insertAdjacentElement・insertAdjacentText）。Amazon の script での使用が最も多い DOM の API（調べは phase.md） | cleared（2026-09-29。html5lib の fragment 206/206、template.content・namespaceURI、DOM の試験 markup を追加、host 15/15・guest 15/15。Amazon の画素は不変） | p005、p030、p031 |
| [ws074-p082](phase082/phase.md) | getComputedStyle（resolved value、73 の property、live、読むだけ）、offsetParent・offsetTop・offsetLeft、window の scrollTo・scroll・scrollBy と Element の scrollIntoView・scrollTo・scrollBy・root の scrollTop（page が clamp、view が移す）。box の索引（node → 最初の box） | cleared（2026-09-29。DOM の試験 computed を追加、host 16/16・guest 16/16。Amazon の Uncaught と画素は不変） | p031、p081 |
| [ws074-p083](phase083/phase.md) | DOMException（interface・name・code、bind_throw_dom を DOMException に、試験の頁の期待値の見直し）。main の指示（2026-09-29） | cleared（2026-09-29。name・code・instanceof が Chromium と一致、DOM の試験 exception を追加し 5 つの頁を name で見るように、host 18/18・guest 18/18） | p030、p081、p082 |
| [ws074-p084](phase084/phase.md) | Amazon の画素の差の原因の調べ（DOM・layout）と Phase の案。flex の row の item の自動の最小の大きさ（min-width:auto）と凍らせる縮みを直した | uncleared（2026-09-29、wrap up。調べと案は済み: 動的な script が走らない（top の約 10〜13 点）、百分率の高さ、太字の幅ほか。flex の変更の ASan・guest・boot が残り） | p031、p082 |
| [ws074-p085](phase085/phase.md) | class（p028 から分けた。p080〜p084 は DOM の側と衝突しないように空けた）: 宣言と式、constructor（既定を含む）、method・accessor・static、extends・super・new.target、public と private の field・method、static block | **cleared**（2026-09-29。test262 18558 → 21806、新しく落ちた 0。ASan・guest の js・dom の試験・boot 済み。Amazon の class の SyntaxError 2 → 0、Uncaught top 2・search 7、画素は不変） | p078、p079 |
| [ws074-p086](phase086/phase.md) | async function と、それに要る generator・Promise（p029 から分けた）: generator function と yield（`yield*` を除く）、Promise と組み合わせ、microtask の反応、未処理の reject の報告、async function と await | **cleared**（2026-09-29。test262 21806 → 24995、新しく落ちた 0。Amazon の async の誤り top 1・search 4 → 0、Uncaught search 7 → 3） | p030、p085 |
| [ws074-p087](phase087/phase.md) | Symbol・iterator の protocol・for-of と Map・Set・WeakMap・WeakSet（p028 から分けた）、symbol を使う操作（toPrimitive・hasInstance・toStringTag・species・RegExp の委譲）、URI の関数 | **cleared**（2026-09-29。test262 24995 → 28381、新しく落ちた 0。Amazon の for-of・`Set`・`encodeURIComponent` の誤り 0、search の Uncaught 0） | p079、p086 |
| [ws074-p088](phase088/phase.md) | 動的に挿入された `<script src>` の取得と実行、script の load・error の event（bind/・page/・dom/）。Amazon の AUI（`P.load.js` が 41 本を挿入）が初めて走る。p084 の調べの案 1（top で約 10〜13 点の見込み、新しい Uncaught と XMLHttpRequest（p064）の不足が出る見込み） | **cleared**（2026-09-30。dynamic top 69.23%/ink 61.20%、Web API不足をp092へ） | p084 |
| [ws074-p089](phase089/phase.md) | 百分率の高さ（`height`・`max-height` の %、layout 全体で「高さが定まっているか」を渡す。float・inline-block・grid の `1fr` の中を含む）。p084 の案 2 | **cleared**（2026-09-30。top 82.96%→84.06%、ink 79.01%→80.56%。search 75.85%→76.32%、ink 32.97%→34.48%） | p084 |
| ws074-p090 | 合成の太字（Latin）の advance を Chromium に合わせる（約 9% 広い）。p084 の案 3（1 点前後） | planned | p172 |
| ws074-p091 | flex の残り（overflow で隠れる項目の自動の最小、blockification、column の最小）と CSSOM の小さな不足（`cssFloat` など）。p084 の案 4・5 | planned | p084、p172 |
| [ws074-p092](phase092/phase.md) | Amazonの後続scriptが使うWeb API: `fetch`・observer・`document.elementsFromPoint`・`atob`/`btoa`と有界なheadless settle | **cleared**（2026-09-30。非同期fetch、MutationObserver、sign-in tooltipの幅とstacking order。top 71.81%/ink 64.58%、約32秒・Uncaught 4） | p087、p088 |
| ws074-p093 | JS: 型付き配列（ArrayBuffer・TypedArray・DataView） | planned | p087、p172 |
| [ws074-p094](phase094/phase.md) | Chromiumとの再現可能な比較手順: 固定capture、隔離profile、入力・環境・出力のhash、JSON reportとbaseline回帰 | **cleared**（2026-09-30。top 82.96%/ink 79.01%、search 75.85%/ink 32.97%） | p067 |
| [ws074-p095](phase095/phase.md) | Amazon検索欄の文字の位置: flex itemのcross-axisのbox sizing、form controlの`text-indent` | **cleared**（2026-09-30。検索文字の範囲がChromiumと同じ`x=435..574, y=22..36`） | p035、p032、p092 |
| [ws074-p096](phase096/phase.md) | 公開サイトの固定比較corpus: Mozilla日本語topから同一originのpageをたどり、固定Chrome User-AgentでChromiumと比較して一般化できる差を最大2件修正 | **cleared**（2026-09-30。4 page、`@supports`、button空白。top 78.23%/ink 69.54%） | p094、p095 |
| [ws074-p097](phase097/phase.md) | 指定10公開siteの画像・box・DOM tree比較、阿部寛のホームページのguest full-screenデモ、WPT reftestとAcid、一般化できる差を最大4件修正 | **cleared**（2026-09-30。WPT 44/100、Acid2 90.56%、Acid3 9/100。`postMessage`、table rowspan、presentational hint、intrinsic auto margin） | p094、p096 |
| ws074-p098 | ES module scriptのgraph取得・link・評価、`import`・`export`、GitHubのhydration比較 | planned（2026-09-30ユーザー指定によりp101後へ延期） | p087、p088、p097、p172 |
| [ws074-p099](phase099/phase.md) | Acid2 100%: 固定harnessでexact pixel一致、crash・timeout 0 | **cleared**（2026-09-30、q507。90.56% → 100.00%、plain/ASan） | p097 |
| [ws074-p100](phase100/phase.md) | Acid3の100/100、固定参照とのpixel完全一致、fail・Uncaught・crash・timeout 0 | planned（q1011 Acid3得点100/100・pixel差0。Acid2 exact/target gate が未達） | p099、p172（両方 cleared） |
| ws074-p101 | WPT CSS2 reftest 100%: 固定commitのscriptなし全5904件でpass 100%、error 0 | planned（p100後、p172も必須） | p100、p172 |
| [ws074-p172](phase172/phase.md) | origin/browser2のbrowser変更をWS107後のlibbrowser/browser配置へ取込み、関連計画/試験/bugを意味的に照合するblocking gate | cleared / b-q598（browser3 の q598、main の q598 とは別。209/209 review、whole final gates） | WS107の実成果、p099、branch snapshot |
| [ws074-p173](phase173/phase.md) | Interop 2025の公式focus area対象WPTを固定・baseline化し、全件PASSへ向けた失敗群と後続Phaseを設計 | uncleared / q1012-i01 partial cleared、whole baseline未達 | p172、p188 の main 統合済み Acid3 出力、p189/p190のrunner出力 |
| [ws074-p174](phase174/phase.md) | File System Access APIの仕様/WPT・権限・picker/handle契約を確定し、機能群ごとの実装Phaseへ分割 | planning / Queueなし | p172、p100 |
| [ws074-p175](phase175/phase.md) | OPFSのorigin分離・永続保存・handle契約を確定し、機能群ごとの実装Phaseへ分割 | planning / Queueなし | p172、p174の共有契約の実出力 |
| [ws074-p176](phase176/phase.md) | Test262固定全suiteのbaselineと失敗群/runner coverage、段階的改善Phaseの設計 | planning / Queueなし | p172、JS runnerの実出力 |
| [ws074-p177](phase177/phase.md) | BUG-133: `wb_units_reserve` の幾何の増長後の byte 数の overflow を need に落として防ぐ（Q1 が割り当て、2026-10-03、P2） | cleared（2026-10-03 Q1、host 試験） | — |
| [ws074-p178](phase178/phase.md) | Acid3 の 1 領域の pixel 一致を試み、実行上の障害を記録（旧B1評価は現行scopeから除外） | uncleared（q1002-i01 は得点位置の部分 scope cleared、ROI差9572→800。1領域 pixel 完全一致は未達） | p099、p172、p179（cleared） |
| [ws074-p179](phase179/phase.md) | Acid3 参照ページの `reference.png` が背景画像として描かれない原因を特定し、一般的な欠陥を修正・検証 | cleared（q1001-i01。canvas 背景画像の高さ0判定を修正、差56647） | p172、p178 q1000 の baseline |
| [ws074-p180](phase180/phase.md) | iframe の子が親ページに描かれる一般的な原因を調べ、通常の fallback 描画を除く | cleared（q1003-i01、Acid3差565減、frame load52/52） | p172、p179、p178 q1002 の実測出力 |
| [ws074-p181](phase181/phase.md) | HTML `type` 属性 selector の大文字小文字と hidden input の余分な行高を調べる | cleared（q1004-i01、Acid3差24325減、CSS rule44/44） | p172、p179、p180 cleared、q1003 実測出力 |
| [ws074-p182](phase182/phase.md) | HTTP stylesheet 応答の MIME 判定と Acid3 の赤い h1 を調べる | cleared（q1005-i01、Acid3差5294減、HTTP MIME fixture pass） | p172、p179、p180、p181 cleared、q1004 実測出力 |
| [ws074-p183](phase183/phase.md) | CSS hsl()/hsla() 色と Acid3 slash を調べる | cleared（q1006-i01、Acid3差702減、HSL fixture14/14） | p172、p179、p180、p181、p182 cleared、q1005 実測出力 |
| [ws074-p184](phase184/phase.md) | loaded object の fallback DOM 描画と `FAIL` を調べる | cleared（q1007-i01、Acid3差183減、object fallback fixture pass） | p172、p179、p180、p181、p182、p183 cleared、q1006 実測出力 |
| [ws074-p185](phase185/phase.md) | bucket line と後続本文の0.5px位置差を調べる | cleared（q1008-i01、Acid3差8265減、zero-font fixture pass） | p172〜p184 cleared、q1007 実測出力 |
| [ws074-p186](phase186/phase.md) | sharp text-shadow と見出し影を調べる | cleared（q1009-i01、sharp 単一 scope、Acid3差2451減） | p172〜p185 cleared、q1008 実測出力 |
| [ws074-p187](phase187/phase.md) | 負の padding と第6色箱の幅を調べる | cleared（q1010-i01、Acid3差900減、negative padding fixture pass） | p172〜p186 cleared、q1009 実測出力 |
| [ws074-p188](phase188/phase.md) | 同一ページで観測した訪問履歴と :visited の paint color | cleared（q1011-i01、Acid3差801減→0、privacy fixture pass） | p172〜p187 cleared、q1010 実測出力 |
| [ws074-p189](phase189/phase.md) | Interop 2025 testharness 1,783件の結果取得 runner | uncleared / q28 eligible1508・held0・未attempt275 | p173 q1012 固定manifest出力 |
| [ws074-p190](phase190/phase.md) | Interop 2025 reftest 1,974件・crashtest48件のrunner coverage | uncleared / q20 partial cleared・105 scripted PASS・環境5 | p173 q1012 固定manifest出力 |
| [ws074-p191](phase191/phase.md) | Interop 2025 textdecoration全16 testharnessのCSSOM互換 | cleared / q1023、16件16 PASS/0 FAIL | p173固定manifest、p189 q1015全16件baseline |
| [ws074-p192](phase192/phase.md) | Interop 2025 Window named properties互換 | uncleared / q1025 partial cleared、一意id live lookup | p173固定manifest、p189 q1024 shard |
| [ws074-p193](phase193/phase.md) | Interop 2025 details/hidden until-found互換 | uncleared / q1032計測6 PASS/10 FAIL/2 no output、q1035/q1036で2件追加PASS、reftest25/crashtest3未実行 | p189 q1024 shard、p192 q1025一意id output |
| [ws074-p194](phase194/phase.md) | Interop 2025 Element.attributes / Attr | uncleared / q1029 partial cleared、beforematch WPT1 PASS | p193 q1028 fragment output |
| [ws074-p195](phase195/phase.md) | Interop 2025 flexbox XHTML render timeout10件の一般欠陥調査/修正 | cleared / q1046後続証拠でtimeout0・host/Acid受け入れ充足 | p190 q1044 timeout10 ID証拠 |
| [ws074-p196](phase196/phase.md) | Interop 2025 最上位XML/XHTML document loading | uncleared / q19 classic inline partial cleared・full scripting残 | p195 q1045根因証拠 |
| [ws074-p197](phase197/phase.md) | Interop 2025 Flexbox justify-content overflow配置 | cleared / q1052後続証拠で固定残り9件pixel exact PASS | p196 q1046 XHTML描画証拠 |
| [ws074-p198](phase198/phase.md) | Interop 2025 BFCの親/子縦margin相殺禁止 | cleared / q1052後続証拠で固定残り7件pixel exact PASS | p197 q1047縦方向7 FAIL証拠 |
| [ws074-p199](phase199/phase.md) | Interop 2025 justify-content物理left/right | cleared / q1052後続証拠で固定10件pixel exact PASS・grid2/2 | p198 q1048参照差証拠 |
| [ws074-p200](phase200/phase.md) | Interop 2025 Flex分配余白の端数丸め | cleared / q1050対象3 ID pixel exact PASS | p199 q1049端数差証拠 |
| [ws074-p201](phase201/phase.md) | Interop 2025 縦Flex auto高さとmin-height | cleared / q1051固定vert001b pixel exact PASS | p200 q1050固定8 PASS証拠 |
| [ws074-p202](phase202/phase.md) | Interop 2025 vertical-lr Flex軸 | cleared / q1052固定wmvert pixel exact PASS | p201 q1051固定9 PASS証拠 |
| [ws074-p203](phase203/phase.md) | Interop 2025 XHTML外部ID DOCTYPE読込 | cleared / q1055後続証拠で旧359 load error解消 | p190 q1053 error359・p196 XHTML解析 |
| [ws074-p204](phase204/phase.md) | Interop 2025 XHTML1 Strict `nbsp`既知実体 | cleared / q1055旧47 load error解消 | p203 q1054残り47 `nbsp` error |
| [ws074-p205](phase205/phase.md) | Interop 2025 vertical-lr論理margin写像 | cleared / q1057固定gap5 ID local PASS | p190 q1056旧8回帰・p202 Flex axis |
| [ws074-p206](phase206/phase.md) | Interop 2025 Flex残3回帰 | cleared / q1059後続証拠で3 ID local PASS | p190 q1056旧8回帰・p205 gap5回復 |
| [ws074-p207](phase207/phase.md) | Interop 2025 縦textarea小数差 | cleared / q1059固定1 ID差0 | p206 q1058残198差画素 |
| [ws074-p208](phase208/phase.md) | Interop 2025 Flex base/hypothetical分離 | cleared / q1061固定回帰1 ID差0 | p190 q1060新規FAIL・p206 Flex分配 |
| [ws074-p209](phase209/phase.md) | Interop 2025 Document FontFaceSet.ready 初期完了 | uncleared / q17 scoped partial cleared・controls15/35DOM/global旧PASS維持 | p189 q1064 CSS Grid 280 no_completion |
| [ws074-p210](phase210/phase.md) | Interop 2025 CSS Grid WPT SIGSEGV | cleared / q1067通常・ASan completion | p189 q1066再現process_error |
| [ws074-p211](phase211/phase.md) | Interop 2025 CSS Grid inline-axis self alignment | cleared / q1070 LTR基本配置・関連静的旧PASS維持 | p189 q1068・p190 q1062 fixed baseline |
| [ws074-p212](phase212/phase.md) | Interop 2025 CSS Grid horizontal RTL alignment | cleared / q1071代表WPT18/18・静的旧PASS維持 | p211 q1070 LTR出力 |
| [ws074-p213](phase213/phase.md) | Interop 2025 CSS Grid auto-width self alignment | cleared / q1073代表WPT PASS・静的旧PASS維持 | p212 q1071・p189 q1072 fixed baseline |
| [ws074-p214](phase214/phase.md) | Interop 2025 CSS Grid fit-content min-content floor | cleared / q1075系列4 ID改善・静的旧PASS維持 | p213 q1073・p189 q1074 fixed baseline |
| [ws074-p215](phase215/phase.md) | Interop 2025 CSS Grid replaced item normal/stretch | cleared / q1076固定14 WPT全PASS・静的旧PASS維持 | p214 q1075・p189 q1074 fixed baseline |
| [ws074-p216](phase216/phase.md) | Interop 2025 JavaScript module loading | uncleared / q1079–q1086 partial cleared | p189 q1078 module調査 |
| [ws074-p217](phase217/phase.md) | Interop 2025 Grid alignment failures | uncleared / q1120まで部分cleared、縦書き残 | p189 q1087 Grid recensus、p220縦inline出力 |
| [ws074-p218](phase218/phase.md) | Interop 2025 CSS Grid shorthand | uncleared / q1094部分cleared | p189 q1093 Grid recensus |
| [ws074-p219](phase219/phase.md) | Interop 2025 Grid baseline self-alignment | uncleared / q1096部分cleared | p189 q1093 Grid・p218 q1095 track |
| [ws074-p220](phase220/phase.md) | vertical inline text geometry | in-progress / q1121–q1122部分cleared | p217 q1103・q1092診断 |
| [ws074-p221](phase221/phase.md) | Interop 2025 Grid parsing and CSSOM | in-progress / q1139 computed負calc clamping部分cleared、minmax等残 | p189 q1105固定Grid502・p218 shorthand出力 |
| [ws074-p222](phase222/phase.md) | Interop 2025 Grid automatic placement | in-progress / q1133固定2 WPT partial cleared、全placement再測定待ち | p221 q1132-i01 CSS state・q1132-i02暗黙track sizing |
| [ws074-p223](phase223/phase.md) | Empty Grid intrinsic size | in-progress / q1135固定empty-grid部分cleared | p189 q1134後退診断 |
| [ws074-p224](phase224/phase.md) | Grid track animation | uncleared / q1135診断部分cleared、実装未着手 | p189 q1134後退診断 |
| [ws074-p225](phase225/phase.md) | Vertical Grid logical axes | in-progress / q1142固定vertical-lr部分実装 | p217 q1141診断・文字itemはp220 q1122出力 |
| [ws074-p229](phase229/phase.md) | UAT のアドレスバー（compositor 側: launcher の既定 URL、編集中の箱の幅と横 scroll、field の key repeat） | cleared（2026-10-14 Q1、T1-618・T1-620 QEMU PASS、常に箱の表示を含む） | p228（browser の MAIN_HOME_PAGE） |
| [ws074-p228](phase228/phase.md) | UAT の指摘: inline svg の描画（google.com のロゴと icon）、既定 URL を google.com に | cleared（host、2026-10-14 B1。QEMU・実機は未実施） | p231 の土台 |
| [ws074-p230](phase230/phase.md) | yahoo.co.jp の top（gzip の body、SVG の画像、transform、dynamic link ほか） | cleared（host、2026-10-14 B1） | p228 |
| [ws074-p231](phase231/phase.md) | google.com の script（BigInt・型付き配列・currentScript ほか）と Grid の高さ | cleared（host、2026-10-14 B1） | p230 |
| [ws074-p232](phase232/phase.md) | yahoo.co.jp のニュース feed（fetch・Headers・Request・Response・非同期 XHR・Blob・FormData・focus ほか） | cleared（host、2026-10-14 B1。QEMU・実機は未実施。広告の viewability と Proxy は残り） | p230・p231 |
| [ws074-p233](phase233/phase.md) | yahoo.co.jp の feed の画像まわりの枠（`transform: scale` と `transform-origin`、hairline の枠） | cleared（host、2026-10-14 B1。QEMU・実機は未実施） | p232 |
| [ws074-p234](phase234/phase.md) | WPT Interop 2024・2023 の layout の領域（実 site でよく使う物）の PASS を増やす | cleared（host、2026-10-15 B1。aspect-ratio・object-fit・object-position、aspect-ratio の reftest 73→138。QEMU・実機は未実施） | p233 |
| [ws074-p235](phase235/phase.md) | position: sticky（ページのスクロール）と、子を持つ箱の transform: scale | cleared（host、2026-10-15 B1。sticky・子つき scale。固定 WPT 483→485、css-position 51→54、css-transforms 221→226（偶然 pass 4 件が後退）。QEMU・実機は未実施。flex の aspect-ratio は未着手） | p234 |
| [ws074-p240](phase240/phase.md) | flex item の aspect-ratio、自動最小サイズと flex basis | uncleared / ws074-q20261018-01 host partial cleared（WPT142→149、後退0） | p172 engine・p234 ratio の実装 |
| [ws074-p241](phase241/phase.md) | definiteなstretch cross sizeをflex ratioのbase/minimumへ渡す | uncleared / ws074-q20261018-02 host partial cleared、target pending | p234 ratio・p240 minimum |
| [ws074-p242](phase242/phase.md) | ParentNode.replaceChildren、cleanupのDOM一般操作 | uncleared / q04・q05 host部分cleared、factory/iterator/target等残り | p189 q03のcleanup失敗の実測 |
| [ws074-p243](phase243/phase.md) | Grid used trackのresolved CSSOM | uncleared / q09-i02 primary partial cleared | p221のsolver、p189 q03 raw・p190 q06診断 |
| [ws074-p244](phase244/phase.md) | 固定Grid行spanのoverflow | uncleared / q09-i01 partial cleared、general minimum等残り | p243 q07で露出したsolver不備 |
| [ws074-p245](phase245/phase.md) | no-boxのauto minimum resolved value | cleared / q10 no-box own acceptance | p243/q08のGrid raw、actual box proof |
| [ws074-p246](phase246/phase.md) | intrinsic track breadth/contribution | uncleared / q11 24 animation callback loss | q09 minimum rule、q10 output |
| [ws074-p247](phase247/phase.md) | zero font glyph advance/ink | cleared / q12 zero glyph own criteria | q11診断、p185 strut |
| [ws074-p248](phase248/phase.md) | native animation timing/DOM lifecycle | planning / Queue未選定 | p246 scalar output、p224へprimitive提供 |
| [ws074-p249](phase249/phase.md) | zero line/empty margin eligibility | planned / Queue未選定 | p247 zero ink output、p198 BFC基礎 |
| [ws074-p250](phase250/phase.md) | computed fit-content bound | cleared / q14 computed own criteria | p246 scalar output、q12 raw、q13 runner |
| [ws074-p251](phase251/phase.md) | linear calc zero percentage type | cleared / q14 direct linear type | q12/q13 raw、precode Chrome |
| [ws074-p252](phase252/phase.md) | flex-basis grammar/computed・non-replaced content base | uncleared / Queue未選定 | p240/p241残り、q14 final output待ち |
| [ws074-p253](phase253/phase.md) | ordinary attribute mutation records | uncleared / q17 scoped partial cleared・controls15/35DOM/global旧PASS維持 | p209 q16 preflight、p242 observer model |
| [ws074-p254](phase254/phase.md) | User Timing markの実記録・取得・native entry lifecycle | uncleared / native producer verified・complex detail等残り | p077既存clock、p189 q23 source/probe |
| [ws074-p255](phase255/phase.md) | PerformanceObserver native mark登録・queue・task配信 | uncleared / q28 Window consumer/failure/held partial cleared・whole残り | p254 verified native mark output |
| [ws074-p256](phase256/phase.md) | Window ErrorEvent・uncaught harness fidelity | uncleared / q28 runtime/inline source partial cleared・external/syntax等残り | 既存Event/Window/VM→p255再評価 |
| [ws074-p257](phase257/phase.md) | synchronous generator yield* delegation | cleared / q29 sync own criteria・Chrome38/native GC10 | q28 VM/GC・p189 pinned WebIDL2 |
| [ws074-p258](phase258/phase.md) | native Proxy internal methods・GC/revocation | uncleared / q54 global Proxy/revocable partial cleared・whole残り | q29 VM/GC・p189 actual Proxy blocker |
| [ws074-p259](phase259/phase.md) | inline specified numeric stateとflex-factor canonical serialization | in-progress / q55 | p191 CSSOM/p253 attribute commit/q54 full3530 baseline |
| [ws074-p260](phase260/phase.md) | Interop 2025 の Chromium が通る試験を全部通す（2026-10-27 ユーザー） | in-progress / Queue なし（web のセッション） | クラウド環境の構築、chrome-run.js・interop2025-chrome-gap.py |
| [ws074-p261](phase261/phase.md) | History と Navigation API（同じ文書の中）、p260 の一部 | in-progress | p260 の gap の測定 |
| [ws074-p262](phase262/phase.md) | 縦書きの writing-mode を logical の座標で layout する、p260 の一部 | in-progress | p260 の gap の測定 |

2026-10-13 / q1000-terminal-and-p179-addition: p178 の1領域試行は、参照の背景画像が欠ける全画面差のため uncleared。固定 WPT と host build warning0、Acid3 score100/100・差302056/480000、Acid2差1865/120000 の証拠は[p178](phase178/phase.md)と[q1000履歴](../history/queue-q1000.md)。原因の切り分けと修正を[p179](phase179/phase.md)として追加し、verified 出力後に p178 を再試行する。p100/WS074 の受け入れは未達。メインの共有 Queue/Project/GitHub への投影は未実施。

2026-10-13 / q1001-p179-clearance: ユーザーが p179 最大90分の exact scope を承認。root 高さ0の body 背景画像が canvas で省かれる原因を [p179](phase179/phase.md) で修復し、host build warning0・回帰試験 pass。固定 Acid3 は得点100/100・例外0、差56647/480000（302056から245409減）、Acid2 は差1865/120000で不変。[q1001履歴](../history/queue-q1001.md)に証拠を保存。p179 cleared、p178 の旧 attempt は uncleared のまま、p100/WS074 の pixel 完全一致は未達。p178 の次の1領域試行は新 Queue の範囲決定待ち。GitHub/Project は公開保留。

2026-10-13 / q1002-p178-partial: ユーザーが[得点位置の最大90分 scope](phase178/next-queue-proposal.md)を承認。`layout/position.c` の shrink-to-fit の min-content 下限を直し、参照得点の右端x625を確認。[p178の証拠](phase178/evidence/20261013-q1002/report.json)では host-position25/25、build warning0、Acid3 score100/100・差43486/480000（q1001より13161減）、Acid2差1865不変。得点ROI差9572→800。q1002の部分 scopeはclearedだがp178の全 pixel 一致は未達でPhase uncleared。B1評価はユーザーの単独指示で現行scope対象外、p100/WS074も未達。次の差領域は新Queueの選定が必要。

2026-10-13 / p180-addition-after-q1002: q1002 の test paint/layout では iframe 内の `FAIL` が2個描かれ、test 本文 y373.50 と reference 本文 y350 に差がある。[p180](phase180/phase.md)で iframe 子の親ページへの誤描画を一般的に調べる独立 Phase を追加し、q1003 の最大90分 exact scope案を置いた。p178/q1002の outcome と p100 全画面差0の条件は変えない。Queue は未選定・未承認。

2026-10-13 / q1003-p180-clearance: ユーザーが [p180](phase180/phase.md) の最大90分 exact scope を承認。iframe DOM 子の親ページ box 生成を止め、0×0/通常寸法/`display:contents` host fixture、frame load 52/52、build warning0。Acid3 score100/100・差42921/480000（q1002から565減）、Acid2差1865不変。[証拠](phase180/evidence/20261013-q1003/report.json)。p180 cleared。本文 y373.50 は不変で form/inline line の別原因、p100 全画面差0と WS074 acceptance は未達。次 Queue の承認が必要。

2026-10-13 / p181-addition-after-q1003: test の `<input type=HIDDEN>` に対し UA `input[type=hidden]` が現行 selector 比較では一致せず、form の高さ24の line が本文を押し下げる原因候補。[p181](phase181/phase.md) で HTML `type` 属性値の大文字小文字と明示的 `s`/`i` flag に限定した有限の調査・修正・再計測を提案。q1003/p180 の結果と p100 の全画面条件は変えない。Queue は未選定・未承認。

2026-10-13 / q1004-p181-clearance: ユーザーが [p181](phase181/phase.md) の最大90分 exact scope を承認。HTML `type` 属性 selector の flag 無し・`s`・`i` を区別し、`type=HIDDEN` を UA `input[type=hidden]` に一致させた。fixture pass、CSS rule44/44、build warning0。Acid3 score100/100・差18596/480000（q1003から24325減）、Acid2差1865不変。[証拠](phase181/evidence/20261013-q1004/report.json)。p181 cleared。本文 y349.50 / reference y350 ほかの差、p100全画面差0と WS074 acceptance は未達。次 Queue の承認が必要。

2026-10-13 / p182-addition-after-q1004: Acid3 `empty.css.headers` は text/html と指定し、適用時に h1 が赤くなる。q1004 test は赤、reference は黒。[p182](phase182/phase.md) に非 quirks HTML 文書の HTTP stylesheet 応答 MIME 判定を調査・修正・再計測する有限 Phase を追加。q1004/p181 の outcome と p100 全画面条件は変えない。Queue は未選定・未承認。

## 後の WS・Future Work の候補

- base の libssl・libcrypto の互換品（OpenSSL の置き換え）。
- WebP、`libvorbis-compat`（ogg vorbis）と音声、動画（driver の後）、JS・Wasm の JIT。

## ユーザーの判断（2026-09-29 に master から移した）

| 項目 | 決定 | 記録先 |
| --- | --- | --- |
| 部品としての browser（2026-09-28） | ユーザー:「…libbrowser.soというファイルに将来的に分割できるようにして、コンポーネントにしましょう。ブラウザの構造体へのポインタをハンドルにして、Vulkanレンダリングターゲットを指定してイベントも送ってやるとと描画してくれて、Wayland依存はない、みたいなのがいいと思います！これはいきなり実現しなくても、徐々にそういう設計に変えていきましょう。」→ [WS074 design §19](design.md)（engine と shell の分離、不透明な handle、Vulkan の描画の先、将来の libbrowser.so） |
| ブラウザのデモの目標と touch（2026-09-28） | ユーザー:「Googleの検索トップページと検索が、レイアウトを崩さずに表示できたら、ゴールにしましょう！限定的なCSSと、基本的なJS、WebGLなし、ビデオなしです。インタラクションはそれでいいです。」→ WS074 のデモの目標。「タッチについては…スクロールの操作みたいに余韻のあるやつとかも実装が必要で…HIDドライバ、Waylandコンポジタ、ブラウザの3つに渡る…計画は1カ所で…タッチのfpsが低い廉価な機種でも、ある程度数式で補間して利用できるようにすることを目標に」→ [WS081](../ws081/ws.md) |
| ブラウザのデモの目標の変更（2026-09-28） | Google の検索の結果の page のボットの判定（/sorry）の件でユーザー:「それはuser agentを正直に回答したせいであって、Chromeのものを使えばまず問題ないと思うのですが、amazon.co.jpに変更しましょう。」→ WS074 のデモの目標を **amazon.co.jp** に変更（Google は目標から外す）。p059 の Google の調査と form 等の Phase は流用 |


## 2026-10-01 レビュー: libbrowser の source 所有

ユーザーが engine source の libbrowser への移動、browser の window/tab shell、標準 Vulkan を使える Wayland無しの library、抽象入力 interface を指示。
[WS107](../ws107/ws.md) と [境界全文](../standards/browser-component.md)へ反映。配置の旧設計を置き換える。
WS074 は incomplete のまま。p099 の既存証拠/clearance、p100→p101 の順と互換性目標は保持する。
次の WS074 の Queue は新規則と実 source locator/WS107 の移動状況を確認し、同じ source/runner の移行と並行しない。
engine の new source は libbrowser が所有する設計で生成し、Wayland は shell の abstract input adapter に閉じる。
p100/p101 の個別 Phase record はこの2026-10-01記録時点で未作成だった（p100は2026-10-02追加、p101は未作成）。新しい implementation Queue や clearance はこの方針記録から発生しない。
2026-10-01 / review-20261001-policy-ws074: 新設 WS107 と policy 改訂/影響をこの WS に保存。GitHub の WS comment は公開保留。

2026-10-02 / ws107-q541-design: [WS107](../ws107/ws.md)の165engine file移行で現役host/guest/list-sources runnerを新rootへ更新する。独立componentの有限quality修正はWS107、p100→p101の既存互換性scope/順/acceptanceは保持。今後はlibbrowserrootの実sourceを使い、移行と同時編集しない。詳細[設計](../ws107/design.md)。公開outbox pending。

2026-10-02 / ws107-completed: engine165の新root/Makefile/header/ELF/clientをverified、host-view59/Acid2差0/goldens81/native shell PASS。DOM golden position/valuesのfixture追記漏れ2件のみ修正、旧build/newbuild出力一致。WS074のp100→p101/互換性scopeと順は不変。[WS107結果](../history/ws107/conformance.md)。GitHub comment deferred。

## 2026-10-02 / origin/browser2 blocking import

Event ws074-browser2-gate-20261002: ユーザー「GitHubのorigin/browser2を取り込みます。libbrowserへの移動によりストレートには当たらないが、一貫したファイル移動のルールで取込める。Phaseを追加し後続のブラウザ作業をblocking」と指示。[p172](phase172/phase.md)を追加。WS107の[移動inventory](../history/ws107/inventory.json)で旧engine→libbrowser、shellはbrowserと対応させる。branch tip `e53ef03b80113aec959deb67f828cba21d68d4be`を観測、branch Phase IDは171まで使用（p172を選択）。p100→p101の内部順は保持するが、着手の前にp172 whole-Phase cleared/実統合出力が必要。全27 planned行にp172依存を反映、branch取込後の既存/branch Phase結果を証拠で再照合。q507/p099と現在のQueueは履歴のまま、実装/merge/cherry-pick/pushは未実施。GitHub WS/foreign Phase event投稿は保留。

## 2026-10-02 / ブラウザ専任と追加の互換性目標

Event ws074-dedicated-interop2025-20261002: ユーザーが3人案の1人をWS074専任として固定し、branch取込後のAcid3をpixel完全一致・fail 0まで高め、WPT Interop 2025をクリアする目標を追加。P10を未起動の予約枠として[台帳](../agents/registry.md)に記録。p100の受け入れを強化し、新しい[p173](phase173/phase.md)を追加。p101のCSS2全件は保持するが、専任枠での次の互換性計測はp173を先にする。p172→p100→p173、p100→p101。実装Queue/merge/pushは未実施、remote WS/Phase/Project eventはpublication保留。

Event ws074-browser-next-goals-20261002: ユーザーが専任P10の次の目標としてFile System Access API、OPFS、Interop 2025 100%、JavaScript Test262を指定。新しい[p174](phase174/phase.md)・[p175](phase175/phase.md)・[p176](phase176/phase.md)を追加し、p173の数値目標を全対象PASS 100%と明示。専任の投入候補順はp172→p100→p174→p175→p173→p176。p101 CSS2全件は独立候補として保持。Phaseの最初のbaseline/design clearanceは製品目標達成を意味しない。実装Queue/agent起動/merge/pushは未実施、GitHub publication保留。

2026-10-02 / n3-execution-start: current userのN=3継続指示。P8 q577 BUG-125/p017、P9 q578 C10/p014、P10 q579 browser2/p172を有限3時間で開始。既存focus/順位を保持、browser後続はp172 whole-Phase clearance待ち。[Queue](../queue.md)を参照。GitHub publication保留。

2026-10-02 / q579-wrap-uncleared: p172のbranch統合と広い回帰証拠を保存したが、全209対象の最終reviewは8完了、C/header残141、その他残60。userのagent停止指示でq579をuncleared終了。A1が再開候補を所有し、後続browser Phaseは引き続きblocked。GitHub publication保留。

2026-10-02 / c-table-forward-order-20261002: [p172](phase172/phase.md)全文レビューによりC§2の定数callback tableと先行宣言のcompile矛盾を確認、userが必要宣言だけ先行させる例外を承認。全文標準/Guardrail/automationに記録。import gate/他の全文条件/後続依存は不変。GitHub comment未公開。

2026-10-02 / q584-checkpoint06-integrated: A1-001 4da5eb2e5をmain c2743455cへ統合・ACK。4全文reviewと規約修正、worker warning0 host build/.text同一の証拠、main所有権/例外契約/diff/source hash照合。[checkpoint06](phase172/import/checkpoint06/README.md)。p172はin-progress、review12/209でwhole gateは閉じたまま。

2026-10-02 / q584-A1-wrap-uncleared: Latest user requested normal all-agent wrap-up. [p172 checkpoint14](phase172/import/checkpoint14/README.md) retains all209 current hashes,97 full reviews,112 pending C/header files, warning0 affected build/compiles, identical168 object texts and117 actual native checks. q584-i01/whole p172 end uncleared; WS remains incomplete and downstream gates stay closed. Explicit finite p172 continuation and final conformance are required after main merge/reconciliation. GitHub WS/Phase comments remain unpublished under main ownership.

2026-10-02 / browser3-direct-continuation: current userがサブエージェント無しでWS074継続、browser3へのWIP commit/pushを指示。[p172 q590 scope](phase172/browser3/q590-approved-scope.md)を残112件全文reviewへ投入。q58497/209と全209hashを照合、origin/main261903952へ更新。p172全体基準・p100等の後続gateを保持、Issue/Project publication保留。

2026-10-02 / q590-browser3-checkpoint15: [p172 checkpoint15](phase172/import/checkpoint15/README.md) reviews child styles, DOMImplementation and real viewport fixture fully, fixes callback-retired child cache publication under the existing lifetime contract, and retains188 passing native checks/warning0 build. Reviewed100/209, remaining109; WS incomplete, p172 in-progress and downstream acceptance/dependencies unchanged. Internal amendment belongs only to p172; remote Issue comments deferred.

2026-10-02 / q590-browser3-checkpoint16: [p172 checkpoint16](phase172/import/checkpoint16/README.md) completes image geometry/images/CSSOM/select native fixture review,44 native checks and warning0 compile/link. Reviewed104/209, remaining105; no production change, WS incomplete and downstream gates unchanged.

2026-10-02 / q590-browser3-checkpoint17: [p172 checkpoint17](phase172/import/checkpoint17/README.md) reviews click and timer contracts fully, with native171 checks,57 click-page PASS and existing settle22 checks/warning0 build. Reviewed106/209, remaining103; WS incomplete and downstream gates unchanged.

2026-10-02 / q590-browser3-checkpoint18: [p172 checkpoint18](phase172/import/checkpoint18/README.md) fixes reproduced legacy data-resource error leak under existing ownership, retains before/after LeakSanitizer and181 unique native observations/warning0 build. Three full reviews bring109/209, remaining100; WS incomplete, p172 in-progress, downstream dependencies/criteria unchanged. Internal amendment02 changes only p172; GitHub events deferred.

2026-10-02 / q590-browser3-checkpoint19: [p172 checkpoint19](phase172/import/checkpoint19/README.md) finishes2 full C/component reviews, preserving bootstrap order/class/private contracts with native51 checks and47 exact expected JS lines/warning0 build. Reviewed111/209, remaining98; WS incomplete, p172 in-progress, downstream criteria/dependencies unchanged.

2026-10-02 / q590-browser3-checkpoint20: [p172 checkpoint20](phase172/import/checkpoint20/README.md) reviews DOM subscription/form state fully with147 passing native checks/warning0 build. Reviewed113/209, remaining96; WS incomplete, p172 in-progress, downstream criteria/dependencies unchanged.

2026-10-02 / q590-browser3-checkpoint21: [p172 checkpoint21](phase172/import/checkpoint21/README.md) reviews serialization/Symbol fully, with69 exact expected lines/native77/warning0. Reviewed115/209, remaining94; WS incomplete, p172 in-progress, unchanged downstream criteria/dependencies.

2026-10-02 / q590-browser3-checkpoint22: [p172 checkpoint22](phase172/import/checkpoint22/README.md) completes3 native XML/namespace fixture reviews, native73/warning0. Reviewed118/209, remaining91; WS incomplete, p172 in-progress, unchanged downstream criteria/dependencies.

2026-10-02 / q590-browser3-checkpoint23: [p172 checkpoint23](phase172/import/checkpoint23/README.md) reviews Page geometry fully, with exact DOM43 lines/child-style30/native160/warning0. Reviewed119/209, remaining90; WS incomplete, p172 in-progress, unchanged downstream criteria/dependencies.

2026-10-02 / q590-browser3-checkpoint24: [p172 checkpoint24](phase172/import/checkpoint24/README.md) reviews child resource tasks fully, actual loader52/native148/page14/warning0. Reviewed120/209, remaining89; WS incomplete, p172 in-progress, unchanged downstream criteria/dependencies.

2026-10-02 / q590-browser3-checkpoint25: [p172 checkpoint25](phase172/import/checkpoint25/README.md) repairs two reproduced native MIME parsing defects within full review, final data80/resource105/LSAN80/warning0. Supporting fixture checkpoint11 review revalidated; partial publication050031f14 management records completed here. Reviewed121/209, remaining88; WS incomplete, p172 in-progress, unchanged downstream criteria/dependencies.

2026-10-02 / q590-browser3-checkpoint26: [evidence](phase172/import/checkpoint26/README.md), NodeIterator full review, native46/page60/own warning0/style0; original cursor/prototype/weak ownership preserved; reviewed122/209, remaining87 C/header. q590/p172 in-progress, WS incomplete, downstream gates unchanged, Issue publication deferred.

2026-10-02 / q590-browser3-checkpoint27: [evidence](phase172/import/checkpoint27/README.md), Full traversal/removal fixture reviews, native46/scoped warning0/style0; unchanged GC/adoption corpus; reviewed124/209, remaining85 C/header. q590/p172 in-progress, WS incomplete, downstream gates unchanged, Issue publication deferred.

2026-10-02 / q590-browser3-checkpoint28: [evidence](phase172/import/checkpoint28/README.md), Media full review/reproduced six wrong matches fixed, native39/page38/own warning0/style0; supporting fixture revalidated; reviewed125/209, remaining84 C/header. q590/p172 in-progress, WS incomplete, downstream gates unchanged, Issue publication deferred.

2026-10-02 / q590-browser3-checkpoint29: [evidence](phase172/import/checkpoint29/README.md), JSON full review,116 exact JS lines/LSAN9/own warning0;48 permitted cleanup findings, ordinary error guards; reviewed126/209, remaining83 C/header. q590/p172 in-progress, WS incomplete, downstream gates unchanged, Issue publication deferred.

2026-10-02 / q590-browser3-checkpoint30: [evidence](phase172/import/checkpoint30/README.md), CSS model/mutation fixture full reviews, native56/scoped warning0;14 permitted cleanup findings; original corpus unchanged; reviewed128/209, remaining81 C/header. q590/p172 in-progress, WS incomplete, downstream gates unchanged, Issue publication deferred.

2026-10-02 / q590-browser3-checkpoint31: [evidence](phase172/import/checkpoint31/README.md), Select/table full review, native140/page213/own warning0/style0; constant registries and native contracts preserved; reviewed130/209, remaining79 C/header. q590/p172 in-progress, WS incomplete, downstream gates unchanged, Issue publication deferred.

2026-10-02 / q590-browser3-checkpoint32: [evidence](phase172/import/checkpoint32/README.md), Full select/option/table collection fixture reviews, native123/scoped warning0;19 permitted cleanup findings, original GC corpus unchanged.; reviewed133/209, remaining76 C/header. q590/p172 in-progress, WS incomplete, downstream gates unchanged, Issue publication deferred.

2026-10-02 / q590-browser3-checkpoint33: [evidence](phase172/import/checkpoint33/README.md), Realm full review, native82/JS79 exact lines/own warning0/style0; primary/managed ownership and microtask behavior preserved.; reviewed134/209, remaining75 C/header. q590/p172 in-progress, WS incomplete, downstream gates unchanged, Issue publication deferred.

2026-10-02 / q590-browser3-checkpoint34: [evidence](phase172/import/checkpoint34/README.md), Click fixture full review, actual native10/scoped warning0/style0; unchanged no-stack-root activation/cancellation corpus.; reviewed135/209, remaining74 C/header. q590/p172 in-progress, WS incomplete, downstream gates unchanged, Issue publication deferred.

2026-10-02 / q590-browser3-terminal: [p172 terminal](phase172/browser3/q590-wrap/README.md)とarchive（別セッションの記録、main には入れない）を検証。38追加全文review/4組の内部defect修正・有限検証を保存しq590/p172 unclearedで終了。残74全文review/whole最終検証が必要でWS incomplete、p099 retained cleared。受け入れ/後続依存/priorityは変更せず、後続未選定、Issue/Project公開保留。

2026-10-02 / q594-direct-start: [p172 scoped five reviews](phase172/browser3/q594/selection.json) selected under latest user continuation,90min. Whole acceptance/downstream dependencies unchanged, WS incomplete; WIP/browser3 push and post-next-commit cleanup.

2026-10-02 / q594-browser3-checkpoint35: [evidence](phase172/import/checkpoint35/README.md), XML binding/parser/stream fixture full reviews, native51/scoped warning0;24 permitted cleanup findings; original native scripts and GC contracts unchanged.; reviewed138/209, remaining71 C/header. q594/p172 in-progress, WS incomplete, downstream gates unchanged, Issue publication deferred.

2026-10-02 / q594-browser3-checkpoint36: [evidence](phase172/import/checkpoint36/README.md), CSS rule model and Range deletion full reviews, final own warning0 build/native67;18 permitted shared-cleanup jumps; original CSS sources/native scripts and GC root exclusion preserved.; reviewed140/209, remaining69 C/header. q594/p172 in-progress, WS incomplete, downstream gates unchanged, Issue publication deferred.

2026-10-02 / q594-browser3-terminal: [terminal/verified archive](phase172/browser3/q594/README.md). q594 finished/item cleared for exact five-file partial scope, whole p172 uncleared/WS incomplete. Full review135→140/209,69 C/header remain, all209 hashes match, scoped118 native checks/warning0. Requested441-file/~107MiB cleanup completed after next WIP. New Queue/downstream unselected, next unreserved q595/q591–q593 reservations retained; WIP/normal browser3 push, Issue/Project publication deferred.

2026-10-02 / q595-autonomous-start: current userしばらく自走 instruction applied to [finite ten-file selection](phase172/browser3/q595/selection.json), at most3h. All209 source hashes matched q594 terminal. No subagents, whole criteria/downstream dependencies unchanged, shared toolchain untouched, WIP/browser3 push, Issue/Project deferred.

2026-10-02 / q595-browser3-checkpoint37: [evidence](phase172/import/checkpoint37/README.md), Range boundary/data full reviews; native89+100/scoped warning0;36 permitted cleanup jumps; original native GC/root exclusion and mutation scripts preserved.; reviewed142/209, remaining67 C/header. q595/p172 in-progress, WS incomplete, downstream gates unchanged, Issue publication deferred.

2026-10-02 / q595-browser3-checkpoint38: [evidence](phase172/import/checkpoint38/README.md), Range clone/extract full reviews; native15+14/scoped warning0;53 permitted cleanup jumps; original6000/1800 participant corpus and allocation-GC root exclusions preserved.; reviewed144/209, remaining65 C/header. q595/p172 in-progress, WS incomplete, downstream gates unchanged, Issue publication deferred.

2026-10-02 / q595-browser3-checkpoint39: [evidence](phase172/import/checkpoint39/README.md), Range insert/mutation/surround full reviews; native92+36+16/scoped warning0;43 permitted cleanup jumps; original direct native conversion/host callback/1800-node GC and weak tokens preserved.; reviewed147/209, remaining62 C/header. q595/p172 in-progress, WS incomplete, downstream gates unchanged, Issue publication deferred.

2026-10-02 / q595-browser3-checkpoint40: [evidence](phase172/import/checkpoint40/README.md), Collection native/form full reviews plus actual verified unpublished-state and argument-conversion GC repairs; native37+13 and affected12+25+10+65+56=218, own private build/scoped warning0;33 permitted forward cleanup jumps; original caller-root exclusions/corpora and all production strings/23 constant tables preserved.; reviewed150/209, remaining59 C/header. q595/p172 in-progress, WS incomplete, downstream gates unchanged, Issue publication deferred.

2026-10-02 / q595-collection-ownership-amendment01: verified missing callee roots during unpublished-wrapper allocation and four user conversion families repaired within selected collection binding/fixtures. Original failures and exact snapshots retained; final218 actual native checks/warning0 and retirement reclamation verified. Internal design only, foreign scope/interfaces/dependencies unchanged, whole p172 uncleared/WS incomplete and59 remaining full reviews/final gates. Issue/WS remote event delivery deferred. [design/evidence](phase172/browser3/q595/amendment01.md).

2026-10-02 / q595-browser3-terminal: [verified terminal/archive](phase172/browser3/q595/README.md), exact ten-file partial cleared/Queue finished. Review140→150/209,59 C/header remain/all209 current hashes verified, actual native580/warning0. Two reproduced collection GC ownership defects repaired; whole p172 uncleared/WS incomplete/final gates and downstream unchanged. All WIP/browser3 normal push, main only/own invoked builds and fixtures returned, Issues/Project deferred. Next Queue unselected, next free q596/q591–q593 reservations retained.

2026-10-03 JST / q596-autonomous-acid3-start: current user「では、その目標に向かって自走をお願いします。」selects [existing p172 finite continuation](phase172/browser3/q596/selection.json), at most3h/remaining59 full reviews and original whole final checks, from all209 verified current hashes. Goal remains Acid3 score100/100/zero differing pixels; p100 gate and fg017/WS relative priority retained, no new product choice. Main only/all WIP/browser3 push, remote Issues/Project deferred.

2026-10-03 JST / q596-function-ownership-amendment01: [internal design/fix](phase172/browser3/q596/amendment01.md); actual factory/reentrant-GC failures reproduced and input/unpublished/suspended-state ownership revised in the selected files. Scope/interfaces/dependencies/whole criteria unchanged; final checkpoint41 verification pending, p172 in-progress/WS incomplete.

2026-10-03 JST / q596-browser3-checkpoint41: [evidence](phase172/import/checkpoint41/README.md), VM function/factory full reviews; actual parent/code/closure and reentrant-native GC repairs, default factory56/interp45/realms30/ownership27/heap31 plus retired-realm PASS, fixed JS14/14, own warning0;83 permitted forward cleanup findings and original production strings/tables preserved.; reviewed152/209, remaining57 C/header. q596/p172 in-progress, WS incomplete, downstream gates unchanged, Issue publication deferred.

2026-10-03 JST / q596-browser3-checkpoint42: [evidence](phase172/import/checkpoint42/README.md), Realm/context ownership fixture full reviews; native30+27/scoped warning0/style0; immediate checked execution before teardown/source release, output brand guards and preserved actual GC/root exclusions/corpora.; reviewed154/209, remaining55 C/header. q596/p172 in-progress, WS incomplete, downstream gates unchanged, Issue publication deferred.

2026-10-03 JST / q596-browser3-checkpoint43: [evidence](phase172/import/checkpoint43/README.md), Form-owner/native clone/table mutation/row fixture full reviews; native56+26+42+75=199/scoped warning0, 37 permitted cleanup jumps, original actual GC/root exclusions/native scripts/6000/512 cloning corpus and C strings retained.; reviewed158/209, remaining51 C/header. q596/p172 in-progress, WS incomplete, downstream gates unchanged, Issue publication deferred.

2026-10-03 JST / q596-browser3-checkpoint44: [evidence](phase172/import/checkpoint44/README.md), Insertion/mutation-record/style-sheet/submit full reviews; native29+16+21+13=79 plus prior table42+75=117/scoped warning0; 85 allowed single forward cleanup jumps; original native GC/root exclusions/scripts/C strings retained; prior split-call format claim corrected and revalidated.; reviewed162/209, remaining47 C/header. q596/p172 in-progress, WS incomplete, downstream gates unchanged, Issue publication deferred.

2026-10-03 JST / q596-browser3-checkpoint45: [evidence](phase172/import/checkpoint45/README.md), Native SVG length409/text347 full reviews; actual60+42=102 native checks/scoped warning0; 34 permitted forward cleanup jumps; original scalar/text/child XML corpora, precise GC/sole native roots and C strings retained.; reviewed164/209, remaining45 C/header. q596/p172 in-progress, WS incomplete, downstream gates unchanged, Issue publication deferred.

2026-10-03 JST / q596-browser3-checkpoint46: [evidence](phase172/import/checkpoint46/README.md), XML model518/projector541 full reviews; actual236+54=290 native checks/scoped warning0; 20 permitted forward cleanup jumps; original XML syntax/bounds/pinned resources/native graphs, actual30000 wide corpus/precise GC roots and C strings retained.; reviewed166/209, remaining43 C/header. q596/p172 in-progress, WS incomplete, downstream gates unchanged, Issue publication deferred.

2026-10-03 JST / q596-browser3-checkpoint47: [evidence](phase172/import/checkpoint47/README.md), Native property617/shared-heap probe337 full reviews; actual65+8=73 native checks/scoped warning0; 0 permitted forward cleanup jumps; original native property policies/errors/GC and8 realm identity cases/C strings retained.; reviewed168/209, remaining41 C/header. q596/p172 in-progress, WS incomplete, downstream gates unchanged, Issue publication deferred.

2026-10-03 JST / q596-browser3-checkpoint48: [evidence](phase172/import/checkpoint48/README.md), Actual HTTP/local child-loading584 full review; native52/scoped warning0; 25 permitted forward cleanup jumps; original asynchronous/delayed/cancellation/timer/native GC/8MiB pressure/root interval corpus and C strings retained.; reviewed169/209, remaining40 C/header. q596/p172 in-progress, WS incomplete, downstream gates unchanged, Issue publication deferred.

2026-10-03 JST / q596-browser3-checkpoint49: [evidence](phase172/import/checkpoint49/README.md), Frame lifetime1190 full review; original148 native checks/scoped warning0; 79 permitted forward cleanup jumps; all29 native saved graph families/capped failure/owner counters/precise GC root intervals and C strings retained.; reviewed170/209, remaining39 C/header. q596/p172 in-progress, WS incomplete, downstream gates unchanged, Issue publication deferred.

2026-10-03 JST / q596-browser3-terminal: [terminal/verified archive](phase172/browser3/q596/README.md). q596/q596-i01 finished/uncleared at the unchanged 3h deadline, p172 uncleared and WS074 incomplete. Checkpoints41–49 finished20 full reviews, now170/209 with39 production C/header pending. Checkpoint50 captured the real vm_iter_rest GC crash and a scoped repair with warning0 compile and unchanged actual probe exit0; spread.c full review and one style issue remain. Whole p172 manifest/ABI/client/native/ASan/target/boot gates remain unverified, and the whole host builder exit is unknown. p100 remains planned/unselected; last fixed Acid3 comparison is37.04% agreement,302208/480000 differing pixels. Next q597 is only a candidate, q591–q593 reservations retained; no subagents, Issues/Project publication deferred.

2026-10-03 JST / q597-autonomous-start: current userの既存Acid3自走指示により、q596 archive read-back後、[39 source/whole original p172 exact selection](phase172/browser3/q597/selection.json)を有限3hで開始。全209 current hashesとclean HEADを確認、他executor無し。p100はwhole clearance/実出力待ち、WS incomplete、A/B予約と後続依存は維持。main単独、WIP/browser3 push、Issue/Project公開保留。

2026-10-03 JST / q597-browser3-checkpoint51: [evidence](phase172/import/checkpoint51/README.md), VM spread.c全文review完了。実GCで修正前の引数展開NaN/オブジェクトspread失敗/配列spread SIGSEGVを確認し、同じ3経路とrest/protocolの計5probeは修正後exit0。full host build warning0、JS14/14、関連host56+100+31+37、styleは許容forward cleanup54のみ。他の38 C/headerとwhole manifest/ABI/client/ASan/target/bootは未達。review171/209、q597/p172 in-progress、WS incomplete、p100未選定、Issue/Project公開保留。

2026-10-03 JST / q597-browser3-checkpoint52: [evidence](phase172/import/checkpoint52/README.md), JS Function builtin628行全文review。修正前の実GCでapply引数喪失→NaN、bind未公開関数回収→SIGSEGVを再現し、修正後同じ2probe exit0/期待値42・live=1。full host build warning0、JS14/14、host56+100+31、styleは許容forward cleanup30のみ。他の37 C/headerとwhole gatesは未達。review172/209、q597/p172 in-progress、WS incomplete、p100未選定、Issue/Project公開保留。

2026-10-03 JST / q597-browser3-checkpoint53: [evidence](phase172/import/checkpoint53/README.md), DOM mixin631行全文review。検索文字列/中間・結果配列とappend/prepend未挿入Textの実GC所有を修正、full host build warning0、DOM23/23、native insertion29/mutation16/tree exit0、styleは許容forward cleanup19のみ。残36 C/headerとwhole gates未達。review173/209、q597/p172 in-progress、WS incomplete、p100未選定、Issue/Project公開保留。

2026-10-03 JST / q597-browser3-checkpoint54: [evidence](phase172/import/checkpoint54/README.md), native table row655行全文review、source変更なし。scoped compile warning0/style0、元のhost-row75/75・mutation42/42・collection25/25。残35 C/headerとwhole gates未達。review174/209、q597/p172 in-progress、WS incomplete、p100未選定、Issue/Project公開保留。

2026-10-03 JST / q597-browser3-checkpoint55: [evidence](phase172/import/checkpoint55/README.md), CharacterData/Text742行全文review、source変更なし。scoped compile warning0/style0、host Text20/tree exit0/mutation16/range-data100/range-insert92。残34 C/headerとwhole gates未達。review175/209、q597/p172 in-progress、WS incomplete、p100未選定、Issue/Project公開保留。

2026-10-03 JST / q597-browser3-checkpoint56: [evidence](phase172/import/checkpoint56/README.md), HTML reflection803行全文review/属性setter GC所有と宣言順修正。full host build warning0、HTML probe40/40/object URL44/44/DOM23/23/host-form29/url exit0、style許容forward cleanup5のみ。残33 C/headerとwhole gates未達。review176/209、q597/p172 in-progress、WS incomplete、p100未選定、Issue/Project公開保留。

2026-10-03 JST / q597-browser3-checkpoint57: [evidence](phase172/import/checkpoint57/README.md), HTML input原本814行・修正後896行全文review。属性setterの一時文字列/atom GC所有と宣言順を修正、full host build warning0、入力probe94/94・controls100/100・DOM23/23・関連native130/130、styleは許容forward cleanup7のみ。残32 C/headerとwhole gates未達。review177/209、q597/p172 in-progress、WS incomplete、p100未選定、Issue/Project公開保留。

2026-10-03 JST / q597-browser3-checkpoint58: [evidence](phase172/import/checkpoint58/README.md), page sheets853行全文review。非同期URL解決失敗時の部分sheet破棄を修正、full host build warning0/style0、専用page75/75・HTTP async19/19・DOM23/23・native33/33。OOM注入は未実施。残31 C/headerとwhole gates未達。review178/209、q597/p172 in-progress、WS incomplete、p100未選定、Issue/Project公開保留。

2026-10-03 JST / q597-browser3-checkpoint59: [evidence](phase172/import/checkpoint59/README.md), iframe binding890行全文review。table初期化に不要なforward宣言を規約位置へ移動、source意味変更なし。full host build warning0/style0、専用page43/43・関連native242+retirement・DOM23/23。残30 C/headerとwhole gates未達。review179/209、q597/p172 in-progress、WS incomplete、p100未選定、Issue/Project公開保留。

2026-10-03 JST / q597-browser3-checkpoint60: [evidence](phase172/import/checkpoint60/README.md), page lifecycle/dump1038行全文review。dumpの全fallible appendを即時check、失敗layoutの部分tree破棄と前回成功layout復元を修正。full host build warning0、sheets DOM/style golden完全一致、native relayout21/position22、DOM23/23、style許容forward cleanup4のみ。OOM注入未実施。残29 C/headerとwhole gates未達。review180/209、q597/p172 in-progress、WS incomplete、p100未選定、Issue/Project公開保留。

2026-10-03 JST / q597-browser3-checkpoint61: [evidence](phase172/import/checkpoint61/README.md), browser shell main906行全文review。dump/--run console/PPMの出力失敗を非zero終了へ修正、normal4経路と/dev/full失敗3経路確認。full host build warning0/style0、DOM/style golden一致、DOM23/23・JS14/14。残28 C/headerとwhole gates未達。review181/209、q597/p172 in-progress、WS incomplete、p100未選定、Issue/Project公開保留。

2026-10-03 JST / q597-browser3-checkpoint62: [partial evidence](phase172/import/checkpoint62/README.md), DOM nodeのdoctype生成中3文字列rootと属性容量overflowを修正。full host build warning0、native204/204・DOM23/23、style許容forward cleanup2のみ。ただしbind/implementation.cとhtml/modes.cの連続文字列変換中所有が未解決で、dom/node.c全文reviewは未計上、181/209・残28を維持。次は呼び出し元を追跡・修正し再検証。whole gates未達、q597/p172 in-progress、WS incomplete、p100未選定、Issue/Project公開保留。

2026-10-03 JST / q597-browser3-checkpoint63: [evidence](phase172/import/checkpoint63/README.md), DOM node全文reviewを呼び出し元のGC所有修正後に完了。bind/implementation.cの既レビュー箇所を再検証、html/modes.cのDOCTYPE経路を修正し同ファイル全文reviewは保留。全209hash一致、review182/209・残27。full host build warning0、XML probe62/62・native204/204・DOM23/23、style許容forward cleanup26のみ。html5lib corpus未配置、強制GC再現は未主張。whole gates未達、q597/p172 in-progress、WS incomplete、p100未選定、Issue/Project公開保留。

2026-10-03 JST / q597-browser3-checkpoint64: [evidence](phase172/import/checkpoint64/README.md), CSS tokenizer1104行全文review。CRLFのbad-string source offset誤りを修正前後の実token比較で確認し、lookahead加算overflowを防止。全209hash一致、review183/209・残26。full host build warning0/style0、native CSS116/116・DOM23/23・values style golden完全一致。whole gates未達、q597/p172 in-progress、WS incomplete、p100未選定、Issue/Project公開保留。

2026-10-03 JST / q597-browser3-checkpoint65: [evidence](phase172/import/checkpoint65/README.md), TreeWalker全文review・NodeFilter namespace/生成state/callback値/受理nodeの一時GC所有を修正。全209hash一致、review184/209・残25。full host build warning0、traversal実GC9/9・page70 PASS・DOM23/23、style許容forward cleanup7のみ。修正前crash再現は未主張。whole gates未達、q597/p172 in-progress、WS incomplete、p100未選定、Issue/Project公開保留。

2026-10-03 JST / q597-browser3-checkpoint66: [evidence](phase172/import/checkpoint66/README.md), layout box tree全文review・極端な長さ/NaNのlayout unit変換を定義済み範囲へ制限。全209hash一致、review185/209・残24。full host build warning0/style0、UBSan変換5値、native relayout21/position22/table75・DOM23/23・style golden4件完全一致。下流geometry算術は別の制限。whole gates未達、q597/p172 in-progress、WS incomplete、p100未選定、Issue/Project公開保留。

2026-10-03 JST / q597-browser3-checkpoint67: [evidence](phase172/import/checkpoint67/README.md), computed style binding全文review・公開前stateのGC所有を修正。全209hash一致、review186/209・残23。full host build warning0/style0、frame lifetime148/148・関連page68 PASS・DOM23/23。修正前GC再現は未主張。whole gates未達、q597/p172 in-progress、WS incomplete、p100未選定、Issue/Project公開保留。

2026-10-03 JST / q597-browser3-checkpoint68: [evidence](phase172/import/checkpoint68/README.md), page script/event loop全文review・定数表位置と外部script失敗診断の確保結果を修正。全209hash一致、review187/209・残22。full host build warning0/style0、resource105/105・settle14+8・write3経路・child loading52/52・DOM23/23。resource runner初回binary指定誤りは修正後結果のみ採用。whole gates未達、q597/p172 in-progress、WS incomplete、p100未選定、Issue/Project公開保留。

2026-10-03 JST / q597-browser3-checkpoint69: [evidence](phase172/import/checkpoint69/README.md), Window/realm全文review・定数表に必要な宣言だけ前置き、postMessageのoptions/origin/event/callbackをtimer登録までGC rootで保持。全209hash一致、review188/209・残21。full host build warning0、styleは許容のforward cleanup8件のみ。frame lifetime148/148・realm30/30・DOM23/23・settle14+8・postMessage origin5経路（4受信/1拒否）PASS。whole gates未達、q597/p172 in-progress、WS incomplete、p100未選定、Issue/Project公開保留。

2026-10-03 JST / q597-browser3-checkpoint70: [evidence](phase172/import/checkpoint70/README.md), CSSOM1203行全文review。source変更不要、全209hash一致、review189/209・残20。GCC scoped warning0/style0、cp69 whole host build warning0。native CSSOM12/12・mutation12/12・rule model44/44、font指定のpage22/22・23/23、DOM23/23。font省略の初回pageは15 FAIL、必要fontで再実行後の結果のみ採用。whole gates未達、q597/p172 in-progress、WS incomplete、p100未選定、Issue/Project公開保留。

2026-10-03 JST / q597-browser3-checkpoint71: [evidence](phase172/import/checkpoint71/README.md), DOM query/classList/dataset全文review・一時selector文字列、結果配列、token/map/accessorのGC rootを公開まで保持。全209hash一致、review190/209・残19。full host warning0、styleは許容のforward cleanup25件のみ。DOM23/23・child style30/30・3000要素/400 class add/remove cycles/dataset probe PASS。強制GC位置の検証なし。whole gates未達、q597/p172 in-progress、WS incomplete、p100未選定、Issue/Project公開保留。

2026-10-03 JST / q597-browser3-checkpoint72: [evidence](phase172/import/checkpoint72/README.md), strict UTF8/XML parser1524行全文review。source変更不要、全209hash一致、review191/209・残18。GCC scoped warning0/style0、cp71 whole host build warning0。XML model236/236・DOM54/54・binding34/34・node32/32・document GC16/16。whole gates未達、q597/p172 in-progress、WS incomplete、p100未選定、Issue/Project公開保留。

2026-10-03 JST / q597-browser3-checkpoint73: [evidence](phase172/import/checkpoint73/README.md), VM object/array全文review・length非確保判定、symbol/accessor生成、named property/reshape/array shrinkのGC rootを修正。全209hash一致、review192/209・残17。full host warning0、styleは許容のforward cleanup3件のみ。object100/0・array length4/4・VM factory GC56/56・JS14/14・DOM23/23。OOM fault injection未実施。whole gates未達、q597/p172 in-progress、WS incomplete、p100未選定、Issue/Project公開保留。

2026-10-03 JST / q597-browser3-checkpoint74: [evidence](phase172/import/checkpoint74/README.md), HTML tokenizer2481行全文review。source変更不要、全209hash一致、review193/209・残16。GCC scoped warning0/style0、cp73 whole host warning0。9caseのwhole/UTF16単位stream一致、token/error明示検査pass。html5lib corpusはlocalに存在せず未実施。whole gates未達、q597/p172 in-progress、WS incomplete、p100未選定、Issue/Project公開保留。

2026-10-03 JST / q597-browser3-checkpoint75: [evidence](phase172/import/checkpoint75/README.md), HTML insertion modes2119行全文review。source変更不要、全209hash一致、review194/209・残15。GCC scoped warning0/styleは許容cleanup5件、cp73 whole host warning0。tree focused5/5（doctype/head/body/table foster parenting/template）。html5lib corpusなしで未実施。whole gates未達、q597/p172 in-progress、WS incomplete、p100未選定、Issue/Project公開保留。

2026-10-03 JST / q597-browser3-checkpoint76: [evidence](phase172/import/checkpoint76/README.md), HTML parser全文review。detached elementのattribute/template割当中GC rootを追加、C literal/ABI維持、全209hash一致、review195/209・残14。full host/scoped warning0、styleは許容cleanup3件。native parser8/8・stream9/9・table row75/75・DOM23/23・tree3/3。強制GC割当窓未実施。whole gates未達、q597/p172 in-progress、WS incomplete、p100未選定、Issue/Project公開保留。

2026-10-03 JST / q597-browser3-checkpoint77: [evidence](phase172/import/checkpoint77/README.md), VM property access全文review。literal accessor/descriptor accessor/string wrapperのGC rootを追加、C literal/ABI維持、全209hash一致、review196/209・残13。full host/scoped warning0/style0。object100/0・VM factory GC56/56・JS14/14・DOM23/23。強制GC割当窓未実施。whole gates未達、q597/p172 in-progress、WS incomplete、p100未選定、Issue/Project公開保留。

2026-10-03 JST / q597-browser3-checkpoint78: [evidence](phase172/import/checkpoint78/README.md), Element binding全文review。hidden/script.async一時stringのGC rootを追加、C literal/ABI維持、全209hash一致、review197/209・残12。full host/scoped warning0、styleは許容cleanup4件。DOM23/23・form29/0・image geometry10/10。強制GC割当窓未実施。whole gates未達、q597/p172 in-progress、WS incomplete、p100未選定、Issue/Project公開保留。

2026-10-03 JST / q597-browser3-checkpoint79: [evidence](phase172/import/checkpoint79/README.md), Node binding全文review。共有node-array appendでcaller arrayとowning wrapperのGC rootを追加、C literal/ABI維持、全209hash一致、review198/209・残11。full host/scoped warning0/style0。DOM23/23・insertion29/29・clone26/0・collection GC37/37・XML binding34/34。強制GC割当窓未実施。whole gates未達、q597/p172 in-progress、WS incomplete、p100未選定、Issue/Project公開保留。

2026-10-03 JST / q597-browser3-checkpoint80: [evidence](phase172/import/checkpoint80/README.md), Document binding全文review。Cookie無host時の未初期化statusを修正し、namespace/nameと未公開node、streamから除去したchildのGC rootを追加。既存literal維持、error pathの"complete"のみ1個追加、ABI維持、全209hash一致、review199/209・残10。full host/scoped warning0、styleは許容forward cleanup11のみ。DOM23/23、namespace12/12、stream9/9、parser8/8、XML document16/16、XML binding34/34、collection GC37/37。強制GC割当窓未実施。whole gates未達、q597/p172 in-progress、WS incomplete、p100未選定、Issue/Project公開保留。

2026-10-03 JST / q597-browser3-checkpoint81: [evidence](phase172/import/checkpoint81/README.md), inline style binding全文review。一時宣言配列とname/value・未公開accessorのGC rootを追加し全成功/失敗経路で解放。既存literal/ABI維持、全209hash一致、review200/209・残9。full host/scoped warning0、styleは許容forward cleanup35のみ。CSSOM12/12・mutation12/12・rule44/44・style source21/21・DOM23/23・collection GC37/37。強制GC割当窓未実施。whole gates未達、q597/p172 in-progress、WS incomplete、p100未選定、Issue/Project公開保留。

2026-10-03 JST / q597-browser3-checkpoint82: [evidence](phase172/import/checkpoint82/README.md), VM interpreter全文review。arguments object/key/accessor、rest iterator、super array引数をGC保護。generator resume失敗値とloose equalityの未初期化読取りを修正。既存literal/ABI維持、全209hash一致、review201/209・残8。初回host buildのmaybe-uninitializedを修正後、full/scoped warning0、styleは許容forward cleanup13のみ。object100/0・VM factory56/56・array4/4・JS14/14・DOM23/23。強制GC割当窓未実施。whole gates未達、q597/p172 in-progress、WS incomplete、p100未選定、Issue/Project公開保留。

2026-10-03 JST / q597-browser3-checkpoint83: [evidence](phase172/import/checkpoint83/README.md), Object builtin全文review。native vector/descriptor、converted wrapper、未公開結果、tag一時文字列をGC保護。既存literal内容/ABI維持、全209hash一致、review202/209・残7。full/scoped warning0、styleは許容forward cleanup28のみ。object100/0・native properties65/65・VM factory56/56・JS14/14・DOM23/23。全getter窓の強制GC未実施。whole gates未達、q597/p172 in-progress、WS incomplete、p100未選定、Issue/Project公開保留。

2026-10-03 JST / q597-browser3-checkpoint84: [evidence](phase172/import/checkpoint84/README.md), CSS parser全文review。未閉鎖属性/関数selectorと属性flag後の余剰tokenを拒否、nth数値範囲を検証、@supportsのENOMEMを伝播。DOM query異常系5件追加。既存literal内容/ABI維持（s flag literal追加）、全209hash一致、review203/209・残6。full/scoped warning0、style0、CSSOM12/12・rule model44/44・mutation12/12・JS14/14・DOM23/23。ENOMEM注入未実施。whole gates未達、q597/p172 in-progress、WS incomplete、p100未選定、Issue/Project公開保留。

2026-10-03 JST / q597-browser3-checkpoint85: [evidence](phase172/import/checkpoint85/README.md), Event binding全文review。新規Event/EventTarget状態とwrapper、constructor option取得、dispatch listener snapshotのGC root寿命を補強し、getter成功/失敗経路を明示。bind/input.cの隣接Keyboard/Wheel callerに未root箇所を発見したが、q597対象外のため未修正。全209hash一致、review204/209・残5。full/scoped warning0、styleは許可されたcleanup goto15件のみ、click10/10・JS14/14・DOM23/23。全経路の強制GC未試験、whole gates未達、q597/p172 in-progress、WS incomplete、p100未選定、Issue/Project公開保留。

2026-10-03 JST / q597-browser3-checkpoint86: [evidence](phase172/import/checkpoint86/README.md), Array helper部分review。array_hasのerror時未初期化出力、sort/joinのGC root寿命、scratch割当失敗を修正。全209hash一致、review204/209・残5（Arrayは部分review）。full/scoped warning0、styleは許可されたcleanup goto5件のみ、JS14/14。全callback強制GC・ENOMEM注入未実施。whole gates未達、q597/p172 in-progress、WS incomplete、p100未選定、Issue/Project公開保留。

2026-10-03 / q597-terminal: archive（別セッションの記録、main には入れない）, ユーザー再起動停止によりq597-i01/p172 uncleared、WS074 incomplete。全209hash一致・204/209全文review・残5、Array部分root修正、最後のhost warning0/JS14/14/DOM23/23/click10/10。p172 whole gate未達のためp100未選定。再開はcheckpoint87照合と次の有限Queue選定。

2026-10-03 / q598-start: [selection](phase172/browser3/q598/selection.json)、ユーザーの再開指示でq597未達を有限3hへ再選定。残5全文review、input caller限定修正、元のp172 whole gate。WS incomplete、p100未選定。

2026-10-03 JST / q598-checkpoint88: [evidence](phase172/import/checkpoint88/README.md), bind/input.cのKeyboardEvent/WheelEvent wrapperを追加getter中にGC rootで保持。209 inventoryの対象外なのでreview countは204/209・残5のまま、全209hash一致。whole/scoped warning0、JS14/14・DOM23/23、styleは許可cleanup goto7のみ。強制GC getter全経路未試験、whole gate未達、q598/p172 in-progress、WS incomplete、p100未選定。

2026-10-03 JST / q598-checkpoint89: [evidence](phase172/import/checkpoint89/README.md), Array constructor/prototype、新規copy配列、pop/shift戻り値、reduce accumulatorの一時GC rootを追加。全209hash一致、Arrayは部分reviewで204/209・残5のまま。whole/scoped warning0、JS14/14・DOM23/23、styleは許可cleanup goto29のみ。全再入経路の強制GC未実施、whole gate未達、q598/p172 in-progress、WS incomplete、p100未選定。

2026-10-03 JST / q598-checkpoint90: [evidence](phase172/import/checkpoint90/README.md), Array helperのreceiver/value、flat再帰mapped値、reverse一時値をGC rootで保護。全209hash一致、Arrayは部分reviewで204/209・残5のまま。whole/scoped warning0、JS14/14・DOM23/23、styleは許可cleanup goto33のみ。primitive wrapper等の残reviewと全経路強制GCは未実施、whole gate未達、q598/p172 in-progress、WS incomplete、p100未選定。

2026-10-03 JST / q598-checkpoint91: [evidence](phase172/import/checkpoint91/README.md), js/builtin_array.c 2858行の全文C/component reviewと再入GC root補強を完了。全209hash一致、review205/209・残4。GCC14.2 whole/scoped warning0、JS14/14・DOM23/23、focused Array5例、styleは許可cleanup goto47のみ。全callback強制GC/ENOMEM注入は未実施。whole gate未達、q598/p172 in-progress、WS incomplete、p100未選定。

2026-10-03 JST / q598-checkpoint92: [evidence](phase172/import/checkpoint92/README.md), bind/environment.c 3123行の全文C/component reviewを完了。MutationObserverのjob enqueue失敗時にrecordsを失わず再試行可能に修正。全209hash一致、review206/209・残3。GCC14.2 whole/scoped warning0、JS14/14・DOM23/23、style指摘0。enqueue ENOMEM注入/全callback強制GCは未実施。whole gate未達、q598/p172 in-progress、WS incomplete、p100未選定。

2026-10-03 JST / q598-checkpoint93: [evidence](phase172/import/checkpoint93/README.md), bind/range.c 3219行の全文C/component reviewを完了、source修正不要。全209hash一致、review207/209・残2 CSS。GCC14.2 whole/scoped warning0、Range native8本373検査・JS14/14・DOM23/23、style指摘0。全allocation失敗注入は未実施。whole gate未達、q598/p172 in-progress、WS incomplete、p100未選定。

2026-10-03 JST / q598-checkpoint94: [evidence](phase172/import/checkpoint94/README.md), css/values.c 4533行の全文C/component reviewを完了。色指定末尾の余分なtoken拒否と出力capacity検査を実装。全209hash一致、review208/209・残cascade.c。GCC14.2 whole/scoped warning0、CSS host5本128検査・JS14/14・DOM23/23、無効色4例期待通り、style指摘0。capacity故障注入未実施。whole gate未達、q598/p172 in-progress、WS incomplete、p100未選定。

2026-10-03 JST / q598-checkpoint95: [evidence](phase172/import/checkpoint95/README.md), css/cascade.c 4227行の全文C/component reviewを完了。メディア条件の上限超過を黙って切り捨てずE2BIGに修正。全209hash一致、review209/209・残0。GCC14.2 whole/scoped warning0、CSS host128/128・JS14/14・DOM23/23、style指摘0。元のwhole final gatesは未達、q598/p172 in-progress、WS incomplete、p100未選定。

2026-10-03 JST / q598-checkpoint96: [evidence](phase172/import/checkpoint96/README.md), 209/209最終hash・追加input.c全文確認。browser2 manifest569件はapplied209/archive356/excluded4/unresolved0、原Phase71件は履歴として保持。host plain/ASan各98回帰+81golden・Acid2 120000画素一致、独立client CPU/GPU描画、39exports/167source/377include・禁則依存0。target初回image/boot PNG合格、browser source警告0・外部package警告233行。最終warm buildのimage書込/最終boot/guest shellを残し、q598/p172 in-progress、WS incomplete、p100未選定。

2026-10-03 JST / q598-p172-clearance: [p172](phase172/phase.md)を[最終証拠](phase172/import/checkpoint98/README.md)でwhole cleared。209/209対象の全文review、branch manifest/ABI、host+ASanとAcid2、最終target bootとVenus shellを確認。初回cold p014の5秒待ち失敗・再試行PASSを保持。WS074固有のacceptance（Acid3ほか）は未達なのでincompleteのまま。p100は前提解消のみで実行Queue未選定。Agent A側とのQueue ID/共有投影・GitHub投稿は保留。

2026-10-05 Q1 records reconciliation: ユーザーが WS074 を Codex から Q1 に戻し、レンダリングの改善をユーザーの指示まで止めた。記録の矛盾を照合した: Queue ID の衝突（browser3 の q590・q594〜q598 → b-q590・b-q594〜b-q598、WS074 の記録の中だけ）、p172 の取込みの main への実在（209 件中 207 件の hash 一致、2 件は main の 4628f83e の試験の方針の変更）、Acid2 の行の訂正（p099 で exact 合格）、Acid3 の値は checkpoint02 の計測で最終の source では再計測していないこと、p100 は planned（Queue なし、2026-10-03 の baseline の承認は未使用）、main に無い browser3 の記録への link の無効化、壊れた link 2 件（design.md・WS081）。Master・Queue・Past Log・Bug Board の直しは Q1 に提案した。source・build・試験は変えていない。

## 2026-10-06 UAT のフィードバック

- BUG-227 amazon.co.jp が白い画面（libbrowser か shell かの切り分けから）。BUG-206・207 は直し済み・T1-219 待ち

## 他の WS からの覚え（2026-10-07 Q1 が受けた）

- ws090-p025（P1）で `browser.h` に `browser_view_text_target`・`browser_view_compose`・`browser_view_commit_text` を足し（追加だけ、export は browser_view_* の wildcard）、`dom_control` に `preedit` と `preedit_cursor` を足した。web の form の欄の IME の口。
- host-browser-ime で、textarea の value が script から undefined になることが分かった（binding の未実装、plan/ws177/backlog-p1.md にも）。

2026-10-13 / q1005-p182-clearance: [p182](phase182/phase.md) で非 quirks HTML の HTTP stylesheet MIME を判定。実 HTTP fixture 修正前 FAIL→修正後 PASS、build warning0。固定 Acid3 score100/100・差13302/480000（5294減）、Acid2差1865不変。[証拠](phase182/evidence/20261013-q1005/report.json)。p182 cleared。slash 色と上部の余分な文字等、p100 全画面差0・WS074 acceptance は未達。ユーザーの自動反復指示に従い次の最大90分 Queue を選定する。

2026-10-13 / p183-addition-q1006: q1005 差画像に赤い slash と上部の余分な文字。`css_parse_color()` の HSL/HSLA 未対応を原因候補として [p183](phase183/phase.md) に局所 host 試験・最小実装・固定 Acid3/Acid2 を限定。ユーザーの最大90分 Queue 自動反復指示で q1006-i01 を選定。p182 clearance と p100 全画面条件は不変。

2026-10-13 / q1006-p183-clearance: [p183](phase183/phase.md) で CSS HSL/HSLA 色を追加。host HSL14/14・build warning0、固定 Acid3 score100/100・差12600/480000（702減）、Acid2差1865不変。[証拠](phase183/evidence/20261013-q1006/report.json)。p183 cleared。上部文字等、p100全画面差0とWS074 acceptanceは未達。次の最大90分 Queue を自動選定する。

2026-10-13 / p184-addition-q1007: q1006 差画像の左上 `FAIL` は nested object fallback と一致。loaded HTML object の子を親 layout から除く候補を [p184](phase184/phase.md) に限定し、ユーザーの最大90分 Queue 自動反復指示で q1007-i01 選定。p183 clearance と p100 全画面条件は不変。

2026-10-13 / q1007-p184-clearance: [p184](phase184/phase.md) で loaded HTML object の fallback を親 paint から除外し、404 object は fallback を保持。実 HTTP fixture 修正前 FAIL→修正後 PASS、frame load pass、build warning0。固定 Acid3 score100/100・差12417/480000（183減）、Acid2差1865不変。[証拠](phase184/evidence/20261013-q1007/report.json)。p184 cleared。上部リンク文字等、p100全画面差0とWS074受入は未達。次の最大90分 Queue を自動選定する。

2026-10-13 / p185-addition-q1008: q1007 layout/paint で得点と本文の y が reference より0.5px上、枠下辺も連動。[p185](phase185/phase.md) に bucket line と負 margin の最小 fixture 調査・一般欠陥のみ修正を限定し、最大90分 q1008-i01 を選定。p184 clearance と p100全画面条件は不変。

2026-10-13 / q1008-p185-clearance: [p185](phase185/phase.md) で font-size0 strut の偽0.5px を修正。最小 fixture 修正前 FAIL→修正後 PASS、position25/25、build warning0。固定 Acid3 score100/100・差4152/480000（8265減）、Acid2差1865不変。[証拠](phase185/evidence/20261013-q1008/report.json)。p185 cleared。見出し影・上部リンク文字等、p100全画面差0とWS074受入は未達。次の最大90分 Queue を自動選定する。

2026-10-13 / p186-addition-q1009: q1008 差画像で h1 の +3px 灰色の影が欠ける。sharp text-shadow の一般的な有限部分 scope を [p186](phase186/phase.md) に限定し、最大90分 q1009-i01 を選定。p185 clearance と p100全画面条件は不変。

2026-10-13 / q1009-p186-clearance: [p186](phase186/phase.md) で単一 sharp text-shadow を追加。局所 paint fixture と CSS rule44/44、build warning0。固定 Acid3 score100/100・差1701/480000（2451減）、Acid2差1865不変。[証拠](phase186/evidence/20261013-q1009/report.json)。p186 の有限部分 scope cleared。残差801+900、p100全画面差0とWS074受入は未達。次の最大90分 Queue を自動選定する。

2026-10-13 / p187-addition-q1010: q1009 差の紫の第6箱右端900画素は `padding-right:-10px` の受理に対応。[p187](phase187/phase.md) に直接負の padding 無効化と局所試験を限定し、最大90分 q1010-i01 を選定。p186 clearance と p100全画面条件は不変。

2026-10-13 / q1010-p187-clearance: [p187](phase187/phase.md) で直接負の padding を無効宣言として先行値維持。fixture 修正前 FAIL→修正後 PASS、CSS rule44/44、build warning0。固定 Acid3 score100/100・差801/480000（900減）、Acid2差1865不変。[証拠](phase187/evidence/20261013-q1010/report.json)。p187 cleared。残る左上リンク801画素、p100全画面差0とWS074受入は未達。次の最大90分 Queue を自動選定する。

2026-10-13 / p188-addition-q1011: q1010差801は左上の訪問リンク文字と右上20×20疑似要素（後者は paint_color 初期化）に分解。[p188](phase188/phase.md) に同一ページで観測した訪問先の :visited と描画色、privacy の局所試験を限定し、最大90分 q1011-i01 をユーザーの自動反復指示で選定。p187 clearance は不変。

2026-10-13 / q1011-p188-clearance: [p188](phase188/phase.md) で同一 origin 子 frame の成功 URL に限定した訪問色を実装。局所 fixture6/6、frame load52/52、CSS rule44/44、build warning0。固定 Acid3 score100/100・inventory100/100・例外0・差 **0/480000**（100%一致、q1010から801減）、Acid2差1865/120000不変。[証拠](phase188/evidence/20261013-q1011/report.json)。ユーザーの Acid3 目標達成。p100 whole は Acid2 exact と target gate 未確認のため planned、WS074 は他の受入が残り incomplete。main 統合・GitHub publication・push は保留。

2026-10-13 / q1011-main-merge: ユーザーのマージ指示で WS074 の145 file を `main` commit `1f9cc35ca` に取り込んだ。main host build warning/error0、visited-link fixture6/6、固定 Acid3 score100/100・inventory100/100・例外0・差 **0/480000** を再確認。[main再計測](phase188/evidence/20261013-merge-main/report.json)。Acid2差1865/120000不変。p188 cleared、p100 whole planned、WS074 incomplete を維持。`master.md` 未変更、push/GitHub publication保留。

Event ws074-p190-q1062-outcome-20261014: [p190](phase190/phase.md) q1062-i01 partial cleared。固定静的1,561 IDは662 local PASS/899 pixel FAIL/0 error/0 timeout、q1060比2 FAIL→PASS/0 PASS→FAIL。[証拠](phase190/evidence/20261014-q1062/README.md)。p190 whole/p173全3,805件100%は未達。共有投影/GitHub publication保留。

Event ws074-p189-q1063-outcome-20261014: [p189](phase189/phase.md) q1063-i01 partial cleared。単一HTTP候補1,455件中最初の128 IDを測り18 PASS/110非PASS。[証拠](phase189/evidence/20261014-q1063/README.md)。p189 whole/p173全3,805件100%未達。共有投影/GitHub publication保留。

Event ws074-p189-q1064-outcome-20261014: [p189](phase189/phase.md) q1064-i01 partial cleared。現行単一HTTP候補全1,455 IDは160 PASS/1,283非PASS、環境未対応328 ID。[証拠](phase189/evidence/20261014-q1064/README.md)。p189 whole/p173全3,805件100%未達。共有投影/GitHub publication保留。

Event ws074-p209-addition-q1065-20261014: [p209](phase209/phase.md)を追加。p189 q1064のCSS Grid no_completion280件に共通の`document.fonts.ready`欠落を一般修正する。q1065は固定3 IDと初期フォント待機に限定し、p189/p173分母を維持。共有Master/Queue/GitHub publication保留。

Event ws074-p209-q1065-outcome-20261014: [p209](phase209/phase.md) q1065-i01は初期ready部分scope cleared、whole uncleared。固定Grid3 IDはcompletion取得、局所/host・warning0 build・Acid3差0。[証拠](phase209/evidence/20261014-q1065/README.md)。既定runner font条件と280件再測定、p173全3,805件100%未達。共有投影/GitHub publication保留。

Event ws074-p189-q1066-outcome-20261014: [p189](phase189/phase.md) q1066-i01 partial cleared。旧CSS Grid no_completion280 IDは19 PASS/260 FAIL/1再現SIGSEGV、279 completion。[証拠](phase189/evidence/20261014-q1066/README.md)。p189 whole/p173全3,805件100%未達。共有投影/GitHub publication保留。

Event ws074-p210-addition-q1067-20261014: [p210](phase210/phase.md)を追加。q1066で同条件2回再現したGrid1 IDのSIGSEGVをbrowser一般欠陥として切り分ける。p189/p173分母は不変、共有Bug Board投影はQ1競合回避で保留。

Event ws074-p210-q1067-clear-20261014: [p210](phase210/phase.md) q1067-i01/whole cleared。pre-fix ASanで手動realm pointerをGC large cellと誤認するheap-buffer-overflowを確認、一般修正後固定WPT通常3/3・ASan1/1 completion。[証拠](phase210/evidence/20261014-q1067/README.md)。host-heap31/31・Acid3差0、p189/p173全件100%未達。共有投影/GitHub publication保留。

Event ws074-p189-q1068-outcome-20261014: [p189](phase189/phase.md) q1068-i01 partial cleared。固定font同条件1,455 IDは223 PASS/1,232非PASS、旧PASS→FAIL19候補。[証拠](phase189/evidence/20261014-q1068/README.md)。環境未対応328、p189 whole/p173全3,805件100%未達。共有投影/GitHub publication保留。

Event ws074-p189-q1069-outcome-20261014: [p189](phase189/phase.md) q1069-i01 partial cleared。旧PASS→FAIL19件はfont未指定の欠落geometryに依存、固定font FAILが実際の未達。[証拠](phase189/evidence/20261014-q1069/README.md)。最新223 PASS/1,232非PASS、p189 whole/p173全3,805件100%未達。共有投影/GitHub publication保留。

Event ws074-p211-addition-and-q1070-clear-20261014: [p211](phase211/phase.md)を追加し、固定GridのLTR明示幅項目のinline-axis配置を担当。q1070-i01/p211 whole cleared、代表WPT subtest旧4/18→9/18 PASS・旧PASS後退0、関連静的57件旧15 PASS維持、Acid3画素差0。[証拠](phase211/evidence/20261014-q1070/README.md)。RTL/auto幅等とp189/p190/p173全件100%は未達。共有Master/Queue/GitHub publication保留。

Event ws074-p212-addition-and-q1071-clear-20261014: [p212](phase212/phase.md)を追加し、RTL Grid列配置とitem self-alignmentを担当。q1071-i01/p212 whole cleared、固定代表WPT9/18→18/18 subtest PASS・1 ID whole FAIL→PASS、関連90静的旧18 PASSと全Grid529静的旧123 PASS維持。[証拠](phase212/evidence/20261014-q1071/README.md)。中間の旧PASS1件217差画素はauto列端数割当で回復。Acid3画素差0。p189/p190/p173全件100%は未達。共有Master/Queue/GitHub publication保留。

Event ws074-p189-q1072-outcome-20261014: [p189](phase189/phase.md) q1072-i01 partial cleared。固定Grid testharness502件は旧41 PASS/429 FAIL/32 no_output→43 PASS/427 FAIL/32 no_output、旧PASS後退0、subtest PASS+32。[証拠](phase189/evidence/20261014-q1072/README.md)。no_output32件はmodule scriptを含む。p189/p190/p173全件100%未達。共有Master/Queue/GitHub publication保留。

Event ws074-p213-addition-revision-and-q1073-clear-20261014: [p213](phase213/phase.md)を追加し、Grid横書きauto幅の非stretch self-alignmentを担当。最初の固定静的529件に旧PASS→FAIL3件を検出し、auto inline margin残余配分とvertical-rl子項目除外を有限範囲に記録して修正。[証拠](phase213/evidence/20261014-q1073/README.md)では代表testharness1 ID whole FAIL→PASS、局所44/44、静的旧123 PASS全維持・1 FAIL→PASS、Acid3差0。q1073-i01/p213 whole cleared。p189/p190/p173全件100%は未達、共有Master/Queue/GitHub publication保留。

Event ws074-p189-q1074-outcome-20261014: [p189](phase189/phase.md) q1074-i01 partial cleared。固定Grid testharness502 IDは43 PASS/427 FAIL/32 no_output→57 PASS/413 FAIL/32 no_output、14 whole FAIL→PASS/0 PASS→FAIL、subtest PASS+42。[証拠](phase189/evidence/20261014-q1074/README.md)。p189/p190/p173全件100%未達、共有Master/Queue/GitHub publication保留。

Event ws074-p214-addition-and-q1075-clear-20261014: [p214](phase214/phase.md)を追加し、Grid fit-contentのmin-content下限を内部既存測定と共有。q1075-i01/p214 whole cleared、固定幅変化14 WPT中4 whole FAIL→PASS・旧PASS後退0、静的Grid529旧124 PASS全維持、Acid3差0。[証拠](phase214/evidence/20261014-q1075/README.md)。残4件はreplaced image固有、p189/p190/p173全件100%未達、共有Master/Queue/GitHub publication保留。

Event ws074-p215-addition-and-q1076-clear-20261014: [p215](phase215/phase.md)を追加し、Grid replaced画像のnormal自然寸法と明示stretch/比率寸法を担当。q1076-i01/p215 whole cleared、固定幅変化14 WPTは10→14 PASS、静的Grid529旧124 PASS・Flex10 PASS維持、Acid3差0。[証拠](phase215/evidence/20261014-q1076/README.md)。p189/p190/p173全件100%未達、共有Master/Queue/GitHub publication保留。

Event ws074-p189-q1077-outcome-20261014: [p189](phase189/phase.md) q1077-i01 partial cleared。固定Grid testharness502 IDは57 PASS/413 FAIL/32 no_output→71 PASS/399 FAIL/32 no_output、14 whole FAIL→PASS/0 PASS→FAIL、subtest PASS+26。[証拠](phase189/evidence/20261014-q1077/README.md)。p189/p190/p173全件100%未達、共有Master/Queue/GitHub publication保留。

Event ws074-p189-q1078-outcome-and-p216-addition-20261014: [p189](phase189/phase.md) q1078-i01 partial cleared。固定Grid no_output32 ID全て同じmodule named import/共通export、HTMLはmodule typeをskip、JS compilerはimport/export未対応。[証拠](phase189/evidence/20261014-q1078/README.md)。[p216](phase216/phase.md)を追加しmodule実行を有限段階に分ける。p189/p190/p173全件100%未達、共有Master/Queue/GitHub publication保留。

Event ws074-p216-q1079-partial-clear-20261014: [p216](phase216/phase.md) q1079-i01は単独moduleの宣言scopeを修復。[証拠](phase216/evidence/20261014-q1079/README.md)。局所fixture exact、classic JS14/14、warning0 build、Acid3差0。q1079部分cleared、p216 wholeはimport/export・HTML load未達でuncleared。p189/p190/p173全件100%未達、共有投影/GitHub publication保留。

Event ws074-p216-q1080-partial-clear-20261014: [p216](phase216/phase.md) q1080-i01はnamed export宣言5種をmodule scope/hoist/評価に接続。[証拠](phase216/evidence/20261014-q1080/README.md)。固定WPT共通support module単独評価exit0、局所fixture exact、classic JS14/14、warning0 build、Acid3差0。q1080部分cleared、p216 wholeはmodule namespace/import/HTML未達。p189/p190/p173全件100%未達、共有投影/GitHub publication保留。

Event ws074-p216-q1081-partial-clear-20261014: [p216](phase216/phase.md) q1081-i01はVM env slot/GC・module間接binding・HTML deferred queueの内部境界と次の有限実装順を確定。[設計証拠](phase216/evidence/20261014-q1081/README.md)。q1081部分cleared、p216 whole/p189/p190/p173全件100%未達。共有投影/GitHub publication保留。

Event ws074-p216-q1082-partial-clear-20261014: [p216](phase216/phase.md) q1082-i01はGC追跡stable export envと内部module APIを追加。[証拠](phase216/evidence/20261014-q1082/README.md)。exported closureのcounter更新1→2をGC後もlive読取、固定WPT共通support sourceの`runTests`抽出PASS。classic JS14/14、warning0 build、Acid3差0。q1082部分cleared、p216 whole/p189/p190/p173全件100%未達、共有投影/GitHub publication保留。

Event ws074-p216-q1083-partial-clear-20261014: [p216](phase216/phase.md) q1083-i01はnamed importをGC追跡indirect bindingでexportへlink。[証拠](phase216/evidence/20261014-q1083/README.md)。q1081内部設計のlate link改訂、TDZ/代入/重複拒否、target解放+強制GC後のlive counter1→2、classic JS14/14、warning0 build、Acid3差0。q1083部分cleared、p216 whole/p189/p190/p173全件100%未達、共有投影/GitHub publication保留。

Event ws074-p216-q1084-partial-clear-20261014: [p216](phase216/phase.md) q1084-i01はimport metadataの安定列挙とspecifier一致linkを追加。[証拠](phase216/evidence/20261014-q1084/README.md)。複数specifier/誤指定子/live read、classic JS14/14、warning0 build、Acid3差0をPASS。q1084部分cleared、p216 whole/p189/p190/p173全件100%未達、共有投影/GitHub publication保留。

Event ws074-p216-q1085-partial-clear-20261014: [p216](phase216/phase.md) q1085-i01はcanonical URL cacheの内部module graphと依存link/evaluateを追加。[証拠](phase216/evidence/20261014-q1085/README.md)。相対URL/共有依存一度限り/順序/missing export/cycle、classic JS14/14、warning0 build、Acid3差0をPASS。q1085部分cleared、p216 whole/p189/p190/p173全件100%未達、共有投影/GitHub publication保留。

Event ws074-p216-q1086-partial-clear-and-p189-followup-20261014: [p216](phase216/phase.md) q1086-i01でparser-inserted inline moduleをparse後/DCL前にgraph実行。[証拠](phase216/evidence/20261014-q1086/README.md)。旧Grid no_output32 IDと同一集合を固定WPT/3font条件で全32 PASS・各100 subtestへ移行。[p189](phase189/phase.md)にもfollow-up記録。classic JS14/14、warning0 build、Acid3差0。q1086部分cleared、p216 whole/p189/p190/p173全件100%未達。共有投影/GitHub publication保留。

Event ws074-p189-q1087-partial-clear-20261014: [p189](phase189/phase.md) q1087-i01は固定Grid502 IDを全再測定。[証拠](phase189/evidence/20261014-q1087/README.md)。103 PASS/399 FAIL/no_output0、旧no_output32のみPASSへ移行、旧PASS後退0。raw/checkpoint全502照合。q1087部分cleared、p189 whole/p190/p173全件100%未達。共有投影/GitHub publication保留。

Event ws074-p217-addition-20261014: [p217](phase217/phase.md)を追加。q1087固定Grid399 FAILのうちalignment206件を最大群として一般欠陥から調査し、次の有限修正と再測定につなぐ。p189/p173の分母・受け入れは不変。共有投影/GitHub publication保留。

Event ws074-p217-q1088-partial-clear-20261014: [p217](phase217/phase.md) q1088-i01はalignment FAIL206件を分類し、絶対配置grid子のstatic area原点、縦書き強制stretch、`grid` shorthand欠落を別欠陥として切り分け。[証拠](phase217/evidence/20261014-q1088/README.md)。最小HTML2件で再現、次の有限修正候補を限定。q1088部分cleared、p217 whole/p189/p190/p173全件100%未達。共有投影/GitHub publication保留。

Event ws074-p217-q1089-partial-clear-20261014: [p217](phase217/phase.md) q1089-i01は水平LTRの有効明示area絶対子のstatic原点を設定。[証拠](phase217/evidence/20261014-q1089/README.md)。固定WPT代表2件の水平subtestが各FAIL→PASS、wholeはRTL/縦書き未達。旧PASS module代表維持、classic JS14/14、warning0 build、Acid3差0。q1089部分cleared、p217 whole/p189/p190/p173全件100%未達。共有投影/GitHub publication保留。

Event ws074-p217-q1090-partial-clear-20261014: [p217](phase217/phase.md) q1090-i01はRTL絶対子の固定20px/auto24px最小HTMLで実x8対期待x388/384を記録。[証拠](phase217/evidence/20261014-q1090/README.md)。Gridが幅確定前に左端static_xを設定し、position_placeが幅確定後にも左端として使う。次はprivate right-edge metadataでRTL startだけ修正する。q1090部分cleared、p217 whole/p189/p190/p173全件100%未達。共有投影/GitHub publication保留。

Event ws074-p217-q1091-partial-clear-20261014: [p217](phase217/phase.md) q1091-i01はRTL normal余白と幅後の右端static配置を修正。[証拠](phase217/evidence/20261014-q1091/README.md)。固定WPT代表2件の水平RTL subtest各FAIL→PASS、wholeは縦書き2件FAIL。最小RTL5件、旧PASS代表、Grid48/48、classic JS14/14、warning0 build、Acid3差0維持。q1091部分cleared、p217 whole/p189/p190/p173全件100%未達。共有投影/GitHub publication保留。

Event ws074-p217-q1092-partial-clear-20261014: [p217](phase217/phase.md) q1092-i01は縦書き固定代表のwidth期待10/実60と最小Gridのin-flow100×13・absolute24×13を記録。[証拠](phase217/evidence/20261014-q1092/README.md)。inline文字組みは横のまま、Gridの列/行は物理軸、absolute static areaも水平限定。段階的vertical実装を要する。q1092部分cleared、p217 whole/p189/p190/p173全件100%未達。共有投影/GitHub publication保留。

Event ws074-p189-q1093-partial-clear-20261014: [p189](phase189/phase.md) q1093-i01で固定Grid502を同条件全再測定し103 PASS/399 FAIL/no_output0。[証拠](phase189/evidence/20261014-q1093/README.md)。q1087からwhole変化0、旧PASS後退0、13件22 subtest FAIL→PASS・逆0。全502 raw/checkpoint/ID/hash一致。q1093部分cleared、p189 whole/p173全3,805件100%未達。共有投影/GitHub publication保留。

Event ws074-p218-addition-20261014: [p218](phase218/phase.md)を追加。q1093固定Grid399 FAILのshorthand使用はalignment/grid-definition/parsingにまたがるため独立して調査・修正する。p217のalignment受け入れやp189/p173分母は変えない。q1094読取調査を開始。共有投影/GitHub publication保留。

Event ws074-p218-q1094-partial-clear-and-p217-dependency-20261014: [p218](phase218/phase.md) q1094-i01は固定source字面83 FAILを上限とし、静的HTMLでshorthand未認識・longhand正常を再現。[証拠](phase218/evidence/20261014-q1094/README.md)。最初は明示行/列構文をatomic展開、implicit/CSSOM等は後続。p217 alignment style-changesのtrack sizingのみp218該当出力に依存するが、p217受け入れや他alignment作業は不変。[p217変更](phase217/phase.md)。q1094部分cleared、p218/p217/p189/p173全件100%未達。共有投影/GitHub publication保留。

Event ws074-p218-q1095-partial-clear-20261014: [p218](phase218/phase.md) q1095-i01は既存track-listの明示`grid: rows / columns`をatomic展開。[証拠](phase218/evidence/20261014-q1095/README.md)。最小12 geometry、CSS rule51/CSSOM12/mutation12、Grid48、JS14、warning0 build、Acid3差0。固定style-changes 0→3/6 subtest PASS、wholeはbaseline差でFAIL、旧PASS代表維持。p217のshorthand track sizing出力は利用可能だがbaseline等の受け入れは未達。q1095部分cleared、p218/p217/p189/p173全件100%未達。共有投影/GitHub publication保留。

Event ws074-p219-addition-20261014: [p219](phase219/phase.md)を追加。q1095でstyle-changes-001のtrack幅が回復してもempty子baseline offsetが残る。baseline名43 FAILは症状上限で同一原因とは断定しない。p217 alignment206受け入れとp189/p173分母不変。共有投影/GitHub publication保留。

Event ws074-p219-q1096-partial-clear-and-p217-output-20261014: [p219](phase219/phase.md) q1096-i01で空Grid子の固定行合成baselineを実装。[証拠](phase219/evidence/20261014-q1096/README.md)。固定style-changes-001は3/6→6/6 subtest、whole FAIL→PASS。p217 style-changes向け出力を追加したが、残205 alignmentとp219内容line/auto行/縦書きは未達。[p217更新](phase217/phase.md)。旧PASS、warning0 build、Acid3差0維持。共有投影/GitHub publication保留。

Event ws074-p219-q1097-partial-clear-20261014: [p219](phase219/phase.md) q1097-i01は固定style-changes8 IDを同条件測定し、0→2 whole PASS/20 FAIL→PASS subtest。[証拠](phase219/evidence/20261014-q1097/README.md)。003/004はvertical-rl水平offset、005/006は文字line縦baseline、007/008はvertical-lr文字幅/offset。q1097部分cleared、p219/p217/p189/p173全件100%未達、共有投影/GitHub publication保留。

Event ws074-p219-q1098-partial-clear-20261014: [p219](phase219/phase.md) q1098-i01は固定pixel行の水平Grid直接inline text先頭line baselineをempty合成baseline groupに追加。[証拠](phase219/evidence/20261014-q1098/README.md)。固定style-changes005/006各3/6→6/6 subtest・whole PASS、旧001/002と100 subtest PASS維持。host回帰・warning0 build、Acid3差0/Acid2差1865。q1098部分cleared、p219/p217/p189/p173全件100%未達。共有投影/GitHub publication保留。

Event ws074-p219-q1099-partial-clear-20261014: [p219](phase219/phase.md) q1099-i01は固定style-changes8 IDを同条件再測定し2→4 whole PASS、20→26/48 subtest PASS、旧PASS後退0。[証拠](phase219/evidence/20261014-q1099/README.md)。残4 IDは縦書きoffset/文字幅差。q1099部分cleared、p219/p217/p189/p173全件100%未達。共有投影/GitHub publication保留。

Event ws074-p217-q1100-partial-clear-20261014: [p217](phase217/phase.md) q1100-i01は残縦書き4 IDの失敗を空子offsetのみ2件と文字子width初回差2件へ分割。[証拠](phase217/evidence/20261014-q1100/README.md)。前者の限定baseline groupを次の独立修正候補とし、後者には縦文字layoutと論理幅が必要。q1100部分cleared、p217/p219/p189/p173全件100%未達。共有投影/GitHub publication保留。

Event ws074-p217-q1101-partial-clear-20261014: [p217](phase217/phase.md) q1101-i01で空vertical-rl子の固定1列baseline xを配置。[証拠](phase217/evidence/20261014-q1101/README.md)。固定003/004各1/6→6/6 subtest・whole FAIL→PASS、旧001/002/005/006と100-subtest PASS維持。host回帰・warning0 build、Acid3差0/Acid2差1865。q1101部分cleared、p217/p219/p189/p173全件100%未達。共有投影/GitHub publication保留。

Event ws074-p189-q1102-partial-clear-20261014: [p189](phase189/phase.md) q1102-i01は固定style-changes8 IDを同条件再測定し4→6 whole PASS、26→36/48 subtest PASS、旧PASS後退0。[証拠](phase189/evidence/20261014-q1102/README.md)。残007/008はvertical-lr文字子width差。q1102部分cleared、p189/p217/p219/p173全件100%未達。共有投影/GitHub publication保留。

Event ws074-p217-q1103-partial-clear-and-p220-add-20261014: [p217](phase217/phase.md) q1103-i01で固定007/008のvertical-lr幅100誤差をCSS/inline/paint/bounds/hitの横書き固定として切り分け。[証拠](phase217/evidence/20261014-q1103/README.md)。[p220](phase220/phase.md)を追加し、p217の該当文字alignmentだけ検証済み縦inline geometry出力に依存させる。q1103部分cleared、p217/p220/p189/p173全件100%未達。共有投影/GitHub publication保留。

Event ws074-p218-q1104-partial-clear-20261014: [p218](phase218/phase.md) q1104-i01で明示`grid-template`二側と単独`none`をatomic展開。[証拠](phase218/evidence/20261014-q1104/README.md)。最小14 geometry、旧回帰、warning0 build、Acid3差0/Acid2差1865。固定template-validは0→5/40 subtest PASS、whole FAIL、旧100-subtest代表PASS。q1104部分cleared、p218/p189/p173全件100%未達。共有投影/GitHub publication保留。

Event ws074-p189-q1105-partial-clear-20261014: [p189](phase189/phase.md) q1105-i01は固定Grid502件を同条件全再測定し103→110 PASS、399→392 FAIL、no_output0。[証拠](phase189/evidence/20261014-q1105/README.md)。7 whole FAIL→PASS・旧whole PASS後退0、subtest127改善/1後退（fr/gutter `.grid 17`）。502 ID/hash/checkpoint/gzip raw一致。q1105部分cleared、p189/p173全件100%未達。共有投影/GitHub publication保留。

Event ws074-p217-q1106-partial-clear-20261014: [p217](phase217/phase.md) q1106-i01は固定alignment残200 whole FAILを分類し、初回width79/offsetLeft65/offsetTop37/height18/scrollWidth1。[証拠](phase217/evidence/20261014-q1106/README.md)。block-axis auto margin系列7件を独立修正候補とし、positioned/縦stretch/縦文字は別。q1106部分cleared、p217/p189/p173全件100%未達。共有投影/GitHub publication保留。

Event ws074-p217-q1107-partial-clear-20261014: [p217](phase217/phase.md) q1107-i01は水平Grid definite行のauto上下marginを配分。[証拠](phase217/evidence/20261014-q1107/README.md)。固定auto-margins7件全whole FAIL→PASS、計14 subtest改善/逆0。旧PASS・host回帰・warning0 build、Acid3差0/Acid2差1865。q1107部分cleared、p217/p189/p173全件100%未達。共有投影/GitHub publication保留。

Event ws074-p217-q1108-partial-clear-20261014: [p217](phase217/phase.md) q1108-i01はpositioned列軸17/行軸17の水平/縦失敗を分割。[証拠](phase217/evidence/20261014-q1108/README.md)。列軸002/005/007/009/011の水平10 subtestはend/center系`align-self`のoffsetTop差、縦68 subtestはp220等待ち。private area height/alignmentで列軸の限定修正を次候補にする。q1108部分cleared、p217/p189/p173全件100%未達。共有投影/GitHub publication保留。

Event ws074-p217-q1109-partial-clear-20261014: [p217](phase217/phase.md) q1109-i01は水平Gridの確定areaで絶対子のend/center系block軸整列を最終子高さ後に計算。[証拠](phase217/evidence/20261014-q1109/README.md)。固定列軸5件の横書き10 subtestがFAIL→PASS、縦書き10 subtestはwidth差でFAIL継続。旧PASS2件、Grid/CSS/JS、warning0 build、Acid3画素差0・Acid2差1865維持。q1109部分cleared、p217/p189/p173全件100%未達。共有投影/GitHub publication保留。

Event ws074-p217-q1110-partial-clear-20261014: [p217](phase217/phase.md) q1110-i01は水平Gridの確定areaで絶対子のend/center系inline軸整列を最終子幅後に計算。[証拠](phase217/evidence/20261014-q1110/README.md)。固定行軸4件の横書き8 subtestがFAIL→PASS、縦書き8 subtestはwidth差でFAIL継続。旧PASS2件、Grid/CSS/JS、warning0 build、Acid3画素差0・Acid2差1865維持。q1110部分cleared、p217/p189/p173全件100%未達。共有投影/GitHub publication保留。

Event ws074-p217-q1111-partial-clear-20261014: [p217](phase217/phase.md) q1111-i01は固定行軸003/004/008/010/011をq1110後に再測定。[証拠](phase217/evidence/20261014-q1111/README.md)。水平10 subtest中6 FAIL、self-endの011は2 PASS。残る物理left/rightと子方向self-startのarea内配置を次の有限修正に設定。縦書き10件は幅差、whole5件FAIL継続。browser/runner/WPT/ABI/Master/Acid無変更、p217/p189/p173全件100%未達。共有投影/GitHub publication保留。

Event ws074-p217-q1112-partial-clear-20261014: [p217](phase217/phase.md) q1112-i01は水平Grid絶対子のleft/right/self-startを物理area辺へ配置。[証拠](phase217/evidence/20261014-q1112/README.md)。固定行軸5件で横書き6 subtest FAIL→PASS、計10/10 PASS、縦書き10は幅差でFAIL継続。旧PASS2件、Grid/CSS/JS、warning0 build、Acid3画素差0・Acid2差1865維持。q1112部分cleared、p217/p189/p173全件100%未達。共有投影/GitHub publication保留。

Event ws074-p217-q1113-partial-clear-20261014: [p217](phase217/phase.md) q1113-i01は固定column-axis/row-axis012–016計10件を再測定。[証拠](phase217/evidence/20261014-q1113/README.md)。横書き20/20・縦書き20/20 subtest FAIL。行軸は`auto / 1`のauto境界を先頭trackへ潰してcontent shiftを誤適用、列軸はblock軸`align-content:center`のrow shift未伝搬が主な独立原因。列軸RTLのoffsetLeft大差も別調査。次は行軸auto境界の有限修正。browser/runner/WPT/ABI/Master/Acid無変更、p217/p189/p173全件100%未達。共有投影/GitHub publication保留。

Event ws074-p217-q1114-partial-clear-20261014: [p217](phase217/phase.md) q1114-i01は水平Grid絶対子の`grid-column:auto / 1`でauto始端をpadding辺へ設定。[証拠](phase217/evidence/20261014-q1114/README.md)。最小LTR/RTL start/end/centerと明示線control PASS。固定行軸012–016の水平最初の100px差が消え、次の子/軸不一致へ進んだがwhole5件FAIL。旧PASS2件、Grid/CSS/JS、warning0 build、Acid3画素差0・Acid2差1865維持。q1114部分cleared、p217/p189/p173全件100%未達。共有投影/GitHub publication保留。

Event ws074-p217-q1115-partial-clear-20261014: [p217](phase217/phase.md) q1115-i01は水平Grid絶対子の`grid-column:2 / auto`でauto終端をpadding辺へ設定。[証拠](phase217/evidence/20261014-q1115/README.md)。最小LTR/RTL start/end/centerと明示線control PASS。固定行軸013/014/015の`2 / auto`不一致が次の子へ進み、012/016は別残差でwhole5件FAIL。旧PASS2件、Grid/CSS/JS、warning0 build、Acid3画素差0・Acid2差1865維持。q1115部分cleared、p217/p189/p173全件100%未達。共有投影/GitHub publication保留。

Event ws074-p217-q1116-partial-clear-20261014: [p217](phase217/phase.md) q1116-i01は水平Gridの絶対子で`grid-column:auto / auto`の全幅inline areaと明示row原点を設定。[証拠](phase217/evidence/20261014-q1116/README.md)。固定行軸012–016の横書き10 subtest FAIL→PASS、縦書き10はwidth差でFAIL継続。旧PASS2件、Grid/CSS/JS、warning0 build、Acid3画素差0・Acid2差1865維持。q1116部分cleared、p217/p189/p173全件100%未達。共有投影/GitHub publication保留。

Event ws074-p217-q1117-partial-clear-20261014: [p217](phase217/phase.md) q1117-i01は水平Grid固定高さの`align-content:center`で明示行とin-flow itemの原点を移動。[証拠](phase217/evidence/20261014-q1117/README.md)。最小auto/明示2行とin-flow PASS、固定列軸012/015の最初のoffsetTop差が次のoffsetLeft差へ進む。auto row 013/014/016とRTL x差は残りwhole5件FAIL。旧PASS2件、Grid/CSS/JS、warning0 build、Acid3画素差0・Acid2差1865維持。q1117部分cleared、p217/p189/p173全件100%未達。共有投影/GitHub publication保留。

Event ws074-p217-q1118-partial-clear-20261014: [p217](phase217/phase.md) q1118-i01は水平Gridのauto row始端/終端/両端をpadding端とshift済み明示lineから計算。[証拠](phase217/evidence/20261014-q1118/README.md)。固定列軸012–016の横書きLTR5 subtest FAIL→PASS、RTL5件は共通offsetLeft期待190/実948、縦書きもFAIL継続。旧PASS2件、Grid/CSS/JS、warning0 build、Acid3画素差0・Acid2差1865維持。q1118部分cleared、p217/p189/p173全件100%未達。共有投影/GitHub publication保留。

Event ws074-p217-q1119-partial-clear-20261014: [p217](phase217/phase.md) q1119-i01は固定RTL共通x差の親幅を追跡。[証拠](phase217/evidence/20261014-q1119/README.md)。`inline-grid`がCSS表示値でgridへ畳まれ、atomic/intrinsic width経路を通らず1008pxに広がる。WPTの250px期待との差がRTL offsetLeftを説明。次は表示値を分離しGrid計算を保持する有限実装。browser/runner/WPT/ABI/Master/Acid無変更、p217/p189/p173全件100%未達。共有投影/GitHub publication保留。

Event ws074-p217-q1120-partial-clear-20261014: [p217](phase217/phase.md) q1120-i01は`inline-grid`の独立display値・inline atomic幅・Gridレイアウト/CSSOMを保持。[証拠](phase217/evidence/20261014-q1120/README.md)。固定列軸012–016の横書きRTL5 subtestがFAIL→PASS、水平計10/10 PASS。縦書き10はFAIL継続、whole5件FAIL。旧PASS2件・host回帰・warning0 build、Acid3差0/Acid2差1865維持。q1120部分cleared、p217/p189/p173全件100%未達。共有投影/GitHub publication保留。

Event ws074-p220-q1121-partial-clear-20261014: [p220](phase220/phase.md) q1121-i01は固定列軸012–016縦書きの初回幅10/実60を非Grid最小ページと既存Grid fixtureで切り分け。[証拠](phase220/evidence/20261014-q1121/README.md)。非Grid縦書き絶対子も36×10pxで、inline line/paint/bounds/hit/dumpが横書き座標のまま。次の有限実装は非Grid vertical-lrの単一非折返し文字runに絞る。browser/runner/WPT/ABI/Master/Acid無変更、p220/p217/p189/p173全件100%未達。共有投影/GitHub publication保留。

Event ws074-p220-q1122-partial-clear-20261014: [p220](phase220/phase.md) q1122-i01はCSS `text-orientation`の3値・継承/CSS-wide・CSSOMと`writing-mode` CSSOMを追加。[証拠](phase220/evidence/20261014-q1122/README.md)。最小host/旧WPT PASS2件、CSS51/JS14/Grid48、warning0 build、Acid3差0/Acid2差1865維持。縦line/paintは後続、p220/p217/p189/p173全件100%未達。共有投影/GitHub publication保留。

Event ws074-p221-addition-20261014: [p221](phase221/phase.md)を追加。p189 q1105の固定Grid502残392 whole FAIL中、Grid parsing/配下35件をalignment/縦文字から分離して診断・段階修正する。固定41件全PASSをp221独自受け入れとし、p189/p173の分母と受け入れは維持。q1123有限読取scopeを選定、共有投影/GitHub publication保留。

Event ws074-p221-q1123-partial-clear-20261014: [p221](phase221/phase.md) q1123-i01は固定Grid parsing41件を全再測定。[証拠](phase221/evidence/20261014-q1123/README.md)。6 whole PASS/35 FAIL、336/930 subtest、no_output/process_error0。auto/areas属性欠落、track解析/直列化、area/shorthandが主群。次は`grid-template-rows` CSSOMの有限修正。browser/WPT/runner/ABI/Master/Acid無変更、p221/p189/p173全件100%未達。共有投影/GitHub publication保留。

Event ws074-p221-q1124-partial-clear-20261014: [p221](phase221/phase.md) q1124-i01は`grid-template-rows` computed CSSOMを既存列track serializerで返す。[証拠](phase221/evidence/20261014-q1124/README.md)。固定行側6件でsubtest6 FAIL→PASS・逆0、whole6件はFAIL継続。旧whole PASS3件、CSS51/JS14/Grid48、warning0 build、Acid3差0/Acid2差1865維持。q1124部分cleared、p221/p189/p173全件100%未達。共有投影/GitHub publication保留。

Event ws074-p221-q1125-partial-clear-20261014: [p221](phase221/phase.md) q1125-i01はGrid track負長/割合を直値・minmax・fit-content直値で拒否、既存calc有効値を保持。[証拠](phase221/evidence/20261014-q1125/README.md)。固定columns/rows-invalid各33→40/42 subtest、valid各19/34維持、whole4件FAIL。旧whole PASS3件、CSS51/JS14/Grid48、warning0 build、Acid3差0/Acid2差1865維持。q1125部分cleared、p221/p189/p173全件100%未達。共有投影/GitHub publication保留。

Event ws074-p221-q1126-partial-clear-20261014: [p221](phase221/phase.md) q1126-i01はGrid track行名の隣接群・予約名・不正token/未閉鎖を拒否。[証拠](phase221/evidence/20261014-q1126/README.md)。固定columns/rows-invalid各40→42/42 subtest、whole FAIL→PASS、valid各19/34と旧whole PASS3件維持。最小host・CSS51/JS14/Grid48・warning0 build、Acid3差0/Acid2差1865維持。q1126部分cleared、p221/p189/p173全件100%未達。共有投影/GitHub publication保留。

Event ws074-p221-q1127-partial-clear-20261014: [p221](phase221/phase.md) q1127-i01はGrid直値/minmaxの`calc()`長を受理し、負のcomputed trackを零へ丸め、minmax最小frを拒否。[証拠](phase221/evidence/20261014-q1127/README.md)。固定columns/rows-valid各19→27/34 subtest、invalid各42/42 whole PASSと旧whole PASS3件維持。最小host・CSS51/JS14/Grid48・warning0 build、Acid3差0/Acid2差1865維持。q1127部分cleared、p221/p189/p173全件100%未達。共有投影/GitHub publication保留。

Event ws074-p221-q1128-partial-clear-20261014: [p221](phase221/phase.md) q1128-i01は有効なGrid列/行templateのJS style値から空行名`[]`のみCSSOM直列化時に除去。[証拠](phase221/evidence/20261014-q1128/README.md)。固定columns/rows-valid各27→29/34 subtest、invalid各42/42 whole PASSと旧whole PASS3件維持。最小host・CSS51/JS14/Grid48・warning0 build、Acid3差0/Acid2差1865維持。q1128部分cleared、p221/p189/p173全件100%未達。共有投影/GitHub publication保留。

Event ws074-p221-q1129-partial-clear-20261014: [p221](phase221/phase.md) q1129-i01は固定Grid parsing41 IDを全件再測定。[証拠](phase221/evidence/20261014-q1129/README.md)。6→8 whole PASS/35→33 FAIL、336→399 PASS subtest/930→867 FAIL、後退0・無出力0。改善はinvalid2 whole、残はauto-*/areas・template computed行名/暗黙track・auto-repeat・shorthands。次は`grid-auto-flow`有限修正。browser/WPT/runner/ABI/Master/Acid無変更、q1129部分cleared、p221/p189/p173全件100%未達。共有投影/GitHub publication保留。

Event ws074-p221-q1130-partial-clear-20261014: [p221](phase221/phase.md) q1130-i01は`grid-auto-flow`のCSS parse/style/CSSOMとCSS-wide値を追加。[証拠](phase221/evidence/20261014-q1130/README.md)。固定valid/computed各0→7/7・whole FAIL→PASS、invalid3/3と旧whole PASS4件維持。最小host・CSS51/JS14/Grid48・warning0 build、Acid3差0/Acid2差1865。列/dense実配置は後続、q1130部分cleared、p221/p189/p173全件100%未達。共有投影/GitHub publication保留。

Event ws074-p222-addition-20261014: [p222](phase222/phase.md)を追加。q1130の`grid-auto-flow` CSS state後に残る行/列・dense自動配置を担当し、p221のparser/CSSOM責務と分離。q1131の行denseだけを有限選定。p189/p173分母・受け入れ不変、共有投影/GitHub publication保留。

Event ws074-p222-q1131-partial-clear-20261014: [p222](phase222/phase.md) q1131-i01はrow denseのcursor再開とsparse前進の維持を確認。[証拠](phase222/evidence/20261014-q1131/README.md)。最小2列穴埋めは(0,40)→(40,0)、固定implicit-tracks WPTはauto-columns幅不足で前後0/12 whole FAIL。旧whole PASS4件、CSS51/JS14/Grid48、warning0 build、Acid3差0/Acid2差1865維持。q1131部分cleared、p222/p189/p173全件100%未達。共有投影/GitHub publication保留。

Event ws074-p221-p222-q1132-partial-clear-20261014: [p221](phase221/phase.md) i01で単一auto/非負pxのauto-columns/rows CSS state、[p222](phase222/phase.md) i02で暗黙列/行sizingへ接続。[CSS証拠](phase221/evidence/20261014-q1132/README.md)、[配置証拠](phase222/evidence/20261014-q1132/README.md)。固定valid/computed各0→3 subtest、implicit-tracks placement0→6/12（row sparse/dense全6）、旧whole PASS2・host回帰・warning0 build・Acid3差0/Acid2差1865維持。q1132両部分cleared、p221/p222/p189/p173全件100%未達。共有投影/GitHub publication保留。

Event ws074-p222-q1133-partial-clear-20261014: [p222](phase222/phase.md) q1133-i01はcolumn/column denseの配置軸転置を追加。[証拠](phase222/evidence/20261014-q1133/README.md)。固定implicit-tracksは6→12/12・sparseは4→8/8 subtest、両whole FAIL→PASS、後退0。最小4 mode・旧whole PASS4件・CSS51/JS14/Grid48・warning0 build、Acid3差0/Acid2差1865維持。q1133部分cleared、p222/p189/p173全件100%未達。共有投影/GitHub publication保留。

Event ws074-p189-q1134-p223-p224-addition-20261014: [p189 q1134](phase189/evidence/20261014-q1134/README.md)固定Grid502全再測定で旧PASS2件が後退。空Gridの暗黙列がintrinsic幅へ誤算入される問題を[p223](phase223/phase.md)、行track animation値が元値のままの問題を[p224](phase224/phase.md)に分離。p189/p173分母・受け入れ不変。次はq1135で空Grid修正とanimation経路診断。共有投影/GitHub publication保留。

Event ws074-p223-p224-q1135-outcome-20261014: [p223 q1135-i01](phase223/evidence/20261014-q1135/README.md)で空Grid旧PASSを3→9/9に回復、暗黙track配置旧PASSとAcid3差0維持。[p224 q1135-i02](phase224/evidence/20261014-q1135/README.md)で行track animation旧PASSはcomputed CSSOM空同士の偽陽性と確認。実animation機構は未実装。両部分cleared、p223/p224/p189/p173全件100%未達。共有投影/GitHub publication保留。

Event ws074-p221-q1136-outcome-20261014: [p221 q1136](phase221/evidence/20261014-q1136/README.md)で複数固定px/auto暗黙track循環を実装。固定valid各3→6/30、computed各3→4/25、invalid全PASS、旧whole PASS/Acid3差0維持。q1136部分cleared、p221/p189/p173全件100%未達。共有投影/GitHub publication保留。

Event ws074-p221-q1137-outcome-20261014: [p221 q1137](phase221/evidence/20261014-q1137/README.md)で非px固定長/割合/calc暗黙trackを受理。固定valid各6→10/30、computed各4→7/25、invalid全PASS、旧whole PASS/Acid3差0維持。q1137部分cleared、p221/p189/p173全件100%未達。共有投影/GitHub publication保留。

Event ws074-p221-q1138-outcome-20261014: [p221 q1138](phase221/evidence/20261014-q1138/README.md)で暗黙frの列・行配分を追加。固定valid各10→12/30、computed各7→9/25、invalid全PASS、旧whole PASS/Acid3差0維持。q1138部分cleared、p221/p189/p173全件100%未達。共有投影/GitHub publication保留。

Event ws074-p221-q1139-outcome-20261014: [p221 q1139](phase221/evidence/20261014-q1139/README.md)で負calc Grid trackのcomputed長を0pxにclamp。固定auto列/行computed各9→10/25、invalid全PASS、旧whole PASS/Acid3差0維持。q1139部分cleared、p221/p189/p173全件100%未達。共有投影/GitHub publication保留。

Event ws074-p189-q1140-outcome-20261014: [p189 q1140](phase189/evidence/20261014-q1140/README.md)で固定Grid502を全件同条件再測定。124→126 whole PASS、45 subtest改善、旧PASS/subtest後退0。ID/hash/checkpoint/raw全一致。p189部分cleared、p189/p173全件100%未達。共有投影/GitHub publication保留。

Event ws074-p217-q1141-outcome-20261014: [p217 q1141](phase217/evidence/20261014-q1141/README.md)で固定alignment残192 FAILの初回症状を全件分類（幅110/offsetLeft37/offsetTop26/高さ18/他1）。固定縦trackのGrid軸変換を次の一般修正候補、baselineは独立調査とした。q1141部分cleared、p217/p189/p173全件100%未達。共有投影/GitHub publication保留。

Event ws074-p217-p225-addition-20261014: [p217 q1141](phase217/evidence/20261014-q1141/README.md)の固定縦Grid軸不足を[p225](phase225/phase.md)に分離。初回はvertical-lr/LTR/明示固定track/simple in-flow item、文字itemはp220 q1122出力に依存。p217は一般alignment受け入れを維持、p217/p189/p173分母不変。共有投影/GitHub publication保留。

Event ws074-p225-q1142-uncleared-20261014: [p225 q1142](phase225/evidence/20261014-q1142/README.md)は空2×2の縦Grid軸変換を達成したが、文字幅と固定WPT2件がFAILで試行uncleared。p220 q1122はCSS stateのみでgeometry出力ではないと依存記述を訂正。非Grid縦文字line/fragmentをp220で先に検証し、p225を再試行する。Grid/host・Acid3差0後退なし、p217/p189/p173分母と受け入れ不変。共有投影/GitHub publication保留。

Event ws074-p189-q1143-outcome-20261014: [p189 q1143](phase189/evidence/20261014-q1143/README.md)でq1142後の固定Grid502を全件同条件再測定。q1140比whole/subtest状態変化0、126 PASS/376 FAIL、旧PASS後退0、ID/hash/checkpoint/raw全一致。縦書き001初回文字幅実100→60だがFAIL継続。q1143部分cleared、p189/p173全件100%未達。共有投影/GitHub publication保留。

Event ws074-p220-p225-q1144-outcome-20261014: [p220 q1144](phase220/evidence/20261014-q1144/README.md)で限定vertical-lr inline geometryの最小span10×36、複数line20×36、DOM hit/dumpを確認。固定Grid stretch001/002が両whole PASS、旧PASS2件・host回帰・Acid3差0維持。[p225](phase225/phase.md)のq1142 uncleared履歴は維持、他vertical Grid/paint未検証のためwhole未達。p220/p217/p189/p173全件受け入れ不変。共有投影/GitHub publication保留。

Event ws074-p189-q1145-outcome-20261014: [p189 q1145](phase189/evidence/20261014-q1145/README.md)で固定Grid502を全件同条件再測定。126→130 whole PASS、4 subtest改善、旧PASS後退0。縦書きstretch001–004が増分、残005–016の構成をp225後続候補に分類。q1145部分cleared、p189/p173全件100%未達。共有投影/GitHub publication保留。

Event ws074-p225-q1146-outcome-20261014: [p225 q1146](phase225/evidence/20261014-q1146/README.md)で固定vertical-lr Gridのhorizontal-tb子を限定物理軸経路に通し、stretch005–008を4/4 whole PASS。旧001–004・host回帰・Acid3差0維持。q1146部分cleared、p225/p217/p189/p173全件100%未達。次は固定Grid502再測定と009–016可変track。共有投影/GitHub publication保留。

Event ws074-p189-q1147-outcome-20261014: [p189 q1147](phase189/evidence/20261014-q1147/README.md)で固定Grid502を全件同条件再測定。130→134 whole PASS、4 subtest改善、旧PASS後退0。増分vertical-lr stretch005–008、残009–016は明示Grid寸法/auto track。q1147部分cleared、p189/p173全件100%未達。共有投影/GitHub publication保留。

## 引き継ぎ（2026-10-15 夜、Q1、サーバの再起動の前）

- p235（`position: sticky` のページの scroll、子を持つ箱の `transform: scale`）は main に merge 済み（6e74bfd3d）、host のみ。B1 は p235 の後は未起動。
- Q1 の後始末の残り: `git worktree remove /home/awe/zedBSD-worktrees/b1-base`、b1 の build/b1p235 の wpt・wpt-base・a-*・b-*・acid*、build/ws074-suites/wpt/acid/zedbsd-acid*.html。
- 残り: inner scroll container、子つきの rotate・skew、flex item の aspect-ratio（31 件）、GPU の経路での sticky の確かめ。

Event ws074-animation-phase-ownership-reconciled-20261018: 既存[p224](phase224/phase.md)へGrid track interpolation/compositionのgoalを維持し、新[p248](phase248/phase.md)はnative timing/DOM lifecycleのprerequisite outputへ限定。重複した受け入れを整理、p248 → p224 → p246再検証の依存を明記。各Phaseへeventを配送。history/未達/固定分母とq14 implementation scopeは変更なし、共有projection pending。

Event ws074-p252-basis-plan-20261018: [p252](phase252/phase.md)をp240/p241からの既存basis残差の次候補として追加。固定computed/invalid/valid 3 IDのraw、native measurement/cache/style復元を設計へ記録。q14 scope/deadline/criteria不変、q14全件gate後の新Queueまでsource実装しない。関連両Phaseへeventを配送、共有投影pending。

Event ws074-p224-state-reconciled-20261018: [p224](phase224/phase.md)のactive item無し/whole未達に合わせin-progressの古いprojectionをunclearedへ訂正。q1135部分clearedのhistoryは保持、p248 → p224の順と新Queueによる再開条件は不変。共有projection pending。

Event ws074-q14-animation-fit-collapse-diagnosis-20261018: q14途中でGrid animation4 IDの16 callback偽陽性を追加可視化。[p250の独立before/after証拠](phase250/evidence/20261018-q14/animation-collapse-probe.html)でserializer collapseを直接確認し、p224/p248/p246へ自身の未達・resumeを記録。q14既定computed-only criteriaを変更せず、全件terminal gateまでclearance未判定。q11履歴は保持、共有投影pending。

Event ws074-q14-both-cleared-20261018: [p251](phase251/phase.md) linear percentage type/[p250](phase250/phase.md) computed fit bound own criteria cleared。source88713badd・[全件証拠](phase250/evidence/20261018-q14/README.md)。harness1491 whole489→491、old whole後退0、callback13改善/16 native animation偽陽性露出。static1561 whole839/pixel count維持、DOM33/33、host/target warning0、Acid非増加。確認済み1330/3805、未対応292/413/crash48正式条件を保持。p224/p248/p246やWS/target/p100未達はclearしない。次候補はp190 crash正式待機/raw retentionを優先しp252へ続く。両Phase/runner所有Phaseへevent配送、共有projection/remote close pending。

Event ws074-q15-crash-readiness-selected-20261018: [p190](phase190/phase.md)をretention→official wait protocolの最大60分Queueで実行。q14 archive/hash/98acae7a6 read-back後に選定、既知未確認48の正しい計測を[p252](phase252/phase.md)より優先。p252はq14 output verified/planned、両Phaseへ自身のresumeを記録。native C/WPT/public ABI/他runnerは変更せず、正式PASS数はactual確認まで加算しない。共有projection pending。

Event ws074-p190-q15-i01-cleared-20261018: [p190 retention証拠](phase190/evidence/20261018-q15-i01/README.md)。i01部分cleared・正式crash増分0。artifact/hash/checkpointのverified outputでi02 strict waitを開始、Phase/WS whole未達と共有projection pendingを保持。

Event ws074-q15-i02-uncleared-and-p209-redesign-20261018: [p190 q15](phase190/evidence/20261018-q15-i02/README.md)はparser8 pass/native positive6 FAILでi02 uncleared。fixed48未実行/正式PASS増分0、i01 retained artifactsはcleared。原因producerの[p209](phase209/phase.md)へpost-font eligible tasksを同じbudgetで処理する設計を追加し、p209 verified preflight → p190全controls → fixed48を明記。origin/foreign両Phaseへevent配送、q1065/q15履歴/1330固定PASSは保持。次Queue候補はnative producerとstrict48再試行、p252はその後。共有projection pending。

Event ws074-q16-native-producer-selected-20261018: [p209](phase209/phase.md)のpost-font eligible tasksと[p190](phase190/phase.md) strict48再検証を最大90分Queueで選定。q15 archive/d2cee8244 read-back済み。native preflight → controls → actual48を守り、元budget/ABI/WPT protocolを維持。global1491/static1561のlatest verified rawでold whole491/839を比較。両Phaseへ自身のdesign/resume eventを配送、full APIs/WS100%と共有projectionはpending。

Event ws074-q16-uncleared-p253-added-20261018: [q16証拠](phase209/evidence/20261018-q16/README.md)。producer独立7行/budget6/DOM34/C/Acidは通るが全15は14/15、attribute MutationObserver未対応で両item uncleared（i02 unstarted）。global1491/static1561はcombined final sourceへpendingとしてskipped、last verified1330/3805はq14でありcurrent candidate全体を未検証で維持としない。[p253](phase253/phase.md)を追加しp209/p190/p242へ自身の変更・再開条件を配送。q16 limited output → attributes → p209新attempt/control/global → actual48を新Queue候補とする。共有projection pending。

Event ws074-q17-attribute-combined-selected-20261018: [p253](phase253/phase.md)ordinary attribute records → [p209](phase209/phase.md)新attempt再検証 → [p190](phase190/phase.md) strict48を最大90分で選定。q16 hash/48704d92b read-back後に開始、各Phaseへ自身の設計/resumeを配送。namespace/Attr/multi-target/characterData等は未実装面として明示。latest full q14 rawをbeforeとするcombined final1491/static1561でold whole491/839を確認し、条件/分母/WPTを緩めない。共有projection pending。

Event ws074-p253-native-list-owner-design-20261018: p253新attribute recordではArrayのconstructor名を付けるhelperを使わず、native collection/NodeList brandとempty coreを実装する設計へ確定。source collection.cを同Phase内部providerとして追加、Radio/HTMLCollection既存brandを維持。active Window-owned ordinary setterがfirst partial、standalone/cross-doc/namespace/Attr/multi-target/full IDL等は未実装を明示。p242へ既存snapshot helperの限界も記録し、strict15/1491/static1561 gateは不変。共有projection pending。

Event ws074-p190-q17-strict48-partial-clear-20261018: [p190 evidence](phase190/evidence/20261018-q17/README.md)。controls15/15のverified output後、fixed48をstrict条件で全実行し39 PASS/9 readiness_missing、coverage partial cleared。9件はmodule top-level await4・CSS animationend1・ViewTransition API3・style/child transition event1へnative調査を残す。画像だけを成功扱いせず、current全体はq17 global1491待ち。静的1561は839維持/旧PASS後退0・全reference pixel unchanged。p190 whole/WS whole・共有projection pending。

Event ws074-q17-finished-20261018: [exact history](history/ws074-q20261018-17.md)・[global evidence](phase253/evidence/20261018-q17/README.md)。全harness1491はwhole491・callback11531 PASS/20226 FAIL/14 NOTRUNを維持し、whole/callback後退・改善・欠損・追加・順序変更0。全static1561は839 PASS/722 FAIL、全reference pixel metrics unchanged。strict48は39 PASS/9 readiness_missing。最終source f1b83fa8f/pair hash read-back一致、固定WPT tracked clean。確認済み1369/3805（35.978975%）、q14から39増。未実行292 harness/413 reftest・native crash依存9は残す。 全3 scoped item cleared、p190/p209/p253各whole uncleared・WS incompleteを保持。origin/producer/coverageへ個別event配送済み。次Queueはnative first-ready captureとscripted413を候補へ（まだ未選定）、shared projection pending。

Event ws074-p190-q18-selected-20261018: [Queue](queue.md)でpublic SDK試験clientのfirst-ready撮影→p190 scripted413 runnerを最大90分へ選定。q17 WIP c59655415/history hash read-back済み。必要output/controls/原source・DOM除去・query/fuzzy/graph/全raw保持をp190へ記録、prod engine/ABI不変。確認済み1369/3805を保持し、未検証分は加算しない。shared projection pending。

Event ws074-q18-uncleared-p196-redesign-20261018: [exact history](history/ws074-q20261018-18.md)・[evidence](phase190/evidence/20261018-q18/README.md)。first-ready clientはlate timer先取り防止/SDK/C/target成立、必須10対照中XHTMLのみFAIL。最上位XMLがscriptsを実行せずloadへ進むのでp196 output→p190同対照→413へ依存改訂。origin p190/foreign p196へ自身の変更とresumeを配送済み。両q18 item uncleared、q17 verified1369維持、shared projection pending。

Event ws074-q19-selected-redesign-20261018: [Queue](queue.md)でp196 strict validated XHTML classic inline producer→p190 same10 client controls→scripted413を最大90分/16:28 boundに選定。q18 WIP4090ecec6/history hash確認済み、origin/foreign両Phaseへ自身の設計/検証/resumeを配送。sourceはpage.cと既存試験3file、public ABI/他engine/shared projectionは保護。last verified1369は新final測定まで保持。

Event ws074-p190-q19-controls-output-20261018: native producerのChrome5行一致/self・next removal/GC安全性を確認、元10を含む22対照が通過。i02 consumer partial cleared・i03 scripted413へ着手。engine final pairを固定して全1491/static1561/strict48も並行検証中で、whole/i01を前倒しclearしない。shared projection pending。

Event ws074-q19-finished-20261018: [exact history](history/ws074-q20261018-19.md)・[producer/global](phase196/evidence/20261018-q19/README.md)・[scripted](phase190/evidence/20261018-q19-i03/README.md)。全1491 harnessは491 PASS・callback11531 PASS/20226 FAIL/14 NOTRUNを維持、whole/callback後退・追加・欠損・順序変更0。全static1561は839 PASS/722 FAIL・全reference pixel metrics unchanged。strict48は39 PASS/9 readiness_missingを全ID対応で維持。scripted413は全native attempt/568 capturesで82 PASS・73 pixel FAIL・242 unsupported_environment・16 readiness_missing。確認済み1451/3805（38.134034%）、新規計測PASS82増。harness未attempt292、環境/API/worker等の未達は分母に保持。 全3 partial cleared、p196 full XML scripting・p190環境/API/PASS等whole未達を保持、q18を後日成功で書換えない。次Queue選定前にharness未attempt292とenv unsupported242をinventoryし、source/WPT/boundsを維持して自走を継続する。shared projection/target full/p100 pending。

Event ws074-q20-selected-20261018: [p190](phase190/phase.md)でunsupported env242の6 static header filesを正確に配信→413再検証を最大60分へ選定。q19 aa0be0367/history hash read-back、pinned HTTP/duplicate/Content-Type契約を確認。prod source/pair不変・old82維持を必要とし未対応を隠さない。未attempt harness292の分類も保持、shared projection pending。

Event ws074-p190-q20-header-output-20261018: 29 controlsでpinned普通headersの実配信/適用ackと未対応非PASSを確認。q20-i01 partial cleared→i02同413再検証開始、source/旧1451はbyte同一で保持。shared projection pending。


Event ws074-q20-finished-20261018: [exact history](history/ws074-q20261018-20.md)・[証拠](phase190/evidence/20261018-q20/README.md)。普通のstatic response headersをpinned契約どおり配信し、独立HTTP/native 29 controlsを通過。同一native source/pairで固定scripted413全native attempt/805 captures、105 PASS・283 pixel FAIL・5 unsupported_environment・20 readiness_missing。旧82 PASS後退0、未対応242から23 PASS/210 FAIL/4 missingを新たに正しく判定。全388 edgeは無改変pinned comparatorと一致、共通594回のRGB照合は変化0。6 header sourcesはpinned git blob/hash・実配信ack/順序/値を検証。既存harness1491=491 PASS/static1561=839 PASS/strict48=39 PASSはq19のbyte同一source/pair証拠を再利用。確認済み1474/3805（38.738502%）、新規計測PASS23増。未attempt harness292・環境5・native未達は固定分母へ保持。 両partial cleared、p190 whole/API/environment/WS・target/p100未達とshared projection pendingを保持。次候補p252、user自走指示を有限scopeへ適用して継続。

Event ws074-p252-q21-selected-20261018: [Queue](queue.md)でp252 grammar/computed/content baseを最大90分に選定。q20verified1474を保持、native source変更後は全1491/static1561/strict48/scripted413を同final pairで再検証。p240/p241へ自身の依存event配送済み、whole/target/p100/shared projection pending。

Event ws074-q21-finished-20261018: [exact history](history/ws074-q20261018-21.md)・[証拠](phase252/evidence/20261018-q21/README.md)。flex-basis grammar/computed/content baseをnative修正。固定3 IDは27/27 callback PASS、2 whole/7 callback改善（parsing3は5、shorthandは2）。全1491 harnessは493 PASS（+2）・11538 PASS/20219 FAIL/14 NOTRUN、old whole/callback後退・欠損・追加・順序変更0。全static1561は844 PASS/717 FAIL（+5）・old839後退0、画素変更は改善5 IDだけ。5件の旧/新16 capturesを無改変pinned comparatorでも全FAIL→PASS確認。scripted413は105 PASS/283 FAIL/5 unsupported/20 missing・805 captures全RGB不変/388 edge primary一致、strict48は39 PASS/9 missing維持。確認済み1481/3805（38.922470%）、genuine native whole改善7。未attempt harness292と環境/API/native残差は分母保持。 host scoped partial cleared、p252 wholeの受け入れは今回のhost partialだけで認定せずuncleared。replaced/一般intrinsic/縦書き・negative inner base等の既存別未実装面とtarget full link/runtimeも保持。p240/p241/p189/p190/WS全体やp100を先取りclearしない。指定calc canonical順序の既存差は未修正。shared projection/GitHub pending、push無し。

Event ws074-p189-q22-selected-20261018: [Queue](queue.md)でharness HTTP shutdown待機のpoll1行→同1491の全callback同値と実測elapsedを最大45分へ選定。native C/全基準/未対応・確認済み1481は不変。q21 cc65dc3f6/history hash確認済み、shared projection pending。

Event ws074-q22-finished-20261018: [exact history](history/ws074-q20261018-22.md)・[証拠](phase189/evidence/20261018-q22/README.md)。harness HTTP serve_foreverの停止確認pollだけを0.5→0.01秒へ変更。既存分類5対照通過、同じq21 frozen pairで全1491を実測し全ID/whole/returncode/全callback・completion status/項目順/メッセージが完全同値。493 PASS・11538 PASS/20219 FAIL/14 NOTRUN維持、後退/改善/欠損/追加/順序差/メッセージ差0。実測920.9→226.8秒（4.0604倍、694.1秒短縮）。raw/gzip/checkpoint/全tar member/read-back、fixed corpus/font/source/pair hash一致。 i01 scoped partial cleared、p189 wholeの未attempt292/環境/API/全1783は未達。browser C/WPT/他runner/判定基準/分母は不変。確認済み1481/3805（38.922470%）、PASS増分0。static/scripted/crash/DOM/C/Acidはbyte同一pairのq21証拠を再利用し不要な再測定を省略。shared projection/GitHub/target full/p100/WS未達、push無し。

Event ws074-p189-q23-selected-20261018: [Queue](queue.md)でpinned Window/Any wrapper/META/内部WebIDL rewriteとnew generated専用header provider → 全1508/旧1491比較を最大90分へ選定。native API/源/分母不変、確認済み1481を実測まで保持。q22 6e6cff13d/history hash確認済み、shared projection pending。


Event ws074-p189-q23-wrapper-output-20261018: primary/native13/13と既存pure分類5/5を確認。Window/Any/global/script順/query/currentScript/log、実HTTP headers/原body/WebIDL内部rewrite、native failure/unsupported static fallback・duplicate/zero境界を検証。fixture準備の型注釈/CSS参照/duplicate入力時点を訂正して再実行、元のnegative期待は保持。eligible1508=old1491+new17、残275未attemptをinventory/hashで確認。i01 scoped partial cleared→i02全1508実測開始、whole p189/WSは未達。shared projection pending。

Event ws074-q23-performance-prerequisite-redesign-20261018: [p189 q23](phase189/evidence/20261018-q23/native-prerequisite-holdback.json)のraw新1 PASSはno-op mark/observerで前提が欠落し、Chrome/native独立probeで確認。rawは保持し確認済み1481へ加算しない。[p254](phase254/phase.md)実mark producer→[p255](phase255/phase.md)real observer→p189再評価の構造を追加、origin/新両Phaseへ自身のscope/resumeを配送。p077受け入れは当初のreadonly/empty-entry範囲として維持、現在q23のcoverage scope/期限/固定分母は不変。shared projection pending。

Event ws074-q23-finished-20261018: [exact history](history/ws074-q20261018-23.md)・[証拠](phase189/evidence/20261018-q23/README.md)。pinned Window/Any wrapper・META・WebIDL内部GET rewrite・generated専用ordinary headersを提供し、primary/native13＋既存分類5対照通過。全1508=old1491+new17を同じfrozen pairで実測、old1491は全ID/whole/returncode/callback・completion status/order/messages完全同値。追加17は全native attempt、raw1 PASS/10 FAIL/5 no_completion/1 harness_error、426 callbacks（2 PASS/424 FAIL）。全1783の未attempt292→275、native C/WPT/期待値/分母は不変。 新1 raw PASSは既存performance.markの記録なし・getEntries空・PerformanceObserver.observe no-opで入力が欠落しており、native/Chrome独立probeでprior/future 0/0対1/1とfuture通知の差を確認。元WPT結果は改変せずknown native prerequisite holdback1として確認済みに加算しない。raw fixed1482/3805、確認済み1481/3805（38.922470%）維持。 両item scoped coverage partial cleared、whole p189/WS・native APIs・target full/p100は未達。[p254](phase254/phase.md)実mark→[p255](phase255/phase.md)実observer→p189 holdback再評価の依存を追加。p077旧readonly/empty-entry scopeの受け入れは保持。source/pair/fonts/manifest/全ID/hash、全raw/stdout binary/生成HTML/原JS/resources/header provenance/controls tarをread-back、WPT tracked clean。shared projection/GitHub pending、push無し。


Event ws074-q23-clearance-invalidated-idl-20261018: q23 terminal17cd21137後のauditで、event-timing/storage-accessの2 IDが正規Gitにあるinterfaces/*.idlを疎checkout不在で404とし、idl_test setupで失敗していた。[proof](phase189/evidence/20261018-q23-followup/missing-idl-proof.json)。generated資源の忠実性基準未達のためq23 i01/i02のclearanceをinvalidateしcurrent attemptをunclearedへ訂正。old1491同値/他15入力/原report・原Outcome・全artifactを保持、Native bug修正やfalsePASS holdbackの解除とはしない。確認済み1481は不変。新finite QueueでGit原本のmissing IDL GETを提供・独立wire/expected404を検証→2 ID/全1508再確認して再開する。p254 native producerはそのverified baseline後へ延期、まだ未実装。shared projection pending。

Event ws074-q24-selected-20261018: q23 IDL input不足を訂正するp189新finite Queue q24（17:51 JSTまで）を開始。Git原IDL配信と独立wire検証→2 ID/全1508再確認。p254 native producerの着手はこの検証後。共有投影pending。

Event ws074-q24-finished-20261018: [history](history/ws074-q20261018-24.md)・[evidence](phase189/evidence/20261018-q24/README.md)。疎checkoutに無いinterfaces/*.idlをpinned Git原blobからgenerated専用GETで配信し、5種類/2 ID計6資源の原hash・query・MIME・header metadata不在と全rawを検証。独立wire/本物404/未配信Git資源nonPASS/metadata拒否を含む15 controls通過。全1508を235.8秒で再測定、全whole/status不変、IDL2以外1506の全callback/completion objectは完全同一（旧1491を含む）。IDL2のfetch errorは解消し、native JS compilerがyield*を未実装のため公式WebIDL2 parser実行に失敗する正しい残差へ到達。 raw harness494 PASS/958 FAIL/4 harness_error/26 no_completion/24 no_output/2 multiple、callback11540 PASS/20643 FAIL/14 NOTRUN。known native prerequisite holdback1は保持し確認済1481/3805（38.922470%）、未attempt275。native C/固定WPT/期待値/分母不変、static844/scripted105/crash39/DOM/C/Acidは同一q21 pairの証拠を再利用。 q24両scoped attempt cleared、q23元uncleared履歴は保持。whole p189/WS/p100/target fullは未達、shared projection/GitHub pending、push無し。次はp254 native mark producer、p255 observerでpositive通知を検証するまでheld PASSを確認済へ加算しない。yield*は次のVM候補としてp189に残す。

Event ws074-p254-q25-selected-20261018: p254 native mark producer host partialを[有限Queue q25](queue.md)へ選定（18:30 JSTまで）。q24 corrected1508を前提とし、managed記録/取得/clear/readonly/brand/GC lifetimeを通常APIで実装・検証。p255 consumer/held PASS解除は次のverified output後、whole WS/p100は未達。shared projection pending。

Event ws074-q25-finished-20261018: [history](history/ws074-q20261018-25.md)・[evidence](phase254/evidence/20261018-q25/README.md)。native PerformanceEntry/PerformanceMarkをprivate registryへ追加し、markの実記録/返値、constructor非buffering、readonly enumerable common fields/primitive detail/common4 toJSON、finite非負timestamp、required引数/予約名/例外、stable timestamp順のgetEntries/ByName/ByTypeとclearMarksを実装。native brandとmanaged payloadでprototype偽装を拒否し、receiverのactual relevant global/clock/prototypeを使用。38条件が独立Chromeと完全一致、buffer-only/saved entry/辞書Get・filter再入GC/retired child/cross-realm/allocation pressureのnative7条件通過。global公開前の既存helperのnewborn rootも補強。 最終source2d5db3785・frozen verified pairで全1508 harnessはq24比全whole/callback/completion/returncode完全同一、494 raw PASS維持。static1561は844 PASS/717 FAIL、全pixel metrics不変。scripted413は105 PASS/283 FAIL/5 unsupported/20 missing、805 captures全RGB不変。strict48は39 PASS/9 missing不変。DOM37/37・CSSOM12+12、host/target exact2 source warning0、Acid3得点/全inventory100・pixel差0、Acid2既存差873不変。 確認済1481/3805（38.922470%）維持、raw fixed1482。独立probeはprior/future entry各1に改善したがObserverはまだno-opで通知0、known held PASS1は解除しない。複雑detail clone/id/navigationId/measure/resource/event/workerは未実装、p254 wholeはuncleared、q25 host producer scoped itemだけcleared。p255はverified producer outputを前提に次のfinite Queueへ選定可能。p189未attempt275/p100/target full link/runtime/WS全件100%は未達、shared projection/GitHub pending、push無し。

Event ws074-p255-q26-selected-20261018: [q26](queue.md)でp254 named verified producerを受けるnative Observer/EntryList/実task/GCを18:49 JSTまで選定。positive通知と原WPTの条件成立後にheld1を再評価。固定WPT/Chrome互換profileとpublished CRD差をPhaseへ明記、whole WS/p100/target fullは未達。shared projection pending。

Event ws074-q25-conformance-followup-20261018: q26全文監査でq25追加host-performance-marks.cのsplit callが最後のsource引数とanswer引数を同じ行に置く§8違反を検出。元q25のmanual conformance未解消0という判定を訂正し、i01のstandards clearanceをinvalidate/unclearedへ追記する。原API実装・独立Chrome38/native7・全fixed corpusの実測結果/原Outcome/commit/hashは保持、名前付きproducer機能出力は引き続きverifiedなのでq26のconsumer前提は満たす。q26が既に許可するhost fixture追従範囲でformatを修正し、C tokens/全string literal不変を実照合、native7と全q26最終gateで再確認する。whole p254は元からunclearedであり状態不変、goal1481も不変。shared projection pending。

Event ws074-p256-added-from-q26-oracle-gap-20261018: 新[p256](phase256/phase.md)へWindow ErrorEventの既存欠落を渡す。意図的Observer callback failureはnative raw PASSに対し同じJS/official harnessのChromeはharness_error（completion.status1）、nativeはUncaught consoleのみでstatus0。p255が呼ぶ既存window_report_exceptionはWindow error listenerをdispatchしない。consumer positive30/GC10/producer7はverifiedだがq26 exception/harness fidelity条件は未達、held1は解除しない。p256 standalone timer/global ErrorEvent output→p255再評価を前提とし、p255 whole→p256の循環は作らない。旧raw PASS494にUncaught行は0、現確認済1481を保持。新sourceの実装は次Queue選定後。

Event ws074-q26-ended-uncleared-20261018: [history](history/ws074-q20261018-26.md)・[evidence](phase255/evidence/20261018-q26/README.md)。native PerformanceObserver/EntryListを追加し、real mark登録/queued records/private uncancellable Window task、buffered mark replay、entryTypes過去抑止、sticky mode/sequence/dictionary変換、takeRecords/disconnect/reobserve、actual frozen SameObject supportedEntryTypes=[mark]、callback identity/順序・microtask・例外後の継続とGCを実装。Chrome30条件が完全一致、非保守的stack GC/iterator/callback/再入/例外継続/公開timer cancel耐性/retired child list保持とDocument/realm回収のnative10条件、producer7条件を通過。独立probeはactual prior/future各1・futureだけ通知。 最終source12324ff1e・frozen final pairで全1508 harnessはq25比whole/callback/completionが完全同一、494 raw PASS。static1561は844 PASS/717 FAILで全pixel metrics不変、scripted413は105 PASS/283 FAIL/5 unsupported/20 missingで805 captures全RGB不変、strict48は39 PASS/9 missing不変。DOM38、CSSOM12+12、host/target exact2 source warning0、producer Chrome38/native7、Acid3得点/inventory100・pixel差0/Acid2既存差873維持。 ただしcallbackのassert_unreachedを意図的にthrowする同じJS＋公式harnessの失敗対照がnative raw PASS/completion0、Chrome harness_error/completion1となり、既存Window reporterがconsoleだけでErrorEventをdispatchしないことを実証。q26 callback exception/harness fidelity基準は未達なのでattempt uncleared、held1は加算せず確認済1481/3805（38.922470%）維持（raw1482）。旧raw PASS494にUncaught行は0で既存確認済みを根拠なく減算しない。 責務を新p256 Window ErrorEventへ分離し、既存Event/Window/timer→p256のstandalone error output→p255再評価の依存を追加（cycle無し）。次finite Queueで通常productionの例外通知を実装し、official harness negativeが真に非PASS/positiveがPASSとなることを確認後にheld caseとp255を再評価する。runnerでconsole文字列から結果を補正せずWPT/期待値/分母を保持。p254旧fixture split-call規約違反はq26でtoken/string literal不変formatとnative7/全文規約で訂正、q25 historical unclearedは保存。whole p255/p189/p100/target full/WS100%は未達、shared projection/GitHub pending、push無し。

Event ws074-q27-selected-20261018: p256のnative ErrorEvent/Window.onerror prerequisite partialをq27で選定。source provenance/muted errorsを保持するproduction reporterは次Queueへ残し、p256→p255依存・whole未達・held1除外を保持。共有projection pending。

Event ws074-q27-terminal-20261018: native ErrorEventをprivate registryへ追加し、Event継承/subclass/readonly enumerable message・USVString filename・uint32 line/column・arbitrary error（default undefined）、辞書変換順/brand/GCを実装。実WindowのIDL onerror handlerへ5引数を渡しBoolean trueだけcancel、通常listener/element/generic Eventの従来semanticsを維持。独立native28条件、Chrome27一致＋一次IDL通りのfilename USVString1差を明示、非保守的stackのreentrant GC/retired child ownership・実Document/realm回収9条件を通過。 final frozen pairでharness1508のraw PASS494を維持、whole/callback/completionはq26と全件同値。初回の既存FAIL1 IDだけprocess timeoutとなったが、全他suite終了後に同じ20秒/settle5000で1件だけ再確認し8.902秒で完了、全4305 callbacks/whole FAIL/completionが旧結果と一致。初回raw/checkpointsとretryを両方保持し最終selectionを明記。static1561は844 PASS/717 FAILで全pixel metrics不変、scripted413は105 PASS/283 FAIL/5 unsupported/20 missing・805全RGB不変、strict48は39 PASS/9 missing不変。DOM39、CSSOM12+12、producer7/observer10/error9、host/target exact event/window warning0、Acid3 score/inventory100・pixel0、Acid2既存差873を維持。 確認済1481/3805（38.922470%）維持、raw1482、held1は未解除。q27 scoped itemだけcleared、whole p256はproduction reporter/source provenance/muted errors未達でuncleared、p255の公式harness negativeも未達を保持。legacy content attributeのnamed parameter生成、eval/Function/module/worker、external source/CORS metadata、full target link/runtime/p100/WS100%は達成と主張しない。次finite Queueで通常例外の通知経路を実装し、timer-onlyとObserverのofficial harness negative/positiveを検証してheldを再評価する。 q26 declaration conformance follow-upはorigin p255/history/p100へ反映し、次の該当source Queueで整理する。shared projection pending。

Event ws074-q28-selected-20261018: q27のnative Event outputを前提にq28を選定。p255宣言順conformance partial、p256 runtime/inline provenance partial、p255 failure/held再評価の3 attempt。source policyはbinding所有のopaque managed metadata、unknownはredact、full Phases/100%の未達は保持。exact scopeは局所Queue、shared Master/Queue/cache/GitHub projection pending。

Event ws074-q28-i01-cleared-i02-started-20261018: p255 declaration-only i01をhost/target/token/object同値でclearし、p256 runtime/source i02へ進む。whole p255/p100/held1と確認済1481は保持、q28 final gatesへ接続。

Event ws074-q27-main-header-conformance-followup-20261018: q28全文監査で、q27新host-error-events.cのpublic main定義前がone-line commentだったと確認（C全文§3はmulti-lineを要求）。q27 scoped i01のconformance0/clearanceをこの限定点によりunclearedへ訂正。元native28/GC9/全corpus/host/target/API出力とscore1481は保持し、原cleared記述を履歴へ残す。q28-i02の実装前scope supplementに基づきheader commentだけをmulti-line化し、全code tokens/string不変をpublic-main-header-proofへ保存。q28 final build/native9で修正版を再確認する。元q27を後日の修正に合わせて成功へ書換えず、follow-upとして扱う。

Event ws074-q28-i02-cleared-i03-started-20261018: runtime/inline source partialの独立16/native GC19/公式harness negative/positive4と全fixed gatesを検証しp256 scoped i02をclear。source/privacy/record semanticsのwhole残りは保持、p255 i03で原heldとprior/futureを再評価する。全old whole/callback PASS後退0、まだscore1481/3805。

Event ws074-q28-terminal-20261018: VM functionのopaque managed sourceをactual nested closuresへ継承・traceし、throw/catch/foreign transferと同一primitive/undefinedのfresh throwへ保持。Window bindingのnative source cellは実inline Document URLだけを所有し、通常production reporterからgenuine trusted/cancelable ErrorEventを通知する。元error/pending/traceのroot/save/restore、default prevention/legacy true、bounded nested reporting、retired Windowの停止、checkpoint再入防止とmanaged owner保持を実装。VMはHTML/origin policyへ依存せず、unknown sourceは情報を伏せる。 final source27726847e/frozen pairでDOM40、runtime Chrome/native16一致、非保守的GC19（opaque sourceがfilenameだけを保持し退役Document/realmを保持しないこと、foreign conversion/job中の実owner保持・回収を含む）、旧Event9/producer7/observer10/CSSOM12+12、host warning0/target exact7source warning0を確認。C全文のnew/changed scopeはraw35の許可forward cleanup以外0。Acid3 score/inventory100・pixel0、Acid2既存差873を維持。 full harness1508は236.5秒、494 PASS/955 FAIL/13 harness_error/20 no_completion/24 no_output/2 multiple、callbacks11540 PASS/20642 FAIL/14 NOTRUN。旧whole PASSとPASS callbackの名前/数を全件保持。元console Uncaught由来の9件がFAIL/no_completionからharness_errorへ、wasm1件はharness_error内messageが変化。fatal completion後に旧FAIL callback1が実行されなくなったことを原source/console/auditへ保存し、API改善と誤認しない。static1561は844 PASS/717 FAIL・全pixel metrics不変、scripted413は105 PASS/283 FAIL/5 unsupported/20 missing・805全RGB不変、strict48は39 PASS/9 missing不変。heavy suitesを直列実行し計652.6秒、budget20秒/settle5000は維持。 公式harnessのtimer/Observer intentional failureは同じ原JSでnative/Chrome双方harness_error/completion1、正常timerとactual future-only Observerは双方PASS。prior/future実entry各1・futureだけ通知を再確認し、原held WPTをindex1496で再実行、full censusと全callback/completionが同一PASS、原JSはpinned Git blob SHA256427af82a2e3231c62432a40ff8ba7af4b7fed05b01472ca76a0e672d82447855とbyte同一。この1 IDの既知no-op/未通知/失敗通知前提がすべて解消したためheld1を解除。raw PASS1482は維持し確認済1481→1482/3805（38.948752%）、未attempt275は分母へ保持。新whole gainは0。 全3 attemptは選定済みpartialだけcleared。whole p255/p256/p189/p100/target full/WS100%は未達。external/CORS/redirect/eval/Function/module/worker source、syntax EINVAL/global reporting、legacy content attribute named parameters、reportError/PromiseRejectionEventは未実装。positionsはactual VM frameのscript-relative値で、HTML全体line offset/source map/Error作成stackとの厳密互換は未検証。rare extraction再入は有界なnative説明へ落とし、full HTML動作同一性を主張しない。共有agent全体のmicrotask順序も本Window partialで認定しない。 i01でenvironment.cの34 initializer-required declarationsをtable前、59 helpersを通常blockへ整理、全宣言multiset/他tokens/host debug除去object bytes不変とtarget warning0を確認。q27新fixtureのpublic main headerのone-line違反を発見し、歴史的q27 clearanceをdated follow-upでunclearedへ訂正。q28実装前のroutine comment-only scope補足fa65e0524に基づきmultilineへ修正、全code tokens/string不変とfinal native9で再検証。q26/q27の原uncleared履歴は後日成功へ書換えない。旧producer/observer fixturesのpublic main header等を含むfull WS conformanceはp100に残す。 shared projection pending。

Event ws074-q29-selected-20261018: q28 verified1482/3805・held0の証拠を保存後、[p257](phase257/phase.md) sync yield*を新規選定。p189のoriginal WebIDL2 compiler停止に対処し、runner/WPT/native API補完は変更しない。max90分、exact scopeは局所Queue。Master/共有Queue/next投影pending、whole WS incomplete。

Event ws074-q29-finished-20261018: [p257](phase257/phase.md) own同期delegationをcleared、Queue i01 scoped partial cleared。original WebIDL2の次のProxy残差を[p189](phase189/phase.md)へ保存し、そのPhaseのwholeはunclearedを維持。確認済1482/3805・held0、正規parser native1/4を隠さず記録。full source/conformance/hashes/rawと次再開条件は[history](history/ws074-q20261018-29.md)。Master/共有Queue/next/GitHub projection pending。

Event ws074-q30-selected-20261018: [p258](phase258/phase.md)を追加しmanaged read foundationを選定。script-free heap hooksを保ちrealm-aware receiver/Getへ分離、global公開は後続internal methods readiness後の別scope。q29 history/read-back完了、1482/3805・held0維持、whole WS incomplete/共有投影pending。

Event ws074-q30-finished-20261018: [p258](phase258/phase.md)のmanaged read foundationだけcleared、wholeはwrite/reflection/callability/global公開未達でuncleared。fixed1508全callback/completion同値、Chrome32/native threshold/managed realm/sole result/revoke回収の実出力を保存、確認済1482/3805・held0維持。次finite internal method scopeは未選定。 [exact history](history/ws074-q20261018-30.md)、共有Master/Queue/next/GitHub pending。

Event ws074-q31-selected-20261018: p258内部の依存をHasProperty→descriptor→Setへ具体化し、q31はHasと既存ToPropertyDescriptor検証順だけを選定。precodeの既知順序差をChrome/nativeで実証、p258 wholeとglobal公開は未達。exact max90分/source/criteriaは局所Queue、Master/共有Queue/next投影pending。

Event ws074-q31-finished-20261018: p258 Has/descriptor prerequisiteだけclear、public getter/setter観測順bugを修正。native/Chrome68・Get/Has実GC・全1508 callback/completion同値、確認済1482/3805維持。whole p258はdescriptor internal methods/write/global公開等で未達、次scopeは未選定。[history](history/ws074-q20261018-31.md)。standalone過去Queue投影を原文/sha付きで保存し現行Boardをcurrent cycleへ整理、Past Logは最新30件＋early continuationで全過去を到達可能に維持。共有投影pending。

Event ws074-q32-selected-20261018: [p258](phase258/phase.md)のVM共有descriptor helpersをfinite選定。q31 private Has/Get/GC出力を使い、To/From/CompleteをVM層へ集約、partial field presenceを独立native/Chromeで確認し既存Objectを追従する。focused gatesを適用、q31全1508 evidence再利用と未再実行を明記。standing Queueを再利用し全履歴を保持、whole/global公開/共有投影未達。

Event ws074-q32-finished-20261018: p258共有To/From/Complete変換partialをclear。native/Chrome93一致・GC/actual reclamation・focused gates/Acid確認。全1508 q31/画像crash q28を再利用し1482/3805を保持、whole p258/global公開/WS100%未達。[history](history/ws074-q20261018-32.md)。共有投影pending。

Event ws074-q33-selected-20261018: p258内のrealm-aware GetOwnProperty partialをmax90分で選定、q32 partial outputsを前提に公開単一descriptor APIまで接続。global公開/whole Proxy/WS100%は未達維持。共有投影pending。

Event ws074-q33-finished-20261018: p258 GetOwnProperty partial clear、公開Object single descriptor接続・snapshot/invariant・native/Chrome135一致・GC/回収・focused gates確認。1482/3805はq31/q28原証拠再利用、whole Proxy/global公開/WS100%未達。[history](history/ws074-q20261018-33.md)。共有投影pending。

Event ws074-q34-selected-20261018: p258 DefineOwnProperty partialをmax90分で選定、genuine Chrome/元descriptor保持/実GCを検証。shared boards pending、whole Proxy/global公開/WS100%未達維持。

Event ws074-q34-finished-20261018: p258 DefineOwnProperty partial clear、元partial descriptor保持/invariant・native/Chrome182一致・実GC/回収・focused gates確認。1482/3805はq31/q28原証拠再利用、whole Proxy/global公開/WS100%未達。[history](history/ws074-q20261018-34.md)。共有投影pending。

Event ws074-q35-selected-20261018: p258 Set ordinary/Proxy partialをmax90分選定。TypedArray専用Set/通常prototype assignment routingは別scope/global公開残件。whole p258/WS100%未達、shared projections pending。

Event ws074-q35-finished-20261018: p258 ordinary/Proxy Set partial clear、native/Chrome235一致・独立value/receiver実GCと回収・focused gates確認。TypedArray/通常prototype routingは残件。1482/3805はq31/q28原証拠再利用、whole Proxy/global公開/WS100%未達。[history](history/ws074-q20261018-35.md)。共有投影pending。

Event ws074-q36-selected-20261018: TypedArrayの前提canonical numeric keys/ToBigIntをVMで共有し、公開BigInt constructor/asIntN/asUintNのin-scope差を原native/Chrome対照と全1508で確認するmax90分partial。actual TypedArray Set/global Proxy/WS100%未達、shared projections pending。

Event ws074-q36-finished-20261018: p258 shared conversionsと公開BigInt partial clear、元HTML3差を修正。272 controlsはChrome153負ゼロ反例をraw保存してprimitive spellingを補うmixed oracle、純genuine exotic全一致を主張しない。全1508 q31と全callback/completion同値・1482/3805保持。p189へ検証出力を投影、actual typed Set/global Proxy等未達。[history](history/ws074-q20261018-36.md)。共有投影pending。

Event ws074-q37-selected-20261018: q35 ENOTSUPのfixed-buffer TypedArray Set/Defineをmax90分で具体化。sourceのscript-free native hook契約を守って変換をrealm層へ移し、内部content flag/factory rootsを整える。全1508/独立対照/GCを範囲へ、global/wholeWS未達、shared projections pending。

Event ws074-q37-finished-20261018: p258 fixed-buffer TypedArray Set/Define partial clear、native callback内user conversionをrealmへ移しpure native契約を回復。323対照/実GC graph回収・公開9差修正、Chrome153負ゼロ例外を保持。全1508 q36全whole/callback/completion同値・1482/3805維持、p189へ検証投影。次候補は原WebIDL2 private依存probe/IsArray等、全体/global/WS100%未達。[history](history/ws074-q20261018-37.md)。共有投影pending。

Event ws074-p258-q38-selected-20261018: q37の実native Proxyを元WebIDL2へprivate adapterで接続し、positive/negativeとgenuine Chromiumを対照する最大90分の有界dependency調査を選定。product/source/global Proxyは変更せず、公式PASSへ加算しない。exact範囲は[Queue](queue.md)。whole p258/WS未達を保持。

Event ws074-q38-finished-20261018: private原WebIDL2調査partial cleared。native/genuine Chromium positive4/4とintentional negative完全一致、公開前8不足を特定。次candidate IsArray等、公式1482/3805維持・whole未達。[history](history/ws074-q20261018-38.md)。

Event ws074-p258-q39-selected-20261018: q38のIsArray実差を元にProxy-aware VM/public Array.isArrayとGC/native/genuine対照を最大90分で選定。concat/flatten等別consumers/global公開は残件、exact範囲はQueue。whole p258/WS未達。

Event ws074-q39-finished-20261018: p258 IsArray partial cleared、336対照/real error allocation GCでownerと元Proxy生存・後に実回収、原parser positive/negative保持。focused gates通過、1482/3805維持。remaining keys/delete/prototype/extent等は後続有限Queue、global/p100/WS100%未達。[history](history/ws074-q20261018-39.md)。共有投影pending。

Event ws074-q40-selected-20261018: p258 extent2 operations/public wrappersの有限partialを選定、nested/global/whole残件を保持。[Queue](queue.md)。

Event ws074-q40-finished-20261018: p258 extent2 partial cleared、372対照・4 native GC modesでowner/target/handler/Proxy保持と実回収、pure policy拒否/errno保持。focused gates/元parser保持、1482/3805維持。次delete等candidate、global/p100/WS100%未達。[history](history/ws074-q20261018-40.md)。共有投影pending。

Event ws074-q41-selected-20261018: p258 DeleteProperty有限partialを選定、global/whole残件保持。[Queue](queue.md)。

Event ws074-q41-finished-20261018: p258 DeleteProperty partial clear、406対照と4 native GC modes/sole fresh Symbol含む全5実回収、false hook省略/ordinary委譲/errno維持。shape cacheが初回試験Symbolを保持した事実とfixture修復rawを保持。focused/元parser維持、1482/3805。次prototype等candidate、global/p100/WS100%未達。[history](history/ws074-q20261018-41.md)。共有投影pending。

Event ws074-q42-selected-20261018: p258 prototype2 operations/public/AnnexB wrappersの有限partialを選定、global/whole残件保持。[Queue](queue.md)。

Event ws074-q42-finished-20261018: p258 prototype2/public/AnnexB partial clear、452対照・4 GC modesでprototypeと元graphの独立保持/実回収。ordinary helperをVMへ移管・cyclewalk非通常stop、focused/元parser保持、1482/3805。tool versionsはq35と現q42一致、q39〜q41記録不足をdated followupに明示。次own keys候補、global/whole未達。[history](history/ws074-q20261018-42.md)。共有投影pending。

Event ws074-q43-selected-20261018: p258 OwnPropertyKeys VM foundation/private controlsを有限partial選定。public callersはverified output後の別scope、global/whole残件保持。[Queue](queue.md)。

Event ws074-q43-finished-20261018: p258 OwnPropertyKeys VM foundation partial clear。native501/Chrome499、huge2は外部RangeError差をprimary/診断で保持し全一致false。4 GC modesでprefix/list Symbols保持・rollback後未使用capacity Symbol実回収。focused/元parser保持、1482/3805。次public callers候補、global/whole未達。[history](history/ws074-q20261018-43.md)。共有投影pending。

Event ws074-q44-selected-20261018: p258 Object own-key/descriptor/integrity callersの有限partial＋全1508検証を選定、for-in/core/global/whole残件保持。[Queue](queue.md)。

Event ws074-q44-validation-progress-20261018: private sourceの旧501保持・新51中precode42差を修正、native552/Chrome550（既知huge2のみ）、4 public GC/error modes/focused/元parser保持。追加original validator negative2とintentional assertionの同一callbackを確認、これはq43 before未測定で新gainとは認定しない。全1508測定中、own Phase/tableのactive投影を整合。terminal判定は全callback/completion比較待ち。

Event ws074-q44-finished-20261018: p258 Object callers partial clear、new51 genuine一致/precode42修正・native552/Chrome550既知huge2。4 public GC/error modesでfresh Symbol boxing保持と実回収、元parser/追加validator rule検出保持。全1508 q37全callback/completion同値・gain/loss0・1482/3805、p189へvalidation出力投影。for-in/core/global/whole未達。[history](history/ws074-q20261018-44.md)。共有投影pending。

Event ws074-q45-selection: p258のisPrototypeOf/AnnexB lookup/OrdinaryHasInstance consumer partial開始、q44既存検証維持、whole acceptance未変更。[Queue](queue.md)。共有投影pending。

Event ws074-q45-finished-20261018: p258 chain consumers partial clear、native585/Chrome583既知huge2、新33 genuine一致/precode21修正。actual public lookup errno/GCとdirect ordinary-instance bare managed owner実保持/回収、元WebIDL/validator negative保持。q44全1508証拠再利用・q45未測定、公式確認済1482/3805保持。[history](history/ws074-q20261018-45.md)。共有投影pending。

Event ws074-q46-selection: p258のordinary assignment realm dispatch partial開始、q45既存証拠を保持し全1508を再測定する。whole/global未達。[Queue](queue.md)。共有投影pending。

Event ws074-q46-arraylength-revision: i01 current613/615、元low-level shrinkが削除拒否を無視する原因を確認。criteriaを維持し、同一Queue/deadlineへarray shrink low-level/realm helperと必要私有helpers・既存API commentを追加、全1508検証維持。[Phase設計改訂](phase258/phase.md)。他Phase構造変更無し・whole未達。

Event ws074-q46-finished-20261018: p258 object assignment/array shrink partial clear、native622/Chrome620既知huge2、新37 genuine一致/precode22修正。全1508 q44全callback/completion同一・gain/loss0・1482/3805。実GC/error/降順Delete・元WebIDL/validator保持。p189へvalidation output投影。[history](history/ws074-q20261018-46.md)。共有投影pending。

Event ws074-q47-selection: p258 Get/Has/GetOwn/Define target invariant realm化とprivate nested非callable factoryを選定、最新全1508 q46証拠維持・whole/global未達。[Queue](queue.md)。共有投影pending。

Event ws074-q47-finished-20261018: p258 nested非callable target partial clear、native658/Chrome656既知huge2、新36 genuine一致。sole current snapshot payloadが元actualtarget実回収後GCを越えSameValue適合、output-only後graph実回収と最終payload実回収。元WebIDL/validator/gates保持、最新全1508 q46再利用・q47未測定・1482/3805保持。[history](history/ws074-q20261018-47.md)。共有投影pending。

Event ws074-integration-direction-20261018（2026-10-18T22:35:53.785505+09:00）: ユーザー追加指示（原文）:「マージしながら作業をお願いします。ws.mdには書いていいですが、master.mdには書かないでください。phaseドキュメントには書いていいです。」
このchatのWS074継続作業へ適用: 各有限Queueの検証済みsource/recordsをWIP commitへ保存しmain包含・競合を確認しながら統合を続ける。現在のcheckoutはmainでq44〜q47のsource/terminal commitは全てmain ancestor。既存codex/ws074-acid3はmainとの差がmain側2568/branch側0、未統合commit無し、競合無し。重複/空mergeは作らず、以後別branch成果を統合する場合は対象SHA/差分/checkを固定してWIP統合する。他担当branchの一括mergeやpushは含めない。
ws.mdとPhaseに進捗/統合/指示根拠を記録してよい。master.mdはこのchatから編集しない。共有Master/Queue/cache/GitHub投影は従来どおりQ1所有/pendingで、必要な記録はWS074局所に残す。今回の追加指示は実装範囲/whole acceptance/scoreを変更しない。

[実読戻し](phase258/evidence/20261018-integration-direction.json)。

Event ws074-q48-selection: p258 ObjectToString/concat/flat IsArray consumer開始、plain @@spreadable追加のため全1508測定、Species/幅/global等whole未達。main包含確認し統合継続・master.md編集無し。[Queue](queue.md)。共有投影pending。

Event ws074-q48-finished-20261018: p258 array consumer partial clear、native690/Chrome688既知huge2、新32 genuine一致/precode23修正・boxed GC保持/回収。全1508 q46全callback/completion同一・gain/loss0・1482/3805、p189 validation出力投影。source599508209 main包含/競合無し・master.md編集無し。[history](history/ws074-q20261018-48.md)。共有投影pending。

Event ws074-q49-selection: p258 JSON.stringify actual internal operations/GC所有を選定、wholeは未達、main統合確認・master編集無し。[Queue](queue.md)。

Event ws074-q49-finished-20261018: p258 JSON.stringify partial clear、native726/Chrome724既知huge2、新36 genuine一致、precode22FAILとGC2SIGSEGV/4FAILの同入力をfinalで解消。全1508 q48全callback/completion同一・1482/3805、p189 validation出力投影。source2419e264c main統合・master.md編集無し。[history](history/ws074-q20261018-49.md)。共有投影pending。

Event ws074-q50-selection: p258 JSON.parse/reviver所有とactual internal consumersを選定、whole未達、main統合・master.md編集無し。[Queue](queue.md)。

Event ws074-q50-finished-20261018: p258 JSON.parse/reviver partial clear、native752/Chrome750既知huge2、新26 genuine一致・precode16FAIL/GC1SIGSEGV解消・dynamic text threshold GC確認。最新全1508 q49再利用・q50未測定、1482/3805維持。source9a6b5916f main統合・master.md編集無し。[history](history/ws074-q20261018-50.md)。共有投影pending。

Event ws074-q51-selection: p258 CopyDataProperties/rest internal consumersを選定、for-inは後続、whole未達。main統合・master.md編集無し。[Queue](queue.md)。

Event ws074-q51-finished-20261018: p258 CopyDataProperties/rest partial clear、native780/Chrome778既知huge2、新28 genuine一致・precode20FAIL/GC3functionalFAIL改善・direct API2 native GC/回収。最新全1508 q49再利用・q51未測定、1482/3805維持。source1c53b3ebe main統合・master.md編集無し。[history](history/ws074-q20261018-51.md)。共有投影pending。

Event ws074-q52-selection: p258 for-in traced per-level state/actual methodsを選定。primary許容exotic順序差を厳密な全trace equalityとしない。全1508q49比較。whole未達、main統合・master.md編集無し。[Queue](queue.md)。

Event ws074-q52-finished-20261018: p258 for-in partial clear、native809/Chrome807既知huge2、新shared29一致、native/Chrome algorithm順は合法差、direct native3participant実GC/回収。全1508 q49全callback/completion同一・gain/loss0・1482/3805、p189 validation output投影。source dd5843dd3 main統合・master.md編集無し。[history](history/ws074-q20261018-52.md)。共有投影pending。

Event ws074-q53-selection: p258 callable/Call/Construct＋VM-owned bound target/generic consumer安全性を選定。global constructor/Reflect公開は後続、whole未達。main統合・master.md編集無し。[Queue](queue.md)。

Event ws074-q53-finished-20261019: p258 callable/Call/Construct partial clear、native866/Chrome864既知huge2・新57 genuine一致、ordinary realm1/bound realm2 counterfactual改善、direct Call/Construct実GC/回収。全1508 q52全callback/completion同一・1482/3805、p189 validation output投影。source c602718f7＋483b5b7ad main統合・master.md編集無し。[history](history/ws074-q20261018-53.md)。共有投影pending。

Event ws074-q54-selected-20261019: p258 global Proxy/revocable actual builtinとpublic GC/原WebIDL/全eligible3530測定をfinite Queueへ選定。source additionはJS builtin層、q53 verified outputsを利用。whole/p100/100%未達、共有投影pending・master.md編集無し。

Event ws074-q54-finished-20261019: p258 actual global Proxy/revocable partial clear・public42 genuine一致/実GC回収、原WebIDL baseline4PASSと意図的negative維持。全eligible3530 current pair・1482/3805、harness oldPASS loss0/IDL setup2修復・net+16PASS callback。p189 validation output投影、source6b6be56e0 main統合・master.md編集無し。[history](history/ws074-q20261019-54.md)。共有投影pending。

Event ws074-q55-p259-added-selected-20261019: original CSS numeric failuresとnative/Chrome precision差からaccurate inline source/updating lifecycle＋factor serializationを一つのnew goal p259へ構造化、q55 finite90分で選定。既存p091 broad brief/p110 selector-only/p191 decorationと重複するaccepted outcomeを作らない。native DOM/CSS層で実装・main統合/全3530比較・master編集無し。

Event ws074-q55-p259-clone-revised-20261019: p259のclone specified-source精度をgenuine boundary証拠で修正、bind/cloning.c clone_shallowを同一goal generationへ追加。immutable source共有/独立mutation/new updating0をnative GCとChromeで検証、initial pair保持→final2全3530再測定。Phase in-progress/score未更新、master編集無し。
