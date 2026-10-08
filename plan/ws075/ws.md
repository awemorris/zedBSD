<!-- awesome-plan project=zedbsd record=ws075 -->

# WS075: i915 の高度化（今のデスクトップとグラフィックスを 5330 の i915 のネイティブ実行器で）

<!-- awesome-plan-current:start -->
Status: incomplete（2026-10-08 q910 P2 の照合: 空き時間だけの WS（master の Focus）。p006・p007a・p007b は in-progress（中断）、p017〜p031 の uncleared は描画の高速化の再開待ち）
Primary Milestone: MG006
Related Milestones: MG003
Objectives: O2
Parent: [Master](../master.md)
Queue: none（サブエージェント、WS068 から続けて）
Resume point: 2026-09-30（P1）: ユーザーの指示で描画の高速化をラップアップ（優先を下げ、機能性の実装に集中）。L1・L2 は済み（C6 中央値 67.3 ms、p029）。L3 は p030 で計測、p031（文字の draw をまとめる）は実装の差分を phase031/exp/ に置いて uncleared。再開はユーザーが描画の高速化の再開を言うとき（p031 の再開の手順）。以下は前の記録: 2026-09-30（P1）: p030 で L3 の計測（frame 約 45 ms、draw 1 つ 約 25 µs、文字が約 400 draw）。次は p031（文字の draw をまとめる）。以下は前の記録: 2026-09-30（P1）: p029 で L2 を満たした（blur は窓ごと、既定は無効、Settings は有効。C6 中央値 67.3 ms）。Files と keyboard の既定はユーザーの判断待ち（画面は phase029）。次は L3（p030）、全 WS の L2 がそろってから。以下は前の記録: 2026-09-30（P1）: p028 で L2 の計測（backdrop の使い回しで C6 1 run 65 ms、画面は同じ。slot 512 は効かない。blur を切る手も 65 ms だが絵が変わる、やめるかはユーザー）。次は p029（使い回しの実装）。以下は前の記録: 2026-09-30（P1）: p027 で L1 を満たした（C6 中央値 91.3 ms、stress 100 回で停止 0）。WS075 は L1 で区切り、L2（p028・p029）は全 WS が L1 にそろってから（Q1）。以下は前の記録: 2026-09-30: ユーザーの方針「広く浅く」で段の計画（下の節）を立てた。p026 は cleared（C6 中央値 92.3 ms、L1 の C6 は満たす）。次は L1 の残り（p027: 最終の image で stress 100 回と C6 の 5 run）。以下は前の記録: 2026-09-30: p025（BUG-117）は cleared（executor の object 表の lock）。次の候補: compositor の frame の長さの分析（C6）、GPU の object の 128 の枠。以下は前の記録: 2026-09-30: p024 は cleared（C6 の物差しを作り直し、10 app で中央値約 120 ms と判明）。次は p025（BUG-117、最優先）。以下は前の記録: 2026-09-30: p023 は cleared（分岐の中の ALU を飛ぶ。compositor の 1 run 9.3〜9.7 → 6.7〜6.8 ms、flip の率 7.7 → 9.5/s。latency の C6（50 ms 以下）は 2 回のうち 1 回）。次の候補: 同じ predicate の flag の作り直しの削減、draw ごとの停止（(b)）、素の 5330 での計測。以下は前の記録: 2026-09-30 wrap up: p023 の計測は済み（(a) 分岐の中の ALU が一番効く、(b) 効かない、(c) すりガラスは 1 run を変えず flip の率 +25%、すりガラスの扱いはユーザーの判断）。次は p023 の実装（分岐の中の ALU を飛ぶ）。2026-09-29 夜: p021（compiler の guard）・p020（RPS）・p019（FIFO の先行）は cleared。MAILBOX・IMMEDIATE はユーザーの判断でデモの後（F-057）。次の候補は compiler の段 2（分岐の中の ALU を飛ぶ）か encoder の scoreboard の緩和（main の判断）。以下は前の記録: 2026-09-29: p008（性能）を 3 つに分け（p008 完了待ちの割込み・p018 非同期の実行器・p019 present mode）、p008・p009 は cleared。p018 は着手前の計測で uncleared（8 app の遅さは compositor の GPU の合成、占有 90%）。p021（compiler: 分岐の中の texture の send を IF で飛ぶ）は cleared（10 app の latency 132 → 32 ms）。p018 は計り直しで効果が小さい見込み（要否は main の判断）。p019 と F-054（p020）は RPS の agent。その前、2026-09-28 の夜: p001〜p006 は実機で確認（MRT・query・SSBO・stencil・multisample・transform feedback）。HDMI の主出力 p011〜p013、BUG-091（p014）は cleared。p015（BUG-085 の再試験）は cleared: BUG-094 の原因（HAL の APIC timer の較正と AP の timecounter の probe が vCPU の停止で狂う）を直し、修正の後の 10 回で BUG-085・BUG-094 とも 0。p016（lease の替わり目で HDMI を点けたまま）は cleared: login・logout の 暗転 0（実機の passthrough の register）。p017（BUG-058）は 6 回で再現せず uncleared。BUG-095（capture の image の power-off）を起票。次: 実物の LCD での login・logout の目視（ユーザー）→ p007〜p010
<!-- awesome-plan-current:end -->
作業の手引き（2026-10-01）: [guide.md](guide.md)

