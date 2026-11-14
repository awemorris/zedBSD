<!-- awesome-plan project=zedbsd record=master -->

# zedBSD Master

[AGENTS.md](../AGENTS.md)（規則）· [Q1 の運用](q1.md) · [Cockpit](cockpit.md) · [Queue](queue.md) · [担当の台帳](agents/registry.md) · [運用](agents/protocol.md) · [未完の WS](workstreams-open.md) · [完了した WS](workstreams-closed.md) · [Guardrail](guardrail.md) · [設計方針と全体の決定](master-design-policy.md) · [Amaterrace](amaterrace.md) · [道具と実機](tools.md) · [Bug Board](known-bugs.md) · [Future Work](future-work.md) · [Past Log](history/index.md) · [plan の地図](README.md)

<!--
  「今の状態」の各 block は「master:<名前>:start」〜「master:<名前>:end」で丸ごと置き換えてよい（今の事だけ。経緯・日付の log・ユーザーの発言の原文は書かない）。
  全体に効く今も有効な決定は master-design-policy.md §11、Q1 の運用の決定は q1.md、WS の決定はその ws.md。
-->

## 今の状態

### 回し方

<!-- master:loop:start -->
- **Awesome Plan の原則で自律的に回す**（2026-11-12 ユーザー）。Q1 は Queue を立て、担当に渡し、返却を確かめて merge し、次の Phase を立てる、を止めずに繰り返す。
- Phase の結果に未解析・食い違い・FAIL が残れば、ユーザーに聞かずに**考える Phase**（code と仕様で原因を突き止めて直す。実機は最後に 1 回）を立てて回す。測る Phase と考える Phase を分けて切る。
- 範囲の中の技術の判断（実装の形・試験の形・Phase の切り方・担当の割り当て・実機の順番・小さな直し）は Q1 と担当が決め、ws.md の「Q1 の判断」に残す。
- ユーザーに上げるのは次だけ: 製品の方向、危険の受け入れ、範囲の拡大・取り下げ、Makefile・`mk/` と HAL の API の差分、GHCI の仕様の決定、考える Phase でも解けなかった事。上げる時は Q1 の推しを付けて一度にまとめる（[q1.md](q1.md) §2）。
- ユーザーには試験を頼まない（拾える試験は [cockpit.md](cockpit.md) に置く）。
<!-- master:loop:end -->

### 動いている担当と Queue

<!-- master:agents:start -->
（2026-11-14 21:3x。詳しくは [cockpit.md](cockpit.md)。Q1 は main の checkout `/home/awe/zedBSD-claude1` で動く）

| 担当 | Queue | 中身 |
| --- | --- | --- |
| P1 | — | 空き。WS338 p002 は p001 の独立レビューの後 |
| agy | — | [WS347](ws347/ws.md) RTL の reset は 10 回目（amaterrace-gpu 38593b0e）まで。11 回目は Q1 が 10 回目を確かめてから |
| P2 | q2129 | [WS328](ws328/ws.md) p009 svg-pathdata（Sonnet）。p002〜p008 cleared |
| P3 | q2131 | [WS345](ws345/ws.md) p012 規約の全文の見直し。p015 cleared |
| P4 | q2116 | [WS068](ws068/ws.md)（EGL・GL を Vulkan の上に）試験の config と p045 |
| P5 | q2132 | [WS322](ws322/ws.md)（VF2 の Vulkan Video）p008 encode の設計（Wave420L） |
| P6 | q2109 | [WS302](ws302/ws.md) p003 の締め → p004 DTB の本体 |
| P7 | q2130 | [WS328](ws328/ws.md) p010 svg-style（Sonnet） |
| P8 | q2128 | [WS328](ws328/ws.md) p017 CSS の gradient の解析（Sonnet）。WS349 は p003 まで cleared、p004 は mk の枠（U1）の答え待ち |

- host は 2026-11-14 19:30 頃に再起動。NVMe の書き込みは 1.0 GB/s に戻った（止めた時は 22 MB/s）。merge していない branch の diff は [backup-20261114/](backup-20261114/README.md)。
<!-- master:agents:end -->

### Q1 が次に回す事

<!-- master:next-actions:start -->
上から順に、空いた担当へ。終われば次の Phase を Q1 が立てる。

1. [WS353](ws353/ws.md) 待ち方の作り直し: p016（P1、q2088）→ 規約の見直しの Phase → 完了の判定。
2. [WS347](ws347/ws.md) Amaterrace GPU: agy の reset の 10 回目の確かめ（atg_dc_tmds の reset 以外の書き換え）→ 11 回目以降（rst-files.txt の残り）。atg_dc_regs の testbench の FAIL 3 件はユーザーの判断待ち。reset の後に上位の testbench を流し直して描画の 4 つの原因を解析、依頼 08 の X5〜X9。
3. [WS333](ws333/ws.md) 電力・熱: p025 → 5330 で 1 回測る → p014 の判定 → p024（規約の見直しと文書）。
4. ラップアップの残り: [WS336](ws336/ws.md) p025（P2）→ completed、[WS345](ws345/ws.md) p010・p014 を新しい起動で片付け、cleared を判定する。
5. build system の作り直しの後始末（[BUG-418](bugs/BUG-418.md)、[BUG-419](bugs/BUG-419.md)、[BUG-435](bugs/BUG-435.md) の残り）。
6. 下の優先順位の WS を、空いた担当へ。
<!-- master:next-actions:end -->

