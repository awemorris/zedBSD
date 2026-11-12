<!-- awesome-plan project=zedbsd record=master -->

# zedBSD Master

[AGENTS.md](../AGENTS.md)（規則）· [Q1 の運用](q1.md) · [Cockpit](cockpit.md) · [Queue](queue.md) · [担当の台帳](agents/registry.md) · [運用](agents/protocol.md) · [未完の WS](workstreams-open.md) · [完了した WS](workstreams-closed.md) · [Guardrail](guardrail.md) · [設計方針と全体の決定](master-design-policy.md) · [Amaterrace](amaterrace.md) · [道具と実機](tools.md) · [Bug Board](known-bugs.md) · [Future Work](future-work.md) · [Past Log](history/index.md) · [plan の地図](README.md)

<!--
  「今の状態」の各 block は「master:<名前>:start」〜「master:<名前>:end」で丸ごと置き換えてよい（今の事だけ。経緯・日付の log・ユーザーの発言の原文は書かない）。
  全体に効く今も有効な決定は master-design-policy.md §11、Q1 の運用の決定は q1.md、WS の決定はその ws.md。
-->

## 今の状態

### 動いている担当と Queue

<!-- master:agents:start -->
（2026-11-11 22:10 の時点。詳しくは [cockpit.md](cockpit.md)）

| 担当 | Queue | 中身 |
| --- | --- | --- |
| Q1 | [WS347](ws347/ws.md) | VF2＋KC705 の Amaterrace GPU。Gemini の RTL の修正（amaterrace-gpu ad23fe01）の監査を、読むだけの監査の担当に任せている。次は Gemini への依頼 03 |
| Q1 | q2000 | [WS074](ws074/ws.md) [p263](ws074/phase263/phase.md)（実際の Web サイトの UAT の指摘、U1 は host で直った）。2026-11-03 から止まっている |
| P1 | q2013 | [WS333](ws333/ws.md) の UAPI の実装（p005 後半・p013・p019）（2026-11-12 から） |
| P2 | q2014 | [WS347](ws347/ws.md) Gemini の RTL の修正（依頼 05 の後）の監査（2026-11-12 から） |
| P1・P2 | q2010・q2011 | 2026-11-11 12:37 にユーザーの指示で止めた（uncleared）。P1 の amaterrace の worktree の RTL の変更は使わない（RTL は Gemini）。P2 の worktree `p2-ws347` に driver の未 commit の変更（未確認） |
| 他 | — | 動いていない。2026-11-02 のラップアップの引き継ぎが phase.md に残る: [WS335](ws335/ws.md) p035、[WS345](ws345/ws.md) p010・p014（p014 の cleared の判断は Q1）、[WS336](ws336/ws.md) p024 |

- main は未 push（push はユーザーの指示で、`git log origin/main..main` で数える）。
<!-- master:agents:end -->

### 統合の注意

<!-- master:merge:start -->
- build system は作り直した（top の Makefile と `mk/platform-<機種>/`、branch `refactor` は main に入った）。古い `platform/`・`config/ci/` の前提の道具・文書は追従が要る（[BUG-418](bugs/BUG-418.md)、noct の JIT は [BUG-419](bugs/BUG-419.md)）。Makefile・`mk/` の変更は概要と差分をユーザーに示してから（[makefile-style.md](makefile-style.md)）。
- WS074（ブラウザ）は Q1 が管理し、Claude の担当に割り当ててよい（Codex とクラウドのセッションは終わった）。Windows 版のブラウザ（WS334）と libbrowser の直しは P8 に寄せる（2026-11-02 の割り振り）。
- Amaterrace GPU（`~/amaterrace-gpu`）: Claude は仕様と RTL の監査と driver、RTL は Gemini（[amaterrace.md](amaterrace.md)）。
<!-- master:merge:end -->

### Focus と優先順位

<!-- master:focus:start -->
- **今の Focus（2026-11-11 ユーザー）**: [WS347](ws347/ws.md) — VF2＋KC705 で Amaterrace GPU を動かす。順は bitstream の書き込み → driver を最新の ghci.md に追従 → FPGA の動作確認（疎通、SFU を使わない範囲）。並行して Gemini の RTL を監査し、依頼で直させる。
- build system の作り直しの後始末（BUG-418・BUG-419）。
- 実装の進め方: 外部の標準仕様に基づく物と手順に分けられる大きな仕事は標準廉価実装手順（[docs/agent/spec-module-delegation.md](../docs/agent/spec-module-delegation.md)、Opus が設計、Sonnet 5.5 high が実装）。
- 実機の使い分け: デスクトップ（10.0.10.3）は GPU の driver、Keiland の app は VM で作り VF2 で速さを詰める、5330 は Intel GPU・電源・Alder Lake と USB の NIC、VF2＋KC705 は GHCI。
<!-- master:focus:end -->

<!-- master:priority:start -->
優先順位（2026-11-02 の整理のまま。WS347 の他はユーザーと見直す。依存の順とは別で、Queue の権限は変えない）:
1. 標準廉価実装手順の WS: WS326 → WS318 → WS329 → WS330、WS009 → WS296 → WS297、WS328 → WS074。
2. WS309 p015（ゲームモードの V2）、WS291 の NVIDIA の段、WS307 の残り。
3. WS221（Jumbo frame）・WS299（5330 の NVMe）: 担当の割り当ての時。
4. WS290・WS292・WS226: WS347 の進み次第。保留の WS: Q1 とユーザーの整理の後。
<!-- master:priority:end -->

### ユーザーの未決

<!-- master:open-decisions:start -->
- [WS347](ws347/ws.md): GHCI の仕様の問い（台帳 [ghci-rtl-questions.md](ws347/ghci-rtl-questions.md) の B）。B2（flat の時の VA）と B3（LDS の文）が残る。
- WS299 の継続（5330 が空いたら担当を割り当てる）。担当の外で見つけた物: clock_gettime に vDSO が無く 1 ms 刻み、main から return した時に stdout が flush されない、libc の memcpy の続き。
- P4 が拾った候補: WS162 を canceled にするか（WS172 に吸収）、WS279 p009（ライセンスと類似性の監査）の担当、P4m の時々の失敗（T1-691）の Bug、nvrtx.disp.own の既定、pci-msi-qemu.c を残すか、master-design-policy.md へ移す候補 約 25 件。
- WS282〜WS285: 実装を始める時期、再開の時の試験の PC（D11）と webcam（D12）。
- BUG-412（Pi 5 の click）: ユーザーの click の確かめで閉じる。BUG-345（Pi 5 の click が出ない窓）は再発したら再起動の前に Q1 に知らせてもらう。
- 以前からの物: UEFI の loader の GOP の mode の選び方、WS153 U2〜U15、記録に既にあるユーザーの発言の原文の扱い。
<!-- master:open-decisions:end -->

### 止まっている物

<!-- master:blocked:start -->
- ユーザーが拾える試験（頼まない）は [cockpit.md](cockpit.md) の一覧。
- `plan/records.json`（GitHub の同期の対応）に消した Phase の path が残る（同期は今使っていない）。
- GitHub への公開は push の指示の時だけ。
<!-- master:blocked:end -->

### 次の番号

<!-- master:next:start -->
BUG-422 · q2015 · WS348 · KL_VERSION 110（106 は BUG-396 の reload、107 は BUG-404 の題の帯、108 は BUG-413 の IPv6、109 は WS345 p013 の resize の hook）。使う前に衝突を確かめる（`ls plan/bugs`・`ls -d plan/ws*`・`grep q20 plan/queue.md`）。
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