## 目標

2026-09-27 ユーザー: 「OpenGL 3.2が問題なければ、それ以降のOpenGLはいったん保留して、i915の高度化に進んでください。」
（WS068 の GL 3.2 は Venus で PASS。main の中継）。

今の desktop と graphics を Dell Latitude 5330（Alder Lake-P、`8086:46a8`）の i915 のネイティブ Vulkan 実行器（WS031）で動かす:

- zdesktop（glass、backdrop のぼかし、tab、System Menu）と、その上の Vulkan の client（files、terminal、mview）。
- EGL/GLES 2・3（egltest の場面）と X11 の GLX の GL（固定機能、GL 3.0〜3.2 の glxtest）。
- そのために要る i915 の compiler（SPIR-V → GEN）と実行器（Vulkan の command → GEN）と driver（fence、共有、割込み）の不足の補い
  （[F-023](../future-work.md)、F-022 の残り）、性能と安定（完了の割込み、非同期の実行器、vsync、BUG-056・BUG-057）。

WS031 の単一目標（vkdemo のネイティブ描画）とは別の到達目標なので、新しい WS に置いた。WS031 の planning の Phase のうち
この目標に要るものはここへ移した（下の表）。

## 受け入れ

1. 実機（5330、capture）で zdesktop の glass・backdrop・tab・System Menu の絵が Venus と同じ形で出る（`CAPTURE=zdesktop`・
   `plan/tools/titlebar/menu-hw.sh`）。
2. 実機で egltest の es2・es3 の場面、GLX の zgears と glxtest の `--gl3`・`--gl31`・`--gl32` の検査が通る（通らない機能は
   device の feature として正しく断り、記録する）。
3. host の i915 の shader の検査で、上の client と libGLESv2 の生成する shader が全て通る。
4. 変えた source の規約の全文との照合、回帰（Venus の回帰と boot test、実機の回帰）。

QEMU（Venus）の証拠と実機（i915）の証拠は分けて書く。実機は `flock /tmp/i915-hw.lock` の下で 1 つずつ。

## 規則・境界

- AGENTS.md と [plan/coding-style.md](../coding-style.md) の全文、WS031 の [i915-rebuild-rules.md](../ws031/i915-rebuild-rules.md)。
- HAL（`include/hal/hal.h`）・UAPI（`include/drivers/gpu.h`）の変更は事前に提示する（`plan/ws075/proposed/`）。
- zdesktop の shader・source は desktop のサブエージェントが変えている。**zdesktop を直すのでなく i915 の側を直す**
  （例: panel.frag の関数呼出しは i915 の compiler で inline 化する）。zdesktop に触れる時は main を通して調整する。
- 試験は amd64 だけ。phase の最後に 1 回（host の試験、Venus の回帰、boot test、要るときだけ実機）。

## Phase