### 統合の注意

<!-- master:merge:start -->
- build system は作り直した（top の Makefile と `mk/platform-<機種>/`）。作り直しの後、Makefile・mk/・`apps/*/Makefile` の comment と一部の規則は実態と合っていない事がある（2026-11-13 ユーザー: リファクタで build できなくなった所があり、記述が正しくない前提で扱う）。build の不具合は「単なるバグ」と決めず、作り直しの名残りを疑う。順は `make CONFIGMK=<cfg> llvm-toolchain-cached` → `make sysroot` → 個別の target。include/ を変えたら `make sysroot` を流し直す（[tools.md](tools.md)）。
- 担当の worktree は main から新しく作り、`build/` に main の tree を symlink しない（[BUG-420](bugs/BUG-420.md)）。試験と build は 4 thread まで。
- Makefile・`mk/` は Claude が変えない（`mk/test-config.mk` だけは Q1 が変えてよい）。要る時は差分の案をユーザーへ（[makefile-style.md](makefile-style.md)）。
- WS074（ブラウザ）は Q1 が管理し、Claude の担当に割り当ててよい。Windows 版のブラウザ（WS334）と libbrowser の直しは P8 に寄せる。
- Amaterrace GPU（`~/amaterrace-gpu`）: Claude は仕様と RTL の監査と driver、RTL は Gemini（agy）。互いにレビューする（[amaterrace.md](amaterrace.md)）。
<!-- master:merge:end -->

### 優先順位

<!-- master:priority:start -->
今の Focus は [WS347](ws347/ws.md)（VF2＋KC705 の Amaterrace GPU）。その他は依存と担当の空きで Q1 が回す（Queue の権限は変えない）:
1. WS333（電力・熱）・WS335（TLS）の締め。
2. 標準廉価実装手順の WS（[spec-module-delegation.md](../docs/agent/spec-module-delegation.md)、Opus が設計・Sonnet が実装）: WS326 → WS318 → WS329 → WS330、WS009 → WS296 → WS297、WS328 → WS074。
3. WS309 p015（ゲームモードの V2）、WS291 の NVIDIA の段、WS307 の残り。
4. WS299（5330 の NVMe。5330 が空いた時）、WS221（Jumbo frame）。
5. WS290・WS292・WS226 は WS347 の進み次第。

実機の使い分け: デスクトップ（10.0.10.3）は GPU の driver、Keiland の app は VM で作り VF2 で速さを詰める、5330 は Intel GPU・電源・Alder Lake と USB の NIC、VF2＋KC705 は GHCI、Pi 5 は熱と fan（[tools.md](tools.md)）。
<!-- master:priority:end -->

### ユーザーの判断が要る事

<!-- master:open-decisions:start -->
決めるまで、その部分を使わない形で他を進める。

- **GHCI の仕様**（[ghci-rtl-questions.md](ws347/ghci-rtl-questions.md) の B）: 判断待ちの B を、Q1 の推しを付けてまとめて上げる。決まるまで RTL は今の振る舞い、driver はどちらでも動く形。
- **Makefile・`mk/` の差分の案**: rpi5 の boot の notice と config.txt の comment に Pi 500 の dtb を足す（WS348）、`mk/rootfs.mk` の libcurl.so への link を消す（WS343、P8 が案を作る）、intelmid64 の `CONFIG_DRIVER_DELL_SMM_BOOST` の既定を n に（既定の kernel が link できない）、`plan/tools/aml-host/Makefile` の Dell の macro の名前、x86_64 の defaults を `?=` に、VF2 の config の PLATFORM の名前。
- **範囲**: WS162 を canceled にするか（WS172 に吸収）、WS282〜WS285 を始める時期。
<!-- master:open-decisions:end -->

### Q1 が決める事（ユーザーに聞かない）

<!-- master:q1-decides:start -->
以前ユーザーの未決に置いていたが、範囲の中の技術の判断なので Q1 が決めて ws.md・Bug に残す: nvrtx.disp.own の既定、pci-msi-qemu.c を残すか、P4m の時々の失敗（T1-691）の Bug 化、master-design-policy.md へ移す候補、WS279 p009（ライセンスと類似性の監査）の担当、WS299 の担当の割り当てと見つけた物（vDSO の無い clock_gettime、return の時の stdout の flush、memcpy）の Bug 化、UEFI の loader の GOP の mode の選び方。
<!-- master:q1-decides:end -->

