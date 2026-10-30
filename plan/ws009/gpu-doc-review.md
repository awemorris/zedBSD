# DOC-54 の分割の設計の敵対的レビューと反映（2026-10-30）

Parent: [WS009](ws.md)。対象: 骨格 5fedf634d・照合の script・[gpu-doc-rules.md](gpu-doc-rules.md)・p010〜p023 の phase.md（branch agent/p1-ws009、HEAD 038ee827f）。
design-reviewer（1 本）の指摘の全部と、反映・見送りの表。反映の後の骨格の基点は 4dfcce008（Phase の番号は p010〜p024 に組み直した）。
前の review（[phase009/design-review.md](phase009/design-review.md) の B1〜B3・M1〜M12・m1〜m8）の持ち越しは、reviewer が骨格の spec・規則・script に移っていることを確かめた（m14）。対応は下の最後の表。

Blocker は無かった。

| ID | 指摘（要約） | 扱い | 反映した所 |
| --- | --- | --- | --- |
| M1 | ws.md の受け入れの `--final --examples` が引数の読みで必ず FAIL | 反映 | script: option の順は自由、`--final` は次の option までの file を取る、未知の option は FAIL。ws.md の DOC-54 の行の command を直した |
| M2 | 条件つき・bit でない capability を `Requires:` と索引で表せない | 反映 | gpu-doc-rules.md 2 節の 6（Y は必ず見る bit、入れ子の handler の bit を含む、flag で変わる bit と op の有無は本文と Errors）。script は and と or の混在を FAIL。索引は p022 が各節から集めて code で確かめる |
| M3 | code block の中の `# ` を見出しとして読む | 反映 | script: fenced block の中を見出し・表・節の行として読まない（`--base` も） |
| M4 | lease の終わりで console に戻るという spec が `kern_text_end` と食い違う | 反映 | gpu-display.md の Leases・User-visible side effects、gpu-display-control.md の (9) を「driver ごと、約束しない」に。header の comment（gpu-display.h の commit の段）と code の差は Bug の候補として Q1 へ |
| M5 | `Errors:` の exact list が driver の素通りを含まない | 反映 | 規則に `- Other values: ...` の項を必須（script も要る）。gpu-requests.md の errno の表の spec を「kernel の自分の照合」に |
| M6 | p010 が過大 | 反映 | 索引の 2 列と errno の表を新しい file gpu-requests.md に移し、新しい p022 にした。p010 の待ちの表の読む物に driver の wait の op を足し、上限が driver ごとの物はそう書く spec に |
| M7 | 並行の UAPI の変化の確かめが移っていない | 反映 | gpu-doc-rules.md 1 節（着手と commit の前に `git log 4dfcce008..HEAD -- <header・gpu.c・gpu-fence.c・libvulkan>`、header の変化なら止める）と骨格の作り直しの手順 |
| m1 | 例の compile が 0 回でも PASS | 反映 | script: 0 回は FAIL |
| m2 | traceback で落ちる場合 | 反映 | script: 行の cell 数の照合、layout の無い struct、`--emit` の未知の名前、check ごとの例外を FAIL に |
| m3 | DEV_INFO が日付・plan/・Phase を見ない、title つきの link | 反映 | script: ISO の日付・`plan/` の文字・`Phase N` を DEV_INFO に、title つきの link を読む。docstring を実際に合わせた。`Sources:` の中身（関数名・試験が要求を使うか）は機械で見ない（p024 の review で見る） |
| m4 | `GPU0` などの誤検出、例の独自の名前 | 反映 | script: token は `GPU_`・`HW_GPU` で始まる物だけ。規則 2 節の 10（例で `GPU_`・`gpu_` を付けない） |
| m5 | capset の欄の名前が libvulkan の局所の名前に結び付く、Status の照合が弱い | 一部反映 | Status は 3 行目の完全一致に。欄の名前は consumer の読む語を使う設計のまま（spec に「Vulkan driver の名前」と明記）。libvulkan の改名は 1 節の変化の確かめで止まる |
| m6 | i915 の path の誤り | 反映 | gpu-drivers.md の spec と p019 を i915/display/{display,bootfb,capture}.c に |
| m7 | capset の関数に i915 が無い | 反映 | gpu-capabilities.md の spec と p011 に i915/command.c `i915_get_capset` |
| m8 | bit を足す所の一覧の抜け | 反映 | gpu-capabilities.md・gpu-drivers.md の spec と p011・p019 に powervr・bcm2711・mali・amdrdna/present.c。register の前か後かは書き手が読む（未確認のまま） |
| m9 | job の状態の名前が code と違う | 反映 | gpu-jobs.md の spec を `enum gpu_job_state` の状態に、p016 の注意も |
| m10 | errno の表の候補の抜け、未知の要求は ENOTTY でない | 反映 | gpu-requests.md の Errors の spec に EXDEV・ECANCELED・EALREADY・EOVERFLOW・ESRCH、gpu.md の呼び方の (4) に未知の要求の errno |
| m11 | 起こらない場合を問う spec、回復の待ち | 反映 | gpu.md の Nodes の (4) を access mode ごとの結果に、(5) に回復の待ちと `device->error` |
| m12 | seat.c の comment と gpu_open の食い違い、visionfive2-gpu.md の番号 | 反映 | 規則 2 節の 11（code に従う）、gpu.md の (6)、p023 の注意（visionfive2-gpu.md の番号）と Q1 への一覧 |
| m13 | 試験の path の形（venus-frame）、Vulkan 経由の試験 | 反映 | 規則 2 節の 6 の Sources（file 名は ls で、Vulkan 経由は数えない）、gpu-examples.md の spec、p014 |
| m14 | Q-D54-1 の決定が ws.md・p011 に無い、反映の表が無い | 反映 | ws.md のユーザーの決定と p011 に原文、この文書 |
| m15 | 要求の節の無い file の終わりの条件 | 反映 | 規則 4 節（節ごとの主張の表と 3 つの読み直し）、phase.md の 3 節を file の種類で分けた |
| m16 | 二重の主張の照合が p022 に無い | 反映 | 規則 4 節と p023（統合）の注意に一覧 |
| m17 | ws.md の表の merge の競合 | 反映 | ws.md の Resume point に「Q1 が merge で解く」 |
| m18 | 編集した文書に管理情報が残る | 反映 | evdev.md（q048・q044・WS006-p009・IN-T42・日付）と kernel-and-hal.md の Status の行を直した（4dfcce008）。kernel-boot-parameters.md は p023 で |
| m19 | capset の範囲の行で他の project の欄の名前を写す恐れ | 反映 | gpu-capabilities.md の spec に「欄の名前・構造を写さない」 |