| Phase | 内容 | Status | 依存 |
| --- | --- | --- | --- |
| [ws075-p001](phase001/phase.md) | 調査: shader の全ての不足（host の survey、122 module）、client ごとの実行器の command（静的）、今の zdesktop の実機の capture | cleared（2026-09-27。実機の zdesktop 6/6 PASS、client の shader・command は全て通る。不足は GL/GLES の側） | — |
| [ws075-p002](phase002/phase.md) | desktop の新しい機能を実機で確かめる: tab、System Menu（`plan/tools/titlebar/menu-hw.sh`）、files・terminal（App Home）。出た不足を直す（zdesktop は直さず i915 の側で） | cleared（2026-09-27。実機で home 4/4・menu 11/11・x11 6/6。files の scenario は desktop の変更が落ち着いてから） | p001 |
| [ws075-p003](phase003/phase.md) | 実行器: primitive topology（triangle strip・fan、line list・strip、point list。今は triangle list だけ）、幅 1 以外の線、index の型、vkFreeDescriptorSets（F-023）。GL の app の大半が要る | cleared（2026-09-27。strip・fan・line・point を描く、実機の vkx 9/9。幅・PointSize・vkFreeDescriptorSets は後） | p001 |
| [ws075-p004](phase004/phase.md) | compiler（GLES 2 の核）: 補間の Flat・NoPerspective・Centroid、input builtin（FragCoord・FrontFacing・PointCoord・VertexIndex・InstanceIndex）、output PointSize、texture() の bias・offset と textureLod、local・interface の配列・struct と配列の定数、微分、Determinant・MatrixInverse・pack half、8・16 bit の vertex format | cleared（2026-09-27。実機 vke2 17/17・vke1 4/4・vkx 9/9・vkc 9/9、zdesktop・x11 の capture 6/6。Grad・fine の y 微分は p005） | p001 |
| [ws075-p005](phase005/phase.md) | texture の種類: compiler の texelFetch（OpImage・OpImageFetch）・textureSize、shadow（Dref）、integer sampler、cube・配列・3D の sampler。実行器の cube・配列・3D の image、mip level・layer への描画（ws031-p030）、depth の copy、sampler の compare 等（ws031-p034）、descriptor 配列（ws031-p035） | cleared（2026-09-28。実機 egltest の glsl・glsl3・fbo・cube・es3・formats・volumes が failures 0、vke1 6/6・vke2 17/17・vkx 9/9・vkc 9/9、zdesktop・files PASS） | p004 |
| [ws075-p006](phase006/phase.md) | 実行器と compiler: MRT（ws031-p031）、occlusion query（sync の module）、texel buffer（buffer view）、storage buffer（transform feedback の VS の store）、stencil、multisample の image と resolve | in-progress（増分 1〜5 済み、後退を修正。増分 6 は実機が未実施）。2026-09-30 段の外（後回し、下の段の計画） | p005 |
| [ws075-p007](phase007/phase.md) | GL 3.2 の stage: geometry shader（compiler の stage と 3DSTATE_GS）、gl_Layer と layered の描画、PrimitiveId。2026-10-07 に p007a（compiler、増分 a1〜a5）と p007b（実行器、b1〜b5）に分けて設計（[design.md](phase007/design.md)） | planning（2026-10-07 設計、design-reviewer の反映待ち） | p006 |
| [ws075-p008](phase008/phase.md) | 性能 1: 完了待ちを割込みへ（ws031-p044）。2026-09-29 に 3 つに分けた（p018・p019） | cleared（2026-09-29。worker は engine の割込みで起きる。実機の passthrough で request の終わりの 149/150 が割込みの直後、latency 49.3 ms・20 flip/s は前と同じ、vkx 9/9） | p002 |
| [ws075-p009](phase009/phase.md) | 安定: BUG-056・BUG-057（実機の zgears の止まり）ほか p002〜p008 で出た bug | cleared（2026-09-29。render の context の上限 8 → 32 と compiler の OpSwitch を直し、App Home の 8 app が全て起動、20 分の負荷で failed 0。BUG-094 resolved） | p002 |
| ws075-p010 | 規約の全文との照合、統合回帰（最後） | planning | 全 Phase |
| [ws075-p011](phase011/phase.md) | HDMI の主出力の実機の事前調査（[hdmi-main-output.md](hdmi-main-output.md) の H1）: EDID、点く mode、DVI、HDMI の前後の USB | cleared（2026-09-28。EDID は読める、native は 1920x1280。pipe B・DVI で 720p・1080p・1920x1280 を出力。touch の USB は 5330 に現れない。絵の目視は未実施） | — |
| [ws075-p012](phase012/phase.md) | HDMI の主出力（H2）: `display=hdmi\|auto`・`display.mode=WxH[@R]`、resident を HDMI（port B・pipe B・DVI）で、無ければ eDP | cleared（2026-09-28。実機で `display=hdmi` は HDMI 1920x1280（EDID）の Keiland 全画面、`display.mode=1920x1080@60` も、`display=auto` は eDP。画面は scanout の buffer から。host 80 checks・boot test PASS。LCD の目視と HDMI の無い boot は未実施） | p011 |
| [ws075-p013](phase013/phase.md) | デモの形での確認（H4）: デモの image（`plan/ws075/demo/`、graphical boot + `display=hdmi`）、splash・greeter・login・session の全体、30 分の連続表示、Shut Down、eDP への fallback | cleared（2026-09-28。実機の passthrough で全体を通した。H2 の pipe B の frame counter の不具合を修正、i915 の node の前に sessiond が諦める件は image で回避。最終の image で Notes の起動が kernel の fatal（i915 の timer thread の spin_unlock の持ち主の違い、未修正、要 Bug ticket）。LCD・eDP の目視と bare metal の起動は未実施） | p012 |
| [ws075-p014](phase014/phase.md) | [BUG-091](../bugs/BUG-091.md) の修正: i915 が割込み許可のまま spin lock を持つ所（timer queue・start registry・retire の irq_lock・1 tick の sleep）と `kern_usleep_range()` を irqsave に。H4 の greeter の黒い画像の説明 | cleared（2026-09-28。実機の passthrough で Notes・PDF Viewer の起動 48 回 fault 0、host の contract 42 checks、GPU の無い boot test PASS。H4 の greeter の黒は表示していない buffer A を撮ったもの） | p013 |
| [ws075-p015](phase015/phase.md) | [BUG-085](../bugs/BUG-085.md) の再試験（BUG-091 の修正の後）、`h4-ctl.py shot` の live buffer の選択、[BUG-094](../bugs/BUG-094.md) の原因 | cleared（2026-09-28 の再開。BUG-094 = HAL の起動時の時間の測定が vCPU の停止で狂う（`lapic.c` の較正を gate の縁で括り 3 窓の最短、`timecounter.c` の AP の probe を最低 2 秒）。修正の後の egltest6・p005 の各 5 回で BUG-085・BUG-094 とも 0、shot の live は register（PLANE_SURFLIVE）で選べた。GPU の無い boot test PASS） | p014 |
| [ws075-p016](phase016/phase.md) | lease の替わり目で HDMI を点けたまま（[F-048](../future-work.md) のこの構成の目標）: release で window を出ず最後の絵を次の lease の最初の flip まで保つ（10 秒で期限切れ、PCI shutdown で止める） | cleared（2026-09-28。実機の passthrough で login 6・logout 5 回とも暗 0・黒 0、前は 180〜383 ms と transcoder の停止。Shut Down と期限切れで出力は止まる。実物の LCD の目視は未実施） | p015 |
| [ws075-p017](phase017/phase.md) | [BUG-058](../bugs/BUG-058.md)（App Home の zgears が最初の frame の前に終わる）の再試験 | uncleared（2026-09-28。6 回の起動で再現せず、原因は未特定） | p016 |
| [ws075-p018](phase018/phase.md) | 性能 2: 非同期の実行器（ws031-p045）。p008 から分けた | uncleared（2026-09-29 の試み: 着手前の計測で、8 app の遅さは compositor の合成の GPU（占有 90%、1 batch 100 ms）と分かり、非同期化では良くならない見込み。未実装。p021 を提案） | p008 |
| [ws075-p019](phase019/phase.md) | 性能 3: present mode と vsync（ws031-p027）。p008 から分けた | cleared（2026-09-29。FIFO の先行（arm で返り、次の present が latch を待つ）、実機で rate 55.5→57.9/s・latency 同じ・周波数が下がる。MAILBOX・IMMEDIATE はユーザーの判断でデモの後、[F-057](../future-work.md)） | p008 |
| [ws075-p020](phase020/phase.md) | RPS（GT の周波数）の up/down の割込みと boost（[F-054](../future-work.md)）。今は RP0 固定（ws084-p002） | cleared（2026-09-29。Alder Lake-P では up/down の割込みが来ないので Linux の gen12 と同じ busy の時間の評価と park・unpark に。実機の passthrough で rate 19.7→54.8/s、latency 49.3→32.5 ms、idle は最低の周波数。素の 5330 は未実施） | p008 |
| [ws075-p021](phase021/phase.md) | 性能: compiler が どの channel も走らない block（まず texture の send）を飛ぶ。p018 の計測から提案 | cleared（2026-09-29。10 app の desktop で flip 5.5 → 8.1/s、latency 中央値 132 → 32 ms、compositor の 1 run 14.8 → 8.5 ms。vkx・vke1・vke2・vkc PASS） | — |
| [ws075-p022](phase022/phase.md) | 性能: encoder の scoreboard の直列の緩和（send・math の結果を使う直前まで待たない、token 16 個） | uncleared、patch は保留、再検討は命令の並べ替えを入れるとき（2026-09-29。実装と host の検査は済み、実機の 10 app で compositor の 1 run 8.57 → 8.70 ms と縮まず。source は戻し差分を `phase022/scoreboard-pass.patch` に。`guard/scoreboard-check.h` は使い続ける） | p021 |
| [ws075-p023](phase023/phase.md) | 性能: 分岐の中の ALU を飛ぶ。最初の段で (a) 分岐の中の ALU と (b) draw ごとの pipeline の停止と flush、(c) すりガラスを切った場合を測る | cleared（2026-09-30。計測の段: (a) 上限 8.4 → 4.9 ms、(b) 変わらず、(c) 1 run は変わらず。ユーザーの判断で (a) を実装: 証明できた囲いを「どれかの channel が入れば全 channel」の IF で飛ぶ。実機の passthrough の 10 app で compositor の 1 run 9.3〜9.7 → 6.7〜6.8 ms、flip の率 7.7 → 9.5/s、latency 中央値 pre 65.6・81.5・82.8 → 65.7・48.9 ms、画面は画素まで同じ、vkx・vke1・vke2・vkc PASS） | p021 |
| （候補） | draw ごとの pipeline の停止と flush の削減（p023 の (b)、今の compositor では効かなかった） | planning | — |
| [ws075-p024](phase024/phase.md) | C6 を判定できる物差し（cursor が出る flip まで、5 run 以上をまとめる）・select の flag の再利用・host の fixture の既存の失敗 | cleared（2026-09-30。10 app の C6 は中央値 121.5 ms・p90 173.0 ms（5 run、200 試料）で 50 ms を満たさない。旧い latency は pointer を含まない flip で止まり小さく出ていた。flag の再利用で 1 run -3%、C6 は変わらず。host の fixture は全 10 個 PASS） | p023 |
| [ws075-p025](phase025/phase.md) | [BUG-117](../bugs/BUG-117.md): 窓の多いとき executor が set 0 binding 0 の view・sampler の無い draw を拒み compositor の描画が止まる | cleared（2026-09-30。executor の object 表を複数の session が lock 無しに同時に変え、compositor の descriptor set が消えていた。表と allocation の list に mutex。実機の passthrough の stress 100 回で set の消失 2 → 0 件、vkx・vke1・vke2・vkc PASS） | p024 |
| [ws075-p026](phase026/phase.md) | compositor の frame の長さの主因の分析と、縮める手（C6） | cleared（2026-09-30。主因は executor の同期の run（1 frame 約 58 ms、739 draw を 5〜7 run）と frame pacing の待ち（約 31 ms）。pointer が動いた frame は pacing を待たない（compositor）: C6 中央値 121.1 → 92.3 ms・p90 191.7 → 140.8 ms（5 run）、flip 9.5 → 14/s。段の L1 の C6 は満たす） | p025 |
| [ws075-p027](phase027/phase.md) | L1 の判定: 最終の image で stress-117 の 100 回（停止 0・消失 0）と C6 の 5 run（中央値 100 ms 以内） | cleared（2026-09-30、P1。L1 を満たす: C6 中央値 91.3 ms・p90 135.9 ms（5 run）、stress 100 回で停止 0・拒否 0・消失 0。素の 5330 は未実施） | p026 |
| [ws075-p028](phase028/phase.md) | L2 の計測: 1 frame の draw のうち backdrop の割合、slot を 512 にした 1 run の実験、backdrop を使い回した 1 run の実験（と blur を切った 1 run） | cleared（2026-09-30、P1。backdrop は draw の 39%。1 run の C6: base 82.5、slot 512 80.9（効かない）、使い回し 65.4（画素まで同じ）、blur を切る 65.9 ms（絵が変わる）。p029 は使い回しを推す） | p027 |
| [ws075-p029](phase029/phase.md) | L2 の実装（2026-09-30 ユーザーの判断、下の節）: すりガラスの blur を**窓ごとに有効・無効**にする。無効の窓は下の窓を描き直さず blur 済みの壁紙だけを透かし（p028 の blur を切った実験）、有効の窓は今の backdrop。既定は無効、Settings は有効。app が選ぶ口（libkeiland の glass の API か Keiland の protocol の flag）と compositor の keyboard の panel の flag。Files と keyboard の panel は有効・無効の両方の画面を撮ってユーザーが選ぶ。C6 中央値 75 ms 以内を 5 run（10 app、Settings を含む）、stress 100 回、C7 を再測定 | cleared（2026-09-30、P1。keiland_glass_v1 の version 2 の set_blur と keiland_glass_set_blur（KEILAND_VERSION 17）、既定は無効、Settings は有効、keyboard は zdesktop の --keyboard-blur。L2 を満たす: C6 中央値 67.3 ms（5 run、Settings を含む 10 app）、stress 100 回で停止 0。C7 PASS・C9 10/10・boot PASS。Files と keyboard の既定はユーザーが画面で決める） | p028 |
| [ws075-p030](phase030/phase.md) | L3 の計測と手の選択（L2 の後の frame の内訳） | cleared（2026-09-30、P1。frame 約 45 ms = GPU 約 35（draw の固定の費用 約 12・画素 約 23）+ 同期の往復 約 5 + 記録 約 3 + 合成の CPU 約 3、pacing の待ちは 0。draw 1 つ 約 25 µs（文字 約 400 個）。手の順: 1 文字の draw をまとめる（frame 約 8〜10 ms 減の見積もり）、2 draw ごとの状態の書き直しを減らす、3 非同期の実行器） | p029 |
| [ws075-p031](phase031/phase.md) | L3: 文字の draw をまとめる（compositor、1 つの文字列の glyph を 1 回の draw に。画素の比較と C6 の 5 run） | uncleared（2026-09-30、P1。ユーザーの指示で描画の高速化をラップアップ、優先を下げた。実装は build まで済み、検証の前に止めたので source は戻し差分は phase031/exp/text-batch.patch。再開はユーザーが描画の高速化の再開を言うとき） | p030 |
| [ws075-p032](phase032/phase.md) | BUG-120: GT の object の表を 128 個ずつ伸ばす（最大 2,048） | in-progress（P1、実装と host 試験 PASS、実機は S2） | — |
| （候補） | GPU の object の枠（128）が 30 窓で尽きる（`gt memory: object pool exhausted`、p025 で観測） | planning | — |