### 止まっている物

<!-- master:blocked:start -->
- 5330 は遠隔の電源が無く、S0 idle のように戻らない恐れのある試験は流せない（形を Q1 が考える）。
- `plan/records.json`（GitHub の同期の対応）に消した Phase の path が残る（同期は今使っていない）。
- GitHub への公開は push の指示の時だけ。
<!-- master:blocked:end -->

### 次の番号

<!-- master:next:start -->
BUG-438 · q2133 · WS354 · KL_VERSION 110（106 は BUG-396 の reload、107 は BUG-404 の題の帯、108 は BUG-413 の IPv6、109 は WS345 p013 の resize の hook）。使う前に衝突を確かめる（`ls plan/bugs`・`ls -d plan/ws*`・`grep q20 plan/queue.md`）。
<!-- master:next:end -->

## 目的

- **目的**: 寛容なライセンスで企業が自由に使える UNIX 互換 OS を、GPL の Linux kernel に依存せずに作る。
- **利用者**: OS を組み込んで独自のディストリビューションを作る開発者・企業と、デスクトップ・ラップトップ・SBC で使う個人。
- **最終成果**: Linux/Android を置き換えられる水準の kernel と userland、最小の HAL による移植契約、用途別に構成・配布できる仕組み。
- **範囲**: kernel、HAL、driver、libc、base の userland、Keiland（desktop）、外部 package のクロスビルド、インストーラ、文書。
- **範囲外**: Linux の kernel ABI・DRM の互換、Mesa 流の user mode driver、正式な UNIX 認証・Vulkan CTS 認証の取得（主張しない）。
- **制約**: HAL の API の変更は差分ごとの事前承認（[Guardrail](guardrail.md)）。独立実装とライセンスの境界（[設計方針](master-design-policy.md)）。

### Objectives

- **O1**: 寛容なライセンスで企業が自由に使いやすい UNIX 互換システムを、GPL の Linux kernel に依存せず、Linux/Android を置き換え可能な水準で提供する。
- **O2**: デスクトップ、ラップトップ、SBC、タブレット、モバイルなど様々な規模で動く kernel と userland を提供し、開発者が独自ディストリビューションを自由にカスタマイズ・リブランディング・配布できるようにする。
- **O3**: UNIX/BSD/Linux の遺産から現代のシステムに必要なエッセンスを抽出し、networkd、netconf、service などをシンプルで一貫した仕組みとして再実装する。
- **O4**: ページベース MMU を備える 32bit/64bit コンピュータへ UNIX 互換 OS を確実に移植できる、明確で最小限の HAL を定義し、人類の共有知とする。
- **O5**: AI 時代の OSS のあり方を、大規模な AI 活用開発を通じて探索し、成果・失敗・人間の判断を再利用可能な知見として共有する。

### Milestone

達成は所属の WS の完了の数ではなく、到達点の証拠で判定する。completed の Milestone はまだ無い。WS の所属は下の表と [完了した WS](workstreams-closed.md) の Primary の列。

| Milestone | Objective | 受け入れの核 |
| --- | --- | --- |
| **MG001** 継続開発できる基盤 | O4, O5 | 文書化した環境で build でき、設計境界・規約・試験・制限を追跡できる |
| **MG002** UNIX アプリケーションの実行基盤 | O1 | process・memory・libc・loader/TLS の対応範囲を互換性台帳と代表アプリで確認できる |
| **MG003** 対象機へ導入して単独起動 | O2, O4 | 合意した機種・媒体でインストール後の単独起動と login を確認できる。実機と QEMU の証拠を分ける |
| **MG004** データの保持とメモリ/ストレージの実用 | O1, O2 | 永続化、低メモリ時の進行、媒体世代、既定構成の性能を確認できる |
| **MG005** 一貫したネットワーク/サービス管理 | O1, O2, O3 | networkd・netconf・service の責務・設定・操作が一貫し、永続化と失敗後の復旧を確認できる |
| **MG006** グラフィカルな操作環境 | O2 | 入力・描画・ウィンドウ・端末・GUI ツールの一連の操作を確認できる |
| **MG007** 用途別の独自ディストリビューション | O1, O2 | 第三者が用途別に構成し、独自ブランドで build・配布できる |
| **MG008** 最小 HAL の移植契約と異種機での実証 | O4 | HAL 契約・移植手順と異種/レトロ機での実証を公開する |
| **MG009** AI 活用 OSS 開発の知見の公開 | O5 | 設計権限・レビュー・変更追跡・失敗からの回復の事例と根拠を公開する |

## 未完の WS

一覧は [workstreams-open.md](workstreams-open.md)（ws.md の写し）。完了・取り下げは [workstreams-closed.md](workstreams-closed.md)。