前の review の持ち越し:

| 前の ID | 今の所 |
| --- | --- |
| B1（capability は ops->capabilities、venus の JOB） | gpu-capabilities.md の Capability bits の (1)・(4)、gpu-jobs.md の (5)、gpu-drivers.md の Known differences |
| B2（link の試験の引数） | gpu-doc-rules.md 3 節（docs/reference docs/architecture）。既存の link 切れは 2026-10-30 に直した |
| B3（試験を出典に） | 要求の節の `Sources:`（script は link の有無だけを見る。中身は p024） |
| M1（例の compile は target の clang） | script の `--examples` |
| M2（照合が名前の一部だけ） | script の索引・定数・構造体・capset・名前の照合 |
| M3（Status の行） | 骨格の 3 行目と script |
| M4（並行・待ち・例） | gpu.md の Concurrency、gpu-examples.md |
| M5（読みの権限） | 索引の Open mode、`Requires:` の open mode |
| M6（version と size の例外） | gpu.md の Calling conventions の (2) |
| M7（capset の形） | gpu-capabilities.md の Capability set 4（ユーザーの決定 Q-D54-1） |
| M8（副作用） | gpu-display.md の User-visible side effects、gpu.md の poll、gpu-submission.md の失敗と回復 |
| M9（既存の文書の食い違い） | p023 |
| M10（範囲と並行の作業） | gpu-sysctl.md、gpu-doc-rules.md 1 節 |
| M11（見積もり） | 節ごとの Phase に分けた |
| M12（driver の表の単位） | gpu-drivers.md の Node kinds |
| m1〜m8 | m1: gpu-command-stream.md、m2: gpu-jobs.md、m3: 範囲の外、m4: 英語、m5: ws.md、m6: 関数名で書く規則、m7: gpu-fences.md の (5)、m8: plan/ws014/tests を使わない（userland/tests を出典に） |