各 Phase の受け入れは、host の survey（`plan/ws075/tests/shader-survey/run.sh`）の該当の不足が 0 になることと、実機の capture
（egltest・glxtest の場面の capture の scenario は p003 で足す）。


## 段の計画（2026-09-30、ユーザーの方針「広く浅く」）

ユーザーの方針（2026-09-30 朝、Q1 経由）: デモ critical の WS は各 WS を「まず動く」の段までそろえ、磨き込みは段（L1・L2・L3…）ごとに
数値目標を持つ小さな Phase（1〜2 時間）に分ける。どこで止まってもデモの全場面がその時点の段で動く。1 つの WS を深く掘り続けない。

WS075 の段の数値目標は、デモの S11（窓 10 個の操作）の応答（WS099 の C6: 窓 10 個で pointer の移動から表示までの中央値）と
止まらないこと。どの段も測り方は同じ:

- C6: 5330 の passthrough で `plan/ws075/tests/hdmi/measure-apps.sh` を同じ image で 5 run、`plan/ws075/tests/hdmi/c6.py` のまとめた
  中央値（p90 も記録）。
- 止まらないこと: `plan/ws075/tests/hdmi/stress-117.sh 100`（10 app の上で Model viewer の開閉 100 回、毎回 flip を数える）で描画の停止 0、
  kernel log の `draw refused`・`not a descriptor set` 0。
- 回帰: `test-hw.sh` の vkx・vke1・vke2・vkc、host の `run-vk-host-tests.sh`（全 10 個）、QEMU の boot test、画面の画素の比較（各 app の撮影）。
- 素の 5330 での確認は段ごとに 1 回（ユーザーの実機、未実施のものは未実施と書く）。

| 段 | 数値目標 | 今（2026-09-30） | 何を直せば届く見込みか | Phase（各 1〜2 時間） |
| --- | --- | --- | --- | --- |
| L0 | 窓 10 個が開き、描画が続く | 済み（p021〜p025） | — | — |
| L1 | C6 中央値 100 ms 以内・描画の停止 0（stress 100 回） | **済み（p027）**: C6 91.3 ms（5 run）、stress 100 回で停止 0 | 確かめるだけ | p027: 最終の image で stress 100 回と C6 の 5 run（L1 の判定） |
| L2 | C6 中央値 75 ms 以内 | **済み（p029）**: 67.3 ms（5 run、blur は窓ごと、既定は無効、Settings は有効） | frame の GPU の時間（約 58 ms、1 frame 739 draw）を半分に。draw の数は窓ごとの backdrop（下の scene を 1/8 で描き直して blur）で窓の数の 2 乗に増える。(a) 下が変わらない窓の backdrop を使い回す（compositor、backdrop.c・shell.c）、または (b) slot を増やし（128 → 512）run の数を 5〜7 → 1〜2 に（executor、heap.h）。(a) は絵を変えない範囲で。まず (a)・(b) の効きを 1 run ずつの実験で測ってから 1 つ実装 | p028: 計測（backdrop の draw の割合、slot を増やした 1 run の実験）。p029: 効く方を実装し C6 の 5 run |
| L3 | C6 中央値 50 ms 以内（WS099 の C6） | — | frame を約 30 ms に: L2 の後の残りの GPU の時間（draw ごとの固定の費用: slot ごとの状態の書き直し、context setup）と、同期の run の間の CPU の空き（非同期の実行器、p018）。L2 の結果で決める | p030: L2 の後の frame の内訳の計測と手の選択。p031〜: 1 つずつ（各 1〜2 時間） |

段の外（後回し、デモの台本に要らない。必要になったら段の計画に入れる）:

- p006 の残り（GL の MRT・occlusion query・texel buffer・transform feedback・stencil・multisample の実機の増分 6）、p007（GL 3.2 の
  geometry shader など）。デモの台本は GL 3.2 の機能を使わない（zgears・glxtest の基本は p005 まで）。
- p017（BUG-058 の再試験）、p022（scoreboard の緩和、効かなかった）、draw ごとの pipeline の停止の削減（p023 の (b)、効かなかった）。
- GPU の object の枠（128）が 30 窓で尽きる（BUG-120、tracking）。デモの S11 は窓 10 個。
- p010（規約の全文との照合、統合回帰）は WS の完了の条件として最後に行う（段とは別）。


## files の実機の場面（main の依頼、2026-09-28）

`plan/ws031/tests/i915-capture.py` の `files`（`KEILAND_APP=home plan/ws075/tests/capture-hw.sh files zdesktop OUTDIR`）:
Home から Files を起動し、Ctrl+T・Ctrl+Tab・Ctrl+W でタブを開閉・切り替え、title bar の double click で dock、下端からの drag で
Wiseview を開閉、dock した窓の閉じる button で終える（9 検査）。Files の窓の位置は先に map された窓の数で変わるので、desktop との
差分から窓を見つける（`changed_box()`）。`config-zdesktop-hw.mk` に files とその library を足した。

| run | 結果 |
| --- | --- |
| hw-1 | image に /bin/files が無い（config を直した） |
| hw-2 | Files の glass・タブ・Wiseview は PASS、dock は固定の座標の誤りで FAIL（窓の検出に替えた） |
| hw-3・hw-4 | [BUG-077](../bugs/BUG-077.md): render engine の hang（engine_recover 未実装、device lost）で compositor が停止 |
| hw-5 | 9/9 PASS。画面 `build/ws031-shots/ws075-files-20260928-*.png`（sheet・desktop・files・tab-new・tab-first・tab-closed・docked・wiseview・wiseview-closed・ended） |

Files まで進んだ 4 回のうち 2 回が BUG-077。実機の証拠のみ（QEMU は未実施）。BUG-077 は 2026-09-28 に GPU core の修正で resolved
（修正の後の fix-files1〜3 は 9/9 PASS、`build/ws075-bug077/`）。

## 初期のグラフィックの試験の削除（main の依頼、2026-09-28）

ユーザーの判断（`plan/master.md` の「古いグラフィックの試験の driver」）により、初期の Venus・WSI の host 試験（build できない
ものと Venus だけのもの、参照の無い 2 つ）と `plan/tools/venus-console.c` を削除した。一覧と理由、残したもの（GPU core の host 試験、
QEMU の遠隔の harness、`vkdemo_oracle.py`）は [`plan/ws014/tests/README.md`](../ws014/tests/README.md) の 2026-09-28 の節。
i915 の executor の試験（vkx・vke1・vke2・vkc、gentool、capture の場面、`plan/ws031/tests/run-vk-host-tests.sh`）は残す。

## WS031 から移した Phase

2026-09-27 に移した（WS031 の表に印）。範囲の正本は WS031 の元の Phase の記述（`phase016`・`phase017`・`phase018/phase.md`）。

| WS031 | 移した先 |
| --- | --- |
| ws031-p027（present mode・vsync） | p008 → p019（2026-09-29 に分けた） |
| ws031-p030（mip level・layer への描画） | p005 |
| ws031-p031（MRT） | p006 |
| ws031-p034（sampler） | p005 |
| ws031-p035（descriptor 配列・VS の sampled image） | p005 |
| ws031-p038（整数の varying） | p004 |
| ws031-p040（local の配列・構造体） | p004 |
| ws031-p041（OpSwitch・関数呼出し） | p004（今の corpus には無い。client は glslc -O で inline 化される。GL の shader で要るとき） |
| ws031-p044（完了の割込み） | p008 |
| ws031-p045（非同期の実行器） | p008 → p018（2026-09-29 に分けた） |

### 進め方とハーネス（2026-09-30 Q1 の補足）

- 広く浅くの方針で、L1（p027 の確かめ）の後は他の WS へ回り、L2（p028・p029）は全 WS が L1 にそろってから。
- **L2 の作業像**: p028 は 2 つの手を 1 run ずつの実験の build で比べるだけ（source は main に出さない）。(a) backdrop の使い回しは、
  下の scene の damage が無い窓の blur を前の frame の結果で済ませる。(b) slot 512 は executor の heap の大きさの変更。効きの大きい方だけを
  p029 で実装する。どちらも「画面が画素まで同じ」が必須。
- **ハーネス**: `measure-apps.sh` 5 run と `c6.py`、`stress-117.sh` 100 回、test-hw（vkx・vke1・vke2・vkc）、`engine-gdb.sh` と kernel log の frame の内訳
  （p026 で常設）。実機の passthrough の lock は WS099（P5）・WS101 と共有。

### ユーザーの判断（2026-09-30、すりガラス）

p028 の結果（backdrop が 1 frame の draw の 39%、使い回しで C6 82.5 → 65.4 ms・画素は同じ、blur を切ると 65.9 ms・絵が変わる）を示し、
「残して使い回しを実装」（Q1 の推奨）・「blur をやめる」・「両方（設定で選ぶ）」を尋ねた。ユーザーの答えは **「blur をやめる」**。
窓の sidebar と title bar のすりガラスは、下の窓ではなく blur 済みの壁紙を透かす（p028 の図の右）。p029 はこの形で実装する。使い回しの patch（`phase028/exp/`）は使わない。
- **同日の追加の判断（窓ごと）**: ユーザー「blurですが、アプリごとに有効・無効を切り替えられるようにしましょう。Settingsは常用ではないので有効でOK、
  Filesやスクリーンキーボードでは比較して見た目で決めたいです。常用アプリではパフォーマンスを優先します。でも、スクリーンキーボードって、若干ダサい
  んですよね。だからそれを緩和するために、多少パフォーマンスを犠牲にしてもいいとは思います。」→ p029 は「全ての窓で blur をやめる」から
  「窓ごとに選べ、既定は無効（性能を優先）、Settings は有効、Files と keyboard は比べて決める」に改めた。keyboard は見た目のために性能を多少使ってよい。
- **Files と keyboard の blur（2026-09-30、p029 の見比べの図の後）**: ユーザーの答えは、Files も keyboard の panel も **「無効（壁紙だけ）」**。
  今の既定（両方とも無効）のままで、code は変えない。blur が有効なのは Settings だけ。zdesktop の `--keyboard-blur` は既定で立てない。

