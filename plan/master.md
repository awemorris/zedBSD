<!-- awesome-plan project=zedbsd record=master -->

# zedBSD Master

[Queue](queue.md) · [Guardrail](guardrail.md) · [Future Work](future-work.md) · [Bug Board](known-bugs.md) · [Past Log](history/index.md) · [設定](config.md) · [Agent の運用](agents/protocol.md) · [Agent の台帳](agents/registry.md) · [GitHub Project](https://github.com/users/awemorris/projects/2)

<!--
  Q1 の操作盤。先頭（awesome-plan-current）は「今」だけを書き、各 block は「master:<名前>:start」〜「master:<名前>:end」で丸ごと置き換えてよい。
  block: updated・agents・merge・next・open-decisions・focus・blocked（先頭）、priority・outlook（本体）、decisions-log・history-log（末尾の付録、新しい物を block の先頭に足す）。
  置き換え: sed -i '/master:agents:start/,/master:agents:end/{//!d}' plan/master.md の後に sed -i '/master:agents:start/r new.md' plan/master.md。
-->
<!-- master:agents:start -->
- **2026-10-10 夜（Q1 の引き継ぎ）**: 体制は N=1（P1 だけ）＋T1。ベータ2 の残りは [plan/beta2.md](beta2.md) が正（毎回更新する、ユーザーの指示）。
  - P1: branch agent/p1、worktree /home/awe/zedBSD-worktrees/p1。WS199 p002 は cleared（77a40b51f）、P1 は使用量のためラップアップ済み。次の P1 は [WS199](ws199/ws.md) p003 から（phase.md に「すること・やり方」）→ → p004（T1 は 1 回にまとめる）→ [WS200](ws200/ws.md) p001。branch agent/p1-ws197（WS197 p003 の i03 の途中。ベータ2 の必須になったので WS200 の後に P1 が再開し、区切りごとに main へ merge）、agent/p1-p045 は main に merge 済み（2026-10-10）。
  - T1: branch agent/t1、worktree /home/awe/zedBSD-worktrees/t1。今は依頼なし。T1-435（Vulkan Video の 5330）はユーザーが top の config.mk で image を作り直した後に A〜E を SSH で（ESP に書かない、Claude Code の安全の判定で T1 の ESP の書き込みが拒否されたため）。build/t1-v・t1-vh2・t1-vh14 は残してある。
  - 使用量（2026-10-10 ユーザー: 週の残り 13%、水曜 6:00 に reset）: Q1 の turn を減らす、merge はまとめる、T1 は 1 回、plan はこまめに commit。
<!-- master:agents:end -->

### 統合と試験の待ち

<!-- master:merge:start -->
- main の先頭（2026-10-10 夜）: WS199 i01〜i03（d8cb16814、KL_VERSION 77）、BUG-283・284・285・286 の直し、BUG-275（USB の zero-bandwidth の endpoint、Bluetooth）、BUG-222 の TCP の並び替え 44・ifconfig の media、rtld の dlopen の path、Noct 2.0.3。
- 5330（10.0.30.3、zedBSD の単独起動）: SSH は `sshpass -p kei ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o PubkeyAuthentication=no kei@10.0.30.3`（鍵は今の image に無い）、sudo は `echo kei | sudo -S`。image は 10/09 の a42a544 以降の userland＋Q1 が入れた BUG-275 の kernel（ESP の vmunix、前は vmunix.old）。WS199 i02 以降（NFC・Security Keys の頁）は入っていない。/tmp/fidoctl.new に新しい fidoctl（ユーザーが鍵の up=false の確かめを流す、手順は Q1 が会話で渡した）。
- ユーザーの判断待ち: passkey-fido2 だけを 5330 に入れ替えて NFC の login を試すか、image を作り直すか。fidoctl の `-s assert` の結果（WS199 p002 の前提）。
<!-- master:merge:end -->

### Q1 の次の手順

<!-- master:next:start -->
1. 新しい Q1 は AGENTS.md・plan/beta2.md・この block・plan/ws199/ws.md・plan/ws200/ws.md を読む。P1・T1 の agent は会話に紐づくので、新しい session では P1・T1 を新しい世代で起動する（P1 は agent/p1 の worktree で WS199 p002 から、phase.md に「すること・やり方」）。
2. merge は `source plan/tools/merge_one.sh && merge_one SHA`（merge だけを 1 つの Bash の呼び出しに、memory の規則）。T1 の台帳の番号は Q1 が振る（次は T1-523）。Bug の次の番号は BUG-287、Queue は q923、WS は WS202。
3. 日程: 10/13 凍結の目標 → 10/14 RC・最終回帰（WS129 p006）→ 10/16 公開の準備（WS129 p008）、公開はユーザーの指示。Vulkan Video は T1-435 が PASS なら release の config に `ZEDBSD_BOOT_EXTRA_LINES += i915.debug=video`、でなければ OFF のまま。
<!-- master:next:end -->

### ユーザーの未決の判断

<!-- master:open-decisions:start -->
- **WS084 の 10 回の reboot（素の起動）**: ユーザーが zedBSD で起動する時。
- WS153 U2〜U15 はユーザーが検討中（聞かない）。
- （解決 2026-10-08）WS005（ネットワークと WiFi）: ユーザー「記録のミス、とっくに完了」→ completed。
- （解決 2026-10-08）規約の全文の見直しの Phase: ユーザー「コーディング規約による整形はベータ3でやります。」→ 各 WS の規約の Phase はベータ3 へ（ベータ2 の WS の完了の条件から外す）。
- （解決 2026-10-08）WS172（passkey の認証の枠組み、PIN・FIDO2・TPM）: ユーザー「ベータ3に回します。」→ p004 の survey の review もベータ3 で。lock の画面の方式の選択は WS187 p003 でベータ2。
- （解決 2026-10-08）電源ボタンのメニュー（WS182 D1）: 「現状ではオーケーです」。追加の要望 → WS187（lock の大きな時計）・ws172-p007（PIN・Password・Hardware Key の選択）。
- （解決 2026-10-08）2 番目以降の display の dock bar: 「その画面に置いた window の window icon を出し、時計・状態・App Home・切り替えのつまみも表示する」（ws113-p015）。
<!-- master:open-decisions:end -->

### Focus

<!-- master:focus:start -->
- **fg019 ベータ2 の公開（10/17、RC 10/13）**。優先順（2026-10-07 ユーザー）: Settings Display（WS113）→ USB-C/DP Alt（WS051・WS050、BUG-256）→ Vulkan Video（WS083）→ widget（WS090）→ 通知（WS156）→ touchpad の割り込み・タップ（WS183）→ YubiKey（WS161）→ 写真（WS157）→ カレンダー（WS155）→ Bluetooth（WS143）→ 残り。空き時間だけ: IME（WS095）・RTL8822C（WS186）・Vulkan executor（WS031）・i915 の高度化（WS075、shader の compiler を含む）。ベータ3 と 10/13 以降の物は decisions-log。
<!-- master:focus:end -->

### 止まっている物

<!-- master:blocked:start -->
- 5330 は zedBSD の単独起動中（Linux の作業 T1-378・TPM2 の表は Linux に戻した時）。5320 は電源オフ（WS183 p001・BUG-249・BUG-247 の実機の確認待ち）。
- BUG-256（USB-C の AUX）: 診断の kernel の入れ替えと monitor の挿し込み待ち。
- WS074 のレンダリングの改善: ユーザーの指示まで止める。WS153: U2〜U15 のユーザーの判断まで止める。
- GitHub への記録の公開は保留（.sync が無い）。push はユーザーの指示の時だけ。
<!-- master:blocked:end -->

<!-- awesome-plan-current:end -->

## 目的・利用者・最終成果

- **目的**: 寛容なライセンスで企業が自由に使える UNIX 互換 OS を、GPL の Linux kernel に依存せずに作る。
- **利用者**: OS を組み込んで独自のディストリビューションを作る開発者・企業と、デスクトップ・ラップトップ・SBC で使う個人。
- **最終成果**: Linux/Android を置き換えられる水準のカーネルとユーザランド、最小の HAL による移植契約、用途別に構成・配布できる仕組み。
- **範囲**: kernel、HAL、driver、libc、base の userland、デスクトップ（zdesktop）、外部 package のクロスビルド、インストーラ、文書。
- **範囲外**: Linux の kernel ABI・DRM の互換、Mesa 流の user mode driver、正式な UNIX 認証・Vulkan CTS 認証の取得（主張しない）。
- **制約**: HAL の変更は差分ごとの事前承認（[Guardrail](guardrail.md)）。独立実装とライセンスの境界（[設計方針](master-design-policy.md)）。

## Objectives

- **O1**: 寛容なライセンスで企業が自由に使いやすい UNIX 互換システムを、GPL の Linux kernel に依存せず、Linux/Android を置き換え可能な水準で提供する。
- **O2**: デスクトップ、ラップトップ、SBC、タブレット、モバイルなど様々な規模で動くカーネルとユーザランドを提供し、開発者が独自ディストリビューションを自由にカスタマイズ・リブランディング・配布できるようにする。
- **O3**: UNIX/BSD/Linux の遺産から現代のシステムに必要なエッセンスを抽出し、networkd、netconf、service などをシンプルで一貫した仕組みとして再実装する。
- **O4**: ページベース MMU を備える 32bit/64bit コンピュータへ UNIX 互換 OS を確実に移植できる、明確で最小限の HAL を定義し、人類の共有知とする。
- **O5**: AI 時代の OSS のあり方を、大規模な AI 活用開発を通じて探索し、成果・失敗・人間の判断を再利用可能な知見として共有する。

## Milestone Goals

Milestone の達成は所属 WS の完了数ではなく、到達点の証拠で判定する。現時点で completed の Milestone は無い。

| Milestone | Objective | 受け入れの核 | 進捗 | Primary WS |
| --- | --- | --- | --- | --- |
| **MG001** 継続開発できる基盤 | O4, O5 | 文書化した環境で build でき、設計境界・規約・試験・制限を追跡できる | toolchain（WS021）・build tool（WS010）・x86 HAL の規約（WS023）は完了。文書（WS009）と試験資産の整理（WS026）が残る。テスト配置の整理はWS106で計画。vmunix の LTO（WS053）は完了 | WS009, WS010, WS021, WS023, WS026, WS047, WS053, WS106 |
| **MG002** UNIX アプリケーションの実行基盤 | O1 | process・memory・libc・loader/TLS の対応範囲を互換性台帳と代表アプリで確認できる | TLS（WS022）と外部 package の導入（WS032）は完了。base の utility の POSIX 化（WS043）は完了。POSIX 台帳（WS001）、アプリ導入（WS034）、sh（WS042）が進行中 | WS001, WS022, WS032, WS034, WS042, WS043, WS045, WS046, WS061, WS115, WS116 |
| **MG003** 対象機へ導入して単独起動 | O2, O4 | 合意した機種・媒体でインストール後の単独起動と login を確認できる。実機と QEMU の証拠を分ける | インストーラ（WS019）と Intel Mac（WS020）は完了。4 機種の実機受け入れ（WS028）が残る | WS003, WS004, WS019, WS020, WS028 |
| **MG004** データの保持とメモリ/ストレージの実用 | O1, O2 | 永続化、低メモリ時の進行、媒体世代、既定構成の性能を確認できる | swap（WS016）、UFS（WS024）、I/O・cache（WS025）は完了。実機の性能の一部は未測定。UFS の directory は 12 block まで育つ（WS054、完了） | WS016, WS024, WS025, WS054, WS057, WS058, WS059, WS060 |
| **MG005** 一貫したネットワーク/サービス管理 | O1, O2, O3 | networkd・netconf・service の責務・設定・操作が一貫し、永続化と失敗後の復旧を確認できる | サービス（WS002）、net console（WS011）、service console（WS012）は完了。有線 LAN の常駐管理（WS005・WS033）が残る | WS002, WS005, WS011, WS012, WS033 |
| **MG006** グラフィカルな操作環境 | O2 | 入力・描画・ウィンドウ・端末・GUI ツールの一連の操作を確認できる | 入力（WS006）、Noct/BeUI（WS008）、標準 Vulkan（WS030）、即時起床（WS041）は完了。**Wayland デスクトップ（WS035）が fg010 の中心**。WS104 の OS 境界は A1〜A6 と全体回帰で完了、LinuxのWS105はL1〜L9・全文規約/両OS最終回帰でcompleted（fg012達成、既知resizeはユーザー許可のtracking）。WS109 nativeFreeBSD15.1/実i915/主要app/backend/全文規約がq572でverified、q574実機build/installとp008ユーザー実操作でF6受け入れ合格。WS113複数displayとWS114 Linux標準GTK4互換性は計画のみ。デモ実機を含むMG006全体は未完了 | WS006, WS007, WS008, WS014, WS017, WS029, WS030, WS031, WS035, WS037〜WS039, WS041, WS068, WS104, WS105, WS107, WS109, WS113, WS114 |
| **MG007** 用途別の独自ディストリビューション | O1, O2 | 第三者が用途別に構成し、独自ブランドで build・配布できる | 担う作業は一部だけ（WS013・WS015 は Future Work に保留）。WS105独立/opt build・installとWS108の2OS native deb/QEMU検証・CI/release定義をverified、WS109 nativeFreeBSD独立build/install/privateprefixもverified、5OS package/CI配布はWS112で計画のみ、MG007全体は未充足 | WS013, WS015, WS108, WS112 |
| **MG008** 最小 HAL の移植契約と異種機での実証 | O4 | HAL 契約・移植手順と異種/レトロ機での実証を公開する | source の所有の整理（WS018）と時間の単位（WS040）は完了。他 platform への反映（WS036、aarch64 を含む）と PowerPC（WS027）、rpi4 の開発環境（WS044）が残る | WS018, WS027, WS036, WS040, WS044 |
| **MG009** AI 活用 OSS 開発の知見の公開 | O5 | 設計権限・レビュー・変更追跡・失敗からの回復の事例と根拠を公開する | 担う作業が未定義 | なし |

## Current Focused Goals

| Goal | 当面の成果 | Milestone | 担当の WS | 出典 |
| --- | --- | --- | --- | --- |
| **fg019** | **最初の公開ベータ＝ベータ2 のリリース（目標 2026-10-17、2026-10-06 夜 ユーザー「ベータ1は難なく前倒しできるので、実際には最初のベータはベータ2で、2026年10月17日に公開するのはベータ2に変更です。」）**。旧: ベータ1 のリリース。凍結なし、できた所までをベータ1 にし、安定化は後のベータの版で。版と tag は `zedbsd-0.1.0-beta1`、CI が Prerelease を作りユーザーが動作確認して Latest に手で昇格、配布物は USB の image と Windows の QEMU/Venus の zip | MG003・MG006・MG007・MG002 | 下の「fg019 の内容」 | 2026-10-02 user「次のFeature Goalはベータ1のリリースにします」ほか |
| **fg018** | Linux 標準 GTK4 の実測と互換性の改善 | MG006 | [WS114](ws114/ws.md) | 2026-10-02 user。p007・p008 まで達成 |

### fg019 の内容（2026-10-02〜03 のユーザーとの議論で決定）

| 区分 | WS | 状態・メモ |
| --- | --- | --- |
| デスクトップの基盤 | [WS099](ws099/ws.md)（BUG-125 は blocking）、[WS094](ws094/ws.md)、[WS114](ws114/ws.md) p008 | BUG-125 の原因と修正は済み、BUG-147 の試験の頑健化の後に閉じる |
| ネットワーク（WiFi を含む） | [WS005](ws005/ws.md)、[WS033](ws033/ws.md) | WiFi は network group に制御を許可、自動再接続は起動時（system の store）と login（利用者の store） |
| IME 日本語 | [WS095](ws095/ws.md) | 辞書・inline の変換中の文字・候補の窓・右上の status は済み |
| desktop の構造 | [WS131](ws131/ws.md)（libkeiland-backend と libkeiland、libkeiui の吸収）、[WS132](ws132/ws.md)（PnP の通知） | WS131 は移行計画のレビュー待ち |
| 標準 app（WS131 の後に組み直す） | [WS127](ws127/ws.md) Files（最重点）、[WS089](ws089/ws.md) Settings（重点）、[WS128](ws128/ws.md) 他の app、[WS120](ws120/ws.md) 音楽（m4a） | 日本語 UI はベータ2 以降（F-068） |
| 複数 display | [WS113](ws113/ws.md) | D-ATOMIC は (a) で決定、WS131 の後 |
| 対象 platform | Latitude 5330、[WS118](ws118/ws.md) Latitude 5320 | 5320 は RTL8156 の USB の LAN で遠隔の log |
| インストーラ | [WS119](ws119/ws.md) | Wayland、disk 全体のみ、UEFI のみ。WS118 の後 |
| packages | [WS124](ws124/ws.md) Emacs（端末版）、[WS125](ws125/ws.md) vim、[WS126](ws126/ws.md) Python 3（core） | release の image に入れる |
| GTK4 | [WS115](ws115/ws.md) | 素の GTK4 の移植は後回し（p010 の ld.so の上限から再開） |
| 動画（drop 可、別セッションでユーザーと） | [WS083](ws083/ws.md) | WS122（P2 が 2026-10-05 に実装）・WS121（2026-10-05 Q1 の担当）は Q1 |
| リリース作業 | [WS129](ws129/ws.md) | — |
| 最後 | [WS112](ws112/ws.md) Linux の package | 優先度最下位 |
| ブラウザ（Q1、2026-10-05 ユーザー） | [WS074](ws074/ws.md)（レンダリングの改善はユーザーの指示まで停止、専任の B1 を設計済み）・[WS121](ws121/ws.md) 動画 | WS107 completed |

### 達成した Focused Goal

| Goal | 成果 | 達成 |
| --- | --- | --- |
| fg010 | 2026-10-17 の OSC のデモの Kei Operating System（Latitude 5330） | 2026-10-02 user 判断（実装で到達、nightly の release で公開） |
| fg012 | Keiland の OS の境界の整理と Linux の Keiland（[WS104](ws104/ws.md)・[WS105](ws105/ws.md)） | 2026-10-01 |
| fg013 | test・見本を userland/tests へ（[WS106](ws106/ws.md)） | 2026-10-01（WS106 は残りあり） |
| fg014 | libbrowser の component 化（[WS107](ws107/ws.md)） | 2026-10-02 |
| fg015 | Debian・Ubuntu の native の deb と CI（[WS108](ws108/ws.md)） | 2026-10-02 |
| fg016 | native FreeBSD15 の Keiland（[WS109](ws109/ws.md)） | 2026-10-02 |

以前の詳しい記録（fg010 のデモの台本・判断、各 WS の実行優先の経緯）は [master の旧版](history/master-2026-10-03-before-rewrite.md) と git の履歴にある。

## WS の優先順位

依存による実行の順とは別のもの。Queue の権限は変えない。古い順位（2026-10-03〜04）は付録の history-log。

<!-- master:priority:start -->
2026-10-06 夜 ユーザーの作業の順（原文の要旨）:「ベータ1のWS＋ベータ2の一部実装をまずすべて実装してください。P1,P2でフルに実装してしまってください。デバッグ項目としてカウントされているものを除きます。正常系が通ればいいです。準正常系と異常系で未実装な部分は、そういった積み残しを管理するWSを作って、そこにPhaseとして積みましょう。それはすべての実装が1パス通って疎通確認できてからでいいです。」「そのあとで、デバッグに専念して、すべてのバグを消化します。」

- **規則**: 正常系だけを実装して疎通を確かめる。準正常系・異常系の未実装は各 phase.md の「積み残し」の節に書いておき、全ての実装の 1 パスの後に、積み残しを管理する新しい WS に Phase として集める。デバッグとして数えた項目（Bug・実機の確認）は除く。
1. **第 1 段**（今）: WS131 p018〜p026（app を kl_app へ）、WS089 Settings の残り、WS128 標準 app の仕上げ（PDF の検索）、WS127 Files の残り、WS132 /dev/system・PnP・自動 mount、WS099 compositor の残り、WS159 native の touchpad（実装の分）、WS090 widget・Mahora（p023 を含む）、WS129 ベータ1 の release 作業、WS094 desktop の icon、WS095 IME の残り。desktop・compositor（WS113 複数 display、WS156 通知、WS102・WS110・WS138・WS142・WS164・WS078）。security・system（WS161 YubiKey、WS172 passkey、WS148・WS149・WS151。WS152 はベータ3 へ、2026-10-06 夜 ユーザー）。IME・言語（WS165 手書き、WS154 SKK、WS166 予測変換、WS158 翻訳）。
2. **第 2 段**: WS139 desktop の速さ（最適化、2026-10-06 夜 ユーザー: 第 1 段から移す）、app（WS175 PDF の編集、WS169 メール、WS120 音楽、WS122 動画、WS121・WS145・WS157・WS170、WS079、WS155）、WS130 IPv6 の残り（2026-10-06 夜 ユーザー: 第 3 段から移す）、WS009 文書、WS112 Debian の package（3 つの deb、2026-10-06 夜 ユーザー）、WS117（Qt6 の Linux の互換、ユーザーが検討中）。
3. **第 3 段**（不確実性のある hardware 関連、2026-10-06 夜 ユーザー）: WS083 Vulkan Video（第 2 段から移す）、kernel・driver・電源（WS031、WS052、WS051、WS075、WS050、WS167、WS084）、base・libc・試験（WS001、WS168 の残り、WS173、ほか）。続けて [WS179](ws179/ws.md)（アクセントカラー、2026-10-07 ユーザー）、最後に低い優先度で [WS178](ws178/ws.md)（OpenGL を Desktop へ、GLX を xserver へ、2026-10-07 ユーザー）。
4. **[WS177 ベータ2 積み残し](ws177/ws.md)**（2026-10-06 ユーザー「積み残しWSはベータ2積み残しという形でWSを作りましょう。」）に準正常系・異常系を Phase として集める。
5. **デバッグに専念**して全ての Bug を消化する。
- ベータ3 へ: WS068・WS101・WS171・WS176・WS152・WS115 GTK4・WS116 Qt6・WS126 Python（2026-10-06 夜 ユーザー「私たちのOSの価値は先進的なタブレットとデスクトップの融合したUI/UXであって、既存のデスクトップUIのツールキットはコアコンピタンスではない」「私たちには、私たちがよいと考えるスクリプト言語であるNoctがすでにある」）。ベータ4 以降（2026-10-05 ユーザー、Q1 の表の誤りを訂正）: WS112・WS124・WS125・WS143・WS118・WS119 ほか。止める: WS074 の描画（B1）・WS153（U2〜U15）。
<!-- master:priority:end -->

## 工数の見積もり（残り、LW）


**2026-10-06 夜の見直し（ベータ2 まで、ユーザーの依頼）**: 設計・実装 186.6 LW、デバッグ 20.8 LW、合計 207.4 LW（1 LW＝実時間 20 分で 69.1 時間、N=2 で 34.6 時間）。止まっている WS074・WS153 の 18 LW を足すと 225.4 LW（75.1 時間、N=2 で 37.6 時間）。2026-10-06 に進んだ分（ws142 p007〜p010、ws099 p034b〜p037、ws090 p017〜p019、ws095 p005〜p007、ws130 p001〜p003、ws168 p001〜p002、ws131 p015〜p017、ws175 p001〜p006 など）を引いた値。内訳は session の回答（下の表は 2026-10-05 の値のまま）。
2026-10-05 Q1（ユーザー「各WSの工数見積もりをしてほしいです。あなたの1週間かかるという見積もりは、1LW (logical weeks)と表現してほしいです。master.mdに見積もりを書き込んでおいてほしいです。」）。

- **LW（logical week）**: Q1（Claude）が「1 週間かかる」と見積もる作業の量を 1 LW とする。人の暦の週とは違う。実際には **1 LW ≈ 実時間 1〜2 時間**（2026-10-05 ユーザーの観察、担当 1 人あたり）。各 WS の**残り**の量（完了した Phase を除く）。completed の WS は 0 で表に載せない。
- 粗い見積もり（±50%）。判断待ち・実機・外部の service に依る物は、その待ちを含まない。検討だけの WS（WS148・149・151）は検討の分だけで、実装は結論の後に見積もる。WS074 は 2026-10-05 から Q1（12 LW、レンダリングの改善は停止中）。WS013・WS015 は Future Work に保留中。
- 合計 **394.7 LW**（100 WS）。Milestone ごと: MG001 8.5、MG002 90.3、MG003 17.3、MG004 3、MG005 19、MG006 219.1、MG007 26.5、MG008 11。（2026-10-05 Q1 の注: この合計と段の表は 2026-10-05 の段の移動・WS の追加の前の値で古い。WS ごとの表の「段」の列が正。WS ごとの表の合計は 430.4 LW（112 行）。段の表は 2026-10-05 夜に作り直した、下の「リリースの段ごとの見積もり」）

| WS | 段 | Milestone | 残り（LW） | 見積もりの中身 |
| --- | --- | --- | --- | --- |
| [WS001](ws001/ws.md) | ベータ2（2026-10-05 移動） | MG002 | 3 | POSIX 台帳の残り（p040 以降の utility） |
| [WS004](ws004/ws.md) | ベータ2 | MG003 | 2 | NVMe の実機・転送・driver の共通化 |
| [WS005](ws005/ws.md) | 完了（2026-10-08） | MG005 | 0 | 完了（ユーザーの判断）。WiFi の UI の Bug は Bug Board |
| [WS007](ws007/ws.md) | キャンセル | MG006 | 0.5 | p004 の再現条件と amd64 の残件 |
| [WS009](ws009/ws.md) | ベータ2 | MG001 | 2 | GPU の文書ほか |
| [WS013](ws013/ws.md) | 保留（Future Work） | MG007 | 4 | CPAR（Future Work に保留中） |
| [WS014](ws014/ws.md) | ベータ2 | MG006 | 0.5 | p004 の最終 API と規約 |
| [WS015](ws015/ws.md) | キャンセル | MG007 | 10 | μITRON（Future Work に保留中） |
| [WS017](ws017/ws.md) | キャンセル | MG006 | 1 | LFB の高速化 |
| [WS026](ws026/ws.md) | ベータ2（2026-10-05 移動） | MG001 | 1.5 | 試験資産の整理 |
| [WS027](ws027/ws.md) | キャンセル | MG008 | 8 | PowerPC 移植 |
| [WS028](ws028/ws.md) | キャンセル | MG003 | 1.5 | インストーラの実機（4 機種） |
| [WS029](ws029/ws.md) | ベータ2 | MG006 | 3 | cold VFIO attach の停止ほか |
| [WS031](ws031/ws.md) | ベータ2（2026-10-05 移動） | MG006 | 8 | p015〜p048 の native Vulkan 実行器 |
| [WS033](ws033/ws.md) | ベータ1 | MG005 | 1 | USB LAN の後挿し（BUG-168・169）・抜き差しの実機 |
| [WS034](ws034/ws.md) | キャンセル | MG002 | 4 | package の導入と kernel・libc の是正 |
| [WS037](ws037/ws.md) | 要検討（ブロック） | MG006 | 16 | nvrtx（文書の後、GSP の起動・channel・display・executor） |
| [WS038](ws038/ws.md) | 要検討（ブロック） | MG006 | 16 | Intel Arc dGPU |
| [WS039](ws039/ws.md) | 要検討（ブロック） | MG006 | 20 | AMD RDNA |
| [WS044](ws044/ws.md) | 要検討（ブロック） | MG008 | 1.5 | rpi4 の font・FAT32・lldb |
| [WS045](ws045/ws.md) | ベータ1 | MG002 | 0 | 完了（2026-10-05） |
| [WS046](ws046/ws.md) | ベータ2（2026-10-05 移動） | MG002 | 1.5 | GNU 互換の make |
| [WS047](ws047/ws.md) | キャンセル | MG001 | 3 | build.sh と Noct の build system |
| [WS048](ws048/ws.md) | 要検討（ブロック） | MG008 | 1.5 | rpi4 の USB |
| [WS049](ws049/ws.md) | ベータ1 | MG003 | 0.3 | 実機の UAT（p007・p008・p016） |
| [WS050](ws050/ws.md) | ベータ2 | MG003 | 3 | UCSI（1.x・2.x、role の切替・Alt Mode の選択を含む） |
| [WS051](ws051/ws.md) | ベータ2 | MG006 | 4 | USB-C の DP Alt Mode（i915 の Type-C） |
| [WS052](ws052/ws.md) | ベータ3（2026-10-07 ユーザー） | MG003 | 6 | 電源管理（S0i3） |
| [WS061](ws061/ws.md) | 完了 | MG002 | 1 | expat の configure・compile |
| [WS066](ws066/ws.md) | ベータ2 | MG002 | 1 | 動的 link の起動の高速化 |
| [WS068](ws068/ws.md) | ベータ3（2026-10-06 ユーザー: いったん先送り） | MG006 | 4 | EGL・OpenGL ES |
| [WS073](ws073/ws.md) | ベータ1 | MG002 | 3 | Bug Board の掃討（UAT の Bug を含む） |
| [WS074](ws074/ws.md) | ベータ3（2026-10-08 ユーザー） | MG006 | 12 | Web ブラウザ（2026-10-05 Q1 に戻した、レンダリングの改善はユーザーの指示まで止める） |
| [WS075](ws075/ws.md) | ベータ2 | MG006 | 4 | i915 の高度化 |
| [WS077](ws077/ws.md) | キャンセル | MG001 | 1 | PC-98 の PCI |
| [WS078](ws078/ws.md) | ベータ2 | MG006 | 1 | Kei の名前の移行 |
| [WS079](ws079/ws.md) | ベータ2 | MG006 | 1 | Notes・PDF Viewer の残り |
| [WS080](ws080/ws.md) | 要検討（ブロック） | MG002 | 4 | ld.coff（PE/COFF の動的ローダ） |
| [WS081](ws081/ws.md) | ベータ1 | MG006 | 1.5 | touch の質（BUG-156・166・167・190 を含む） |
| [WS082](ws082/ws.md) | アイディアの Phase（議論の後） | MG002 | 1 | /dev/kvm の移植の検討（移植の本体は別） |
| [WS083](ws083/ws.md) | ベータ2 | MG006 | 8 | Vulkan Video（H.264） |
| [WS084](ws084/ws.md) | ベータ2 | MG006 | 1.5 | i915 の firmware の画面の引き継ぎ |
| [WS085](ws085/ws.md) | ベータ2 | MG006 | 1 | Windows 版 QEMU の Venus |
| [WS088](ws088/ws.md) | ベータ2 | MG006 | 0.5 | Windows の nightly zip |
| [WS089](ws089/ws.md) | ベータ1（2）＋ベータ2（4） | MG006 | 6 | Settings（p021〜p026、Wi-Fi の Bug、p015〜p017） |
| [WS090](ws090/ws.md) | ベータ1 | MG006 | 1.5 | widget の library（p016 の chooser を含む） |
| [WS094](ws094/ws.md) | ベータ1 | MG006 | 0.5 | desktop の file の icon |
| [WS095](ws095/ws.md) | ベータ1 | MG006 | 1.5 | IME（p005、p016 app ごとの状態） |
| [WS096](ws096/ws.md) | キャンセル | MG002 | 30 | Qt6 の互換の書き下ろし |
| [WS097](ws097/ws.md) | キャンセル | MG002 | 30 | GTK4 の互換の書き下ろし |
| [WS098](ws098/ws.md) | 要検討（ブロック） | MG006 | 6 | IME のニューラル化 |
| [WS099](ws099/ws.md) | ベータ1 | MG006 | 3 | compositor（q700 の窓の Bug、p031 bar の高さ、p032） |
| [WS100](ws100/ws.md) | ベータ1 | MG006 | 0.5 | 音量（BUG-170 の実機の確認） |
| [WS101](ws101/ws.md) | ベータ3（2026-10-06 ユーザー: いったん先送り） | MG006 | 6 | GPU の compute |
| [WS102](ws102/ws.md) | ベータ2 | MG006 | 2 | スクリーンキーボード |
| [WS106](ws106/ws.md) | ベータ2 | MG001 | 1 | test app の集約 |
| [WS110](ws110/ws.md) | ベータ2 | MG006 | 0.5 | compositor の通常起動を既定に |
| [WS112](ws112/ws.md) | ベータ2（2026-10-05 移動） | MG007 | 1.5 | Linux 5 種類の package と CI |
| [WS113](ws113/ws.md) | ベータ2 | MG006 | 5 | 複数 display・Settings の Display の頁・明るさ・p010 |
| [WS114](ws114/ws.md) | ベータ2 | MG006 | 1.5 | Linux の GTK4 の互換 |
| [WS115](ws115/ws.md) | ベータ2（2026-10-05 移動） | MG002 | 3 | upstream GTK4 の移植（p010 から） |
| [WS116](ws116/ws.md) | ベータ2（2026-10-05 移動） | MG002 | 4 | upstream Qt6 の移植 |
| [WS117](ws117/ws.md) | ベータ2（2026-10-05 移動） | MG006 | 1.5 | Linux の Qt6 の互換 |
| [WS118](ws118/ws.md) | 要検討（ブロック） | MG003 | 1.5 | Latitude 5320 |
| [WS119](ws119/ws.md) | アイディアの Phase（議論の後） | MG003 | 3 | インストーラの作り直し |
| [WS120](ws120/ws.md) | ベータ2（2026-10-05 移動） | MG006 | 5 | 音楽（service との連携と local の collection） |
| [WS121](ws121/ws.md) | ベータ2（2026-10-05 移動） | MG006 | 4 | browser の動画の再生の支援 |
| [WS122](ws122/ws.md) | ベータ2 | MG006 | 6 | 動画の player（libavcodec の段 → 独自 container の段） |
| [WS123](ws123/ws.md) | キャンセル済み | MG006 | 0 | キャンセル済み |
| [WS124](ws124/ws.md) | ベータ2（2026-10-05 移動） | MG002 | 1.5 | Emacs の package |
| [WS125](ws125/ws.md) | ベータ2（2026-10-05 移動） | MG002 | 0.5 | vim の package |
| [WS126](ws126/ws.md) | ベータ2（2026-10-05 移動） | MG002 | 2 | Python 3 の package |
| [WS127](ws127/ws.md) | ベータ1 | MG006 | 3 | Files（p003 以降、p010〜p012） |
| [WS128](ws128/ws.md) | ベータ1 | MG006 | 2 | 標準 app の仕上げ |
| [WS129](ws129/ws.md) | ベータ1 | MG007 | 1 | ベータ1 の release 作業 |
| [WS130](ws130/ws.md) | ベータ2（2026-10-05 移動） | MG005 | 8 | IPv6 |
| [WS131](ws131/ws.md) | ベータ1 | MG006 | 3 | libkeiland-backend の残り（p015〜p022） |
| [WS132](ws132/ws.md) | ベータ1 | MG006 | 3 | /dev/system の電源・PnP の通知・自動 mount・eject |
| [WS134](ws134/ws.md) | ベータ1 | MG006 | 1 | System Monitor の実機の残り |
| [WS138](ws138/ws.md) | ベータ2 | MG006 | 0.5 | 背景の PNG |
| [WS139](ws139/ws.md) | ベータ2 | MG006 | 2 | desktop の速さ |
| [WS140](ws140/ws.md) | ベータ2 | MG002 | 0.5 | ld.so の上限の動的化 |
| [WS141](ws141/ws.md) | 別 session で実行（2026-10-09 ユーザー、ws141/ は その session が排他的に更新、patch は Q1 が merge） | MG006 | 10 | Raspberry Pi 4 の GPU（N0 から V8・TFU まで） |
| [WS142](ws142/ws.md) | ベータ2 | MG006 | 3 | アプリの切り替え（bar・Alt+Tab・gesture） |
| [WS143](ws143/ws.md) | ベータ2（2026-10-05 移動） | MG006 | 8 | Bluetooth |
| [WS144](ws144/ws.md) | アイディアの Phase（議論の後） | MG005 | 8 | VPN |
| [WS145](ws145/ws.md) | ベータ2（2026-10-05 移動） | MG006 | 4 | 印刷 |
| [WS146](ws146/ws.md) | アイディアの Phase（議論の後） | MG006 | 6 | SSH の独自のオンラインストレージ（WS150 の後） |
| [WS147](ws147/ws.md) | アイディアの Phase（議論の後） | MG006 | 4 | OneDrive（WS150 の後） |
| [WS148](ws148/ws.md) | ベータ2 | MG006 | 0.3 | Privacy の頁の検討（実装は結論次第） |
| [WS149](ws149/ws.md) | ベータ2 | MG006 | 0.3 | Security の頁の検討（実装は結論次第） |
| [WS150](ws150/ws.md) | 要検討（ブロック） | MG004 | 3 | FUSE に当たる枠組み |
| [WS151](ws151/ws.md) | ベータ2 | MG006 | 0.5 | Accessibility の頁の検討（実装は結論次第） |
| [WS152](ws152/ws.md) | ベータ2（2026-10-05 移動） | MG007 | 4 | system の更新 |
| [WS153](ws153/ws.md) | ベータ2（2026-10-05 移動） | MG007 | 6 | app の repository と Apps の頁 |
| [WS154](ws154/ws.md) | ベータ2 | MG006 | 3 | Languages の頁と SKK の IME |
| [WS155](ws155/ws.md) | ベータ2 | MG006 | 2.5 | カレンダー（外観の画像の後） |
| [WS156](ws156/ws.md) | ベータ2 | MG006 | 2 | app の通知 |
| [WS157](ws157/ws.md) | ベータ2（2026-10-05 移動） | MG006 | 4 | 写真の管理 |
| [WS158](ws158/ws.md) | ベータ2 | MG006 | 3 | 翻訳（日本語はベータ2） |
| [WS159](ws159/ws.md) | ベータ1 | MG006 | 5 | native のタッチパッド（I2C-HID）と compositor の touchpad の層 |
| [WS160](ws160/ws.md) | ベータ1 | MG002 | 0.5 | su・sudo・passwd の残り（QEMU の確かめ） |
| [WS161](ws161/ws.md) | ベータ2 | MG006 | 4 | YubiKey（USB の FIDO2 → NFC の CTAP2）（2026-10-05 追加） |
| [WS162](ws162/ws.md) | WS172 に吸収（2026-10-05） | MG006 | 0 | FIDO2 の login（2026-10-05 追加） |
| [WS163](ws163/ws.md) | WS172 に吸収（2026-10-05） | MG006 | 0 | 数字 6 桁の login（2026-10-05 追加） |
| [WS164](ws164/ws.md) | ベータ2 | MG006 | 1 | 起動時の Welcome の画面（2026-10-05 追加） |
| [WS165](ws165/ws.md) | ベータ2 | MG006 | 4 | 手書きの入力（段 1）（2026-10-05 追加） |
| [WS166](ws166/ws.md) | ベータ2 | MG006 | 3 | 予測変換（2026-10-05 追加） |
| [WS167](ws167/ws.md) | ベータ2 | MG006 | 2 | GPU の command の protocol の独自化（2026-10-05 追加） |
| [WS168](ws168/ws.md) | ベータ2 | MG006 | 3 | プレビューの隔離された command（2026-10-05 追加） |
| [WS169](ws169/ws.md) | ベータ2（2026-10-05 決定） | MG006 | 6 | メーラの app と compositor のメールの API（2026-10-05 追加） |
| [WS170](ws170/ws.md) | ベータ2（2026-10-05 決定） | MG006 | 4（最初の範囲） | Phone の app、連絡先からタイムラインまで（2026-10-05 追加） |
| [WS171](ws171/ws.md) | ベータ3（2026-10-06 ユーザー: いったん先送り） | MG008 | 2 | hal.h の全ての関数に契約の comment（2026-10-05 追加、急がない） |
| [WS172](ws172/ws.md) | ベータ2 | MG006 | 4 | passkey の認証の枠組み（/sbin/passkey・/etc/passkey、2026-10-05 追加） |
| [WS173](ws173/ws.md) | ベータ2 | MG006 | 3 | AAT（エージェントが素の実機を操作する受け入れの枠組み、2026-10-05 追加、最優先） |
| [WS174](ws174/ws.md) | ベータ1 | MG003 | 1 | 起動時の Ctrl・Shift（UEFI、BIOS は後日）（2026-10-05 追加） |
| [WS175](ws175/ws.md) | ベータ2（Q1 の案） | MG006 | 17.5 | Notes の PDF の画像と文字の編集（2026-10-06 追加） |
| [WS176](ws176/ws.md) | ベータ3 | MG006 | 30 | Canvas（pen のイラストと画像の編集、2026-10-06 追加） |

### リリースの段ごとの見積もり（2026-10-05 夜に作り直し）

2026-10-05 夜 ユーザー「今作り直す」。WS ごとの表の「段」と「残り」の列から集計した（WS159・WS160 の行を足し、WS169・170・171 をベータ2、WS162・163 を WS172 に吸収）。残りの LW は各 WS の今日の進みを細かく引いてはいない（粗い見積もり ±50%）。表の全行の合計 435.9 LW。

| 段 | 差分（LW） | 累計（LW） | WS |
| --- | --- | --- | --- |
| ベータ1（2026-10-17、RC は 10/13） | 35.3 | 35.3 | 20 件: 005・033・045・049・073・081・089（2）・090・094・095・099・100・127・128・129・131・132・134・159・160 |
| ベータ2 | 205.1 | 240.4 | 66 件: 001・004・009・014・026・029・031・046・050・051・052・066・068・074・075・078・079・083・084・085・088・089（4）・101・102・106・110・112・113・114・115・116・117・120・121・122・124・125・126・130・138・139・140・142・143・145・148・149・151・152・153・154・155・156・157・158・161・164・165・166・167・168・169・170・171・172・173 |
| アイディアの Phase（議論の後に段を決める） | 22.0 | 262.4 | 5 件: 082・119・144・146・147 |
| 要検討（ブロック） | 79.5 | 341.9 | 10 件: 037・038・039・044・048・080・098・118・141・150 |
| 保留（Future Work） | 4.0 | — | 013 |
| 完了 | 1.0 | — | 061 |
| WS172 に吸収 | 0.0 | — | 162・163 |
| キャンセル（不要に） | 89.0 | — | 007・015・017・027・028・034・047・077・096・097・123 |

換算（2026-10-05 ユーザーの観察）:「LWの見積もりは、これまでの様子を見ていると、1 LW が 1~2 physical hourくらいでした。十分実装できると思います。」→ 1 LW ≈ 実時間 1〜2 時間（担当 1 人あたり）。ベータ1 の 30.1 LW は約 30〜60 時間で、2026-10-13 の RC までに収まる見込み（ユーザーの判断で絞らない）。

## Workstream registry

状態は各 ws.md が正本、ここは投影。完了した WS の Phase の記録は 2026-09-24 に plan から削除した（git の履歴にある）。

| WS | Primary | 内容 | 状態 | 再開点 |
| --- | --- | --- | --- | --- |
| [WS001](ws001/ws.md) | MG002 | POSIX.1-2024 準拠 | incomplete | p033〜p039 cleared（patch、df・du、who、stty、dirname、mktemp・install・base64、xargs）、main へ merge。p040 mesg は実装を merge、uncleared（console の case は BUG-067 待ち）。以後はユーザーの指示のときだけ |
| [WS002](ws002/ws.md) | MG005 | システムサービス | completed | — |
| [WS003](ws003/ws.md) | MG003 | 旧実機 bring-up（終了・再利用禁止） | completed（ユーザー判断で終了） | 未完了は WS027・WS028・F-004 へ |
| [WS004](ws004/ws.md) | MG003 | ハードウェア拡張 | incomplete | NVMe 実機・転送・driver 共通化 |
| [WS005](ws005/ws.md) | MG005 | ネットワーク・WLAN | completed | 2026-10-08 ユーザーの判断（記録のミス、完了済み） |
| [WS006](ws006/ws.md) | MG006 | 入力と evdev | completed | — |
| [WS007](ws007/ws.md) | MG006 | グラフィックス・デスクトップ（旧） | canceled（2026-10-05） | p004 の再現条件、amd64 の残件 |
| [WS008](ws008/ws.md) | MG006 | Noct と BeUI | completed | — |
| [WS009](ws009/ws.md) | MG001 | 文書 | incomplete | DOC-54（GPU の文書） |
| [WS010](ws010/ws.md) | MG001 | Noct の script と build tool | completed | — |
| [WS011](ws011/ws.md) | MG005 | ネットワーク設定 console | completed | — |
| [WS012](ws012/ws.md) | MG005 | サービス管理 console | completed | — |
| [WS013](ws013/ws.md) | MG007 | CPAR（container 分割） | incomplete（Future Work F-002 に保留） | 昇格まで再開しない |
| [WS014](ws014/ws.md) | MG006 | GPU framework・virtio-gpu・Wayland の土台 | incomplete | p004（最終 API と規約の確認） |
| [WS015](ws015/ws.md) | MG007 | μITRON リアルタイム領域 | canceled（2026-10-05 ユーザー） | 昇格まで再開しない |
| [WS016](ws016/ws.md) | MG004 | 実行時の swap 制御 | completed | — |
| [WS017](ws017/ws.md) | MG006 | LFB 描画の高速化 | canceled（2026-10-05 ユーザー） | mmap・Xzed の高速描画 |
| [WS018](ws018/ws.md) | MG008 | kernel の source 所有と interface の統合 | completed | — |
| [WS019](ws019/ws.md) | MG003 | インストールとディスク管理 | completed | — |
| [WS020](ws020/ws.md) | MG003 | Intel Mac の UEFI 起動 | completed | — |
| [WS021](ws021/ws.md) | MG001 | x86 LLVM toolchain と sysroot | completed | — |
| [WS022](ws022/ws.md) | MG002 | ELF の TLS | completed | — |
| [WS023](ws023/ws.md) | MG001 | x86 HAL の規約準拠 | completed | — |
| [WS024](ws024/ws.md) | MG004 | 64-bit UFS の一本化 | completed | — |
| [WS025](ws025/ws.md) | MG004 | I/O・cache・物理メモリの再設計 | completed | — |
| [WS026](ws026/ws.md) | MG001 | 試験資産の整理 | planning | Phase 未定義 |
| [WS027](ws027/ws.md) | MG008 | PowerPC 移植 | canceled（2026-10-05 ユーザー） | p001〜p007 |
| [WS028](ws028/ws.md) | MG003 | インストーラの実機動作（4 機種） | canceled（2026-10-05 ユーザー） | NVMe の未動作の切り分け |
| [WS029](ws029/ws.md) | MG006 | i915 native GPU driver | incomplete | cold VFIO attach の間欠的な停止ほか |
| [WS030](ws030/ws.md) | MG006 | 標準 Vulkan 1.0 と直接表示 | completed | — |
| [WS031](ws031/ws.md) | MG006 | i915 native Vulkan 実行器 | incomplete | p015〜p048 planning |
| [WS032](ws032/ws.md) | MG002 | 外部 package のクロスビルド（clang・OpenSSL・OpenSSH） | completed | — |
| [WS033](ws033/ws.md) | MG005 | networking サービスと有線インタフェースの管理 | incomplete | 抜き差しの実機確認 |
| [WS034](ws034/ws.md) | MG002 | アプリケーション拡充と kernel・libc の是正 | canceled（2026-10-05） | package の導入 |
| [WS035](ws035/ws.md) | MG006 | デスクトップ環境とアプリケーション | completed | 2026-09-30 ユーザーの判断で閉じた（目標が 2026-09-23 のまま古く、ゴールが不明確）。p001〜p138: compositor・sessiond・greeter・lock・Keiland の app・audiod・起動の短縮ほか。Chromium は取り消し（ユーザー「独自にBrowserを書いているから」）。デモまでの仕上げは WS099。`plan/ws035/tests/` は共有の道具として残す（plan/tools への移動は後の整理） |
| [WS036](ws036/ws.md) | MG008 | amd64 の成果を他 platform へ（aarch64 を含む） | completed | 2026-09-27 完了（p021 全 platform の回帰と規約、p026〜p029、p027 は案 A: boot の parameter の parser を緩めた）。実機は未実施。toolchain の cache（zedbsd8）は 2026-09-27 に rev-0 へ upload 済み |
| [WS037](ws037/ws.md) | MG006 | **nvrtx**: NVIDIA RTX 2000 以降（Turing〜Blackwell、GTX 16xx を含む）の GPU driver。WS141 の vc4 と同じ進め方（文書が先、GPL の作業の文書は commit しない、定数の一括の改名、最後に類似の監査、GSP の firmware は userland/firmware）。Pascal 以前は範囲の外（2026-10-04 ユーザー） | planned（p001 から、q692） | 独立。試験は centris に挿す NVIDIA の GPU の VFIO |
| [WS038](ws038/ws.md) | MG006 | Intel Arc dGPU（予約） | planning | 番号のみ |
| [WS039](ws039/ws.md) | MG006 | AMD RDNA GPU（予約） | planning | 番号のみ |
| [WS040](ws040/ws.md) | MG008 | 時間の単位を tick 周期から導く | completed | — |
| [WS041](ws041/ws.md) | MG006 | 起きた thread の即時実行 | completed | — |
| [WS042](ws042/ws.md) | MG002 | `/bin/sh` の POSIX 互換性 | completed | — |
| [WS043](ws043/ws.md) | MG002 | base の utility を POSIX に（sed・grep・awk ほか） | completed | — |
| [WS045](ws045/ws.md) | MG002 | base の text utility の GNU 拡張（sed・awk・grep ほか） | completed | 2026-10-05 完了（GNU の差分 518/518・POSIX 1080/1080・amd64 の boot、試験は plan/tools/gnu-utils） |
| [WS046](ws046/ws.md) | MG002 | GNU 互換の make（autotools の出力を実行できる範囲。並列・jobserver は WS064） | incomplete | p002〜p004・p006 cleared。p007 uncleared（BUG-033 の主因を直した）。p009・p012 cleared（BUG-033: configure 204〜252 → 91 秒、link 0.36 秒、file の fault 15 µs/page）。p013 cleared（libc の mount の一覧の API。coreutils の cross build が通った）。次は p014（p011 の当て直し）・p005 |
| [WS047](ws047/ws.md) | MG001 | build.sh と Noct による build system（TUI・kernel・base・packages を別の system に。Makefile は当面残す） | canceled（2026-10-05 ユーザー） | p001 調査と設計 |
| [WS048](ws048/ws.md) | MG008 | Raspberry Pi 4 の USB（PCIe・VL805 の xHCI・USB キーボード） | incomplete | p001〜p003 cleared（FDT、brcmstb の PCIe、firmware の mailbox と VL805 の firmware。host 試験と QEMU の起動、実機は未実施）。p004 cleared（承認済みの hal.h の差分 `hal_pmem_map_uncached` を適用、実機は未実施）。p005 は config の有効化が残り uncleared。2026-09-27 サブエージェント、main へ merge |
| [WS044](ws044/ws.md) | MG008 | rpi4 を開発に使える形に（console の font、FAT32 の boot、lldb） | incomplete | p001 font・p002 FAT32 の boot partition（QEMU）・p005 cleared。p003（lldb）ほかは WS036 の agent。実機は未実施 |
| [WS049](ws049/ws.md) | MG003 | kernel 内の ACPI AML interpreter | incomplete | p001〜p006・p010〜p015 cleared（p006 kernel への統合: 承認済みの `acpi.rsdp` の差分を適用、amd64 の既定で ACPI の driver が起動、guest の `/dev/acpi` が host の dump と一致。2026-09-27 merge）。次は p007。ACPI はデスクトップが片付くかリミットが余るとき（2026-09-27 方針） |
| [WS050](ws050/ws.md) | MG003 | USB-C の UCSI driver | planning | WS049 が前提 |
| [WS051](ws051/ws.md) | MG006 | USB-C の DisplayPort Alternate Mode | planning | WS050 と i915 の display が前提 |
| [WS052](ws052/ws.md) | MG003 | 電源管理（S0i3、modern standby、`/dev/system` で制御。S3・S4 は対応しない） | incomplete | p001 設計（§10 決定済み）、p002 調べ（HAL の差分 H1〜H4 は承認待ち）、p003 ACPI の側（実装・host 試験済み、T1 の boot 待ち）。次は p004（HAL に依らない） |
| [WS053](ws053/ws.md) | MG001 | clang/LLVM の LTO を vmunix に安全に適用する（優先度高め） | completed | 4 platform の vmunix は既定で full LTO（HAL を含む）。実機はユーザー |
| [WS054](ws054/ws.md) | MG004 | UFS の directory を複数の block に育てる（BUG-038） | completed | 直接の 12 block まで。実機はユーザー |
| [WS055](ws055/ws.md) | MG001 | zedBSD の clang が link に `--undefined-version` を既定で渡す（F-009） | completed | 2026-09-27 完了: zedBSD の clang の linker に `--undefined-version` を既定で（LLVM の patch を zedbsd8 へ）。main の toolchain を zedbsd8 に切り替えた |
| [WS056](ws056/ws.md) | MG002 | POSIX の試験と utility の不具合を直す（BUG-034・035・037、実行中に見つけた BUG-042〜044） | completed | 2026-09-27 完了（p001・p002 cleared。BUG-046 は console の `POSIX-R2.ELF` 10 回連続 status 0 で閉じた、BUG-068・069 は WS073）。試験は plan/tools/posix/ |
| [WS057](ws057/ws.md) | MG004 | 仮想メモリの reserve と commit の分離と commit の swap の裏打ち（over commit 禁止）の確認と修正（design policy 10） | completed | 分離と拒否は実装済み、BUG-048 を修正。裏打ちは物理 + swap のまま（ユーザーの決定） |
| [WS058](ws058/ws.md) | MG004 | cache の大きさを現代の機械向けに見直す（主記憶 4 GB・swap 16 GB 前提、design policy 10） | completed | p001・p002 cleared。buffer 物理/8、page cache 物理/2、object cache 256、file 2048、inode 2048、overlay 4096、I/O pool 64 MiB。8192 級は F-013（動的確保と hash）の後 |
| [WS059](ws059/ws.md) | MG004 | disk の無い mount にも `st_dev` を与える（BUG-047） | completed | p001 cleared。`mount_device_number()`。`df` が全 mount を出す |
| [WS060](ws060/ws.md) | MG004 | UFS の journal の commit を batch にして名前の操作を速くする（BUG-040）。journal を既定にする前提（WS063） | completed | 2026-09-27 完了（規約は WS063-p002 で）。p001 は p002・p003 に置き換えて canceled |
| [WS061](ws061/ws.md) | MG002 | expat の configure と compile を Linux と同等の水準にする（fg011） | incomplete | 受け入れの計測は達成（q449 の後）: configure 8.2〜8.9 秒（host 10.7）、make（直列）11.3 秒（host `-j1` 15.5）、`cc t.c -o t` 76〜84 ms（host 83〜85）。残り: 規約の Phase ws061-p011（最後） |
| [WS062](ws062/ws.md) | MG004 | amd64 の disk image を ESP の vmunix・UFS の root partition・swap partition に（2026-09-25 ユーザー指示） | completed | 2026-09-27 完了（p004: 規約の全文。zedimage-host の出力が同じ） |
| [WS063](ws063/ws.md) | MG004 | UFS の journal を既定にする（journal の無い image は mount の時に作る、`nojournal`）（2026-09-26 ユーザー指示） | completed | 2026-09-27 完了（p002: 規約の全文と回帰、crash の試験 v3・v2・root）。v2 の tail の journal は v2 のまま（2026-09-27 ユーザーが案 A で確定）。制限: transaction ごとの解放 block の追跡は 8192 まで |
| [WS064](ws064/ws.md) | MG002 | base の make の並列（`-j`）と、並列の make の時間を host と同等以上に（2026-09-26 ユーザー指示） | completed | 2026-09-27 完了（p003: 規約の全文、make・sh・kernel の lock・vmspace の fork・libc の posix_spawn。guest の make-diff 100/100、fork・vfork・posix_spawn の試験、expat の configure が同じ。時間は未測定） |
| [WS065](ws065/ws.md) | MG002 | `/bin/sh` に POSIX が未規定とする bash 拡張を足す（2026-09-26 ユーザー指示） | completed | 2026-09-27 完了（p004: 規約の全文、host の sh-diff と guest の expat の configure が同じ） |
| [WS067](ws067/ws.md) | MG002 | `/dev/fd` を呼んだ process の descriptor に合わせる（BUG-054、2026-09-26 ユーザー「最優先」） | completed | BUG-054 resolved（QEMU）。p001・p002 cleared |
| [WS066](ws066/ws.md) | MG002 | 動的 link の program の起動を速くする（`ld.so` の最適化）（2026-09-26 ユーザー「あとでやるリスト」） | planning | p001（費用の内訳と設計）。優先度は低い |
| [WS068](ws068/ws.md) | MG006 | EGL と OpenGL ES（と desktop GL 3.0〜4.6）を Vulkan と display 拡張の上に実装する（Wayland とディスプレイ直接の両方）（2026-09-26・27 ユーザー指示） | incomplete | 自前の GLSL compiler（p003 = p015〜p019、GLSL 1.40〜3.30・ES 3.00 と uniform block の p012 = p020・p021）cleared（2026-09-27、サブエージェント、main へ merge）。次は p013（desktop GL 3.0 の context）・p005（GLES 3.0 の API）・p014（GL 3.3〜4.6）。p002・p008・p010・p006 cleared |
| [WS069](ws069/ws.md) | MG006 | zdesktop で X11 の app を動かす（単体の `zdesktop-x11server`、rootless、GLX）（2026-09-26・27 ユーザー指示） | completed | 2026-09-27 完了（q492）。zdesktop-x11server（rootless、窓は Vulkan、GLX）、BUG-057 の修正、Xzed はレトロ用に戻した。残りは F-021・F-024・F-030 |
| [WS070](ws070/ws.md) | MG006 | zdesktop の System Menu: client がメニューの意味を渡し、zdesktop が浮いたタイトルバーとシステムバーに描く（`xdg_toplevel_menu_v1`、libzdesktop で包む）（2026-09-27 ユーザー指示） | completed | completed（2026-09-27）: System Menu と Titlebar（MENU・CONTROLS・TABS）。残りは Future Work（F-042・F-043・F-045）、i915 実機は WS075 |
| [WS071](ws071/ws.md) | MG006 | zedBSD File Manager: Finder 風で zedBSD らしいファイルマネージャ（ホームのダッシュボード、サイドバー、タグ、Quick Look、System Menu）（2026-09-27 ユーザー指示、仕様案は ws071/spec.md） | completed | completed（2026-09-27）: zdesktop-files の最初の版（すりガラスの付箋の pane、タブ、titlebar の CONTROLS、context menu、PNG の thumbnail、DnD、configure_bounds）。残りは Future Work（F-032〜F-041・F-044）、i915 実機は WS075、窓の外への DnD は WS035 |
| [WS072](ws072/ws.md) | MG004 | write cached の UFS の format の lease（BUG-060）と、NVMe の timeout の後の回復で root の mount が ETIMEDOUT になる（BUG-059）（2026-09-27、サブエージェント） | completed | 2026-09-27 完了（p001 BUG-060: write cached の format の lease、p002 BUG-059: NVMe の timeout の後の再発行） |
| [WS073](ws073/ws.md) | MG002 | Bug Board のbug解消（2026-09-27の対象境界を保持）。2026-10-02のP8はWS073に限らず、mainが各bugの既存handling WS/Phaseを照合して配属 | incomplete | p045（BUG-135、UFS の namespace_lock と journal の commit の待ち）を修正（停止 10→2 回）。残りは WS131 p005・p006 の後 |
| [WS074](ws074/ws.md) | MG006 | zedBSD の Web ブラウザ `userland/base/zdesktop-browser`（HTML5 の layout engine → 最適化にこだわらない JavaScript engine の接続 → CSS の準拠と Chrome との比較で目標値を段階的に上げる。JS と Wasm の実行 engine を共通化。画像は libpng-compat・新しい libjpeg-compat、TLS は当面 OpenSSL）（2026-09-27 ユーザー指示） | incomplete | p099 cleared（Acid2 100%）。p172 whole cleared（2026-10-03、Codex の browser3 の b-q598）。2026-10-05 Q1 に戻した。レンダリングの改善（p100 Acid3 の pixel 37.04%→100%、p101・p173〜p176）はユーザーの指示まで停止。B1 の試行 p178 を計画。次の候補 BUG-182 |
| [WS075](ws075/ws.md) | MG006 | i915 の高度化: 今日のデスクトップ（zdesktop の glass・backdrop のぼかし・タブ）とグラフィックス（GLES 2/3、GL 3.0〜3.2）を Latitude 5330 の i915 のネイティブ実行器で動かす（compiler の inlining・F-022・F-023 の不足、性能と安定）（2026-09-27 ユーザー「OpenGL 3.2が問題なければ、それ以降のOpenGLはいったん保留して、i915の高度化に進んでください。」） | incomplete | 2026-09-30: L1（C6 91.3 ms）と L2（p029: blur は窓ごと、既定は無効・Settings だけ有効、C6 67.3 ms）を満たした。L3 は p030 で計測（文字の draw 約 400 で約 10 ms）。ユーザーの指示で描画の高速化を止め、p031（文字の draw をまとめる）は build まで済んだ patch（`phase031/exp/text-batch.patch`）で保留。再開はユーザーが描画の高速化の再開を言うとき |
| [WS076](ws076/ws.md) | MG002 | libc の libm を自前で正しく書き直す（src/libc、誤差 1 ulp 以内、fmod 等は正確）（2026-09-28 ユーザー「libmは独自に書いてください。libcのツリーに入れてください。」） | completed | 2026-09-28 完了（`src/libc/math/`、群 B は全件で正しく丸め、BUG-078 解決）。F-046・F-047 へ移管 |
| [WS077](ws077/ws.md) | MG001 | PC-98 の PCI を有効にする（BUG-024、2026-09-28 ユーザー「Bug024は、PCIを有効にします。」） | canceled（2026-10-05 ユーザー） | **優先度を下げた（2026-09-28 ユーザー「Bug024は優先度を下げます。」）**。p001（調査と設計）。HAL の差分は承認が要る。PC-98 の試験が要るので着手の前に確認 |
| [WS078](ws078/ws.md) | MG006 | Kei Operating System への名前の移行（2026-09-28 ユーザーの決定: OS の名前 Kei、カーネルの内部名 zedbsd、Keiland、`/bin/wayland`・`/bin/xserver`・`/bin/browser`、`KERN_` の接頭辞、ロゴは Kei の 3 文字） | incomplete | 2026-09-28: p002・p003・p006・BUG-080・retro への移動は済み、p004 はほぼ済み。残り: 注釈と log の名前、p005 |
| [WS079](ws079/ws.md) | MG006 | 手書きノート（Notes、筆圧 4096 段階の USB のペンタブレット、PDF に保存し編集の metadata を持つ）と PDF Viewer（scroll と page の swipe）、上の右端から左下へのスワイプで Notes を起動・最前面・全画面（2026-09-28 ユーザー） | incomplete | 2026-09-30: 段 L1（全 Phase）と L2 の QEMU の分（ws079-p016: 台本 S8・S9 を注入の touch と pen で自動で通す、PDF の頁送りの最長 142 ms ≤ 200 ms）を満たした。残り: 実機と Windows の QEMU での確かめ（ユーザー、`plan/ws079/demo-s8-s9-manual.md`）、L3 の実機のペン |
| [WS080](ws080/ws.md) | MG002 | `ld.coff`: Win64 PE32+ の動的ローダ（PE/COFF の mapping・relocation・DLL・import/export・Microsoft x64 ABI・最小の TEB/PEB・GS base）。NT の loader は再現せず `AddressOfEntryPoint` へ直接。互換の DLL は上に積む（2026-09-28 ユーザーの仕様 [spec.md](ws080/spec.md)） | incomplete（p001 の設計の文書まで、p004 in-progress・p005 planned、2026-10-04） | p001（設計）から。GS base は swapgs（案 A）に決定、差分は p001 で承認を得る。path は `/usr/libexec/ld.coff`・`/usr/lib/coff64/`（商標のため Win64 の名前を OS に出さない）。source は `userland/base/ld-coff/`・`userland/desktop/w64/`。橋の DLL は置かず互換の DLL が zedBSD の UAPI を直接呼び Wayland と直接通信。判断待ち: 優先度 |
| [WS081](ws081/ws.md) | MG006 | touch の操作の質: 慣性のある scroll と、低い fps の安い touch panel の数式による補間・予測。HID の driver・compositor・ブラウザ（と Keiland の app）にまたがる計画をこの 1 か所で（2026-09-28 ユーザー） | incomplete | 2026-09-30: L2 の準備 p016 cleared（touch の報告の記録の道具 `plan/ws081/tests/touchlog.c`、Linux の QEMU で 60 Hz・90 Hz の数と欠けを正しく数えた、ユーザーの 3 行の手順 `windows-touch.md`）。次は Windows の QEMU 用の image（p018、P4）→ ユーザーの計測 → L3 の p017 |
| [WS082](ws082/ws.md) | MG002 | Linux の `/dev/kvm` の移植の検討（eventfd 等の非 POSIX の fd の代わりに unix socket の message で MMIO・IRQ の通知。ioctl を直接の移植・別の仕組みでの代替・実装不能に分類）（2026-09-28 ユーザー） | incomplete | p001 cleared（[study.md](ws082/study.md)）。§10 の 11 項目のユーザーの判断待ち |
| [WS083](ws083/ws.md) | MG006 | Vulkan Video の拡張（`VK_KHR_video_queue`・`video_decode_queue`・`video_decode_h264`）と i915 の VCS・MFX の対応、最初の目標は H.264 の decode（2026-09-28 ユーザー。OSC のデモには必須ではない） | planning | p001（設計）から。デモの後 |
| [WS084](ws084/ws.md) | MG006 | i915 の firmware の画面の引き継ぎ（素の実機の UEFI の起動で GOP が点けた pipe を N1 で止めて driver のものにし、デスクトップを出す）（2026-09-29 ユーザー、main が実装） | incomplete | p001・p002 cleared（2026-09-29、素の 5330 で takeover → LCD の Keiland、24.5 present/s）。残り: parity との乖離 1〜3 の整理 |
| [WS085](ws085/ws.md) | MG006 | Windows版WINQ-EMUのVenusでデスクトップを表示する（2026-09-29 ユーザー） | incomplete | p001 実行中。mapped blob scanoutでデスクトップを表示。SDL→仮想USB HIDタッチを実装しQMP 2指注入でメニューを確認。Files起動停止の共有画像通信を修正し開閉・再起動とTerminal同時起動を確認。物理タッチと表示所有者切替は未試験 |
| [WS086](ws086/ws.md) | MG002 | ls の出力を GNU ls と同じにする（端末なら既定で列、端末の幅）（2026-09-29 ユーザー） | completed | 2026-09-29 完了（端末で GNU と同じ列、GNU ls 9.7 と host 3362 件・guest 180 件で差 0、名前は常に UTF-8）。残りは F-055 |
| [WS087](ws087/ws.md) | MG002 | /bin/sh の対話の行編集: 矢印キーの履歴（BUG-103）と Tab の補完（2026-09-29 ユーザー） | completed | 2026-09-29 完了（QEMU・host）: 履歴の file（~/.sh_history）、矢印の履歴の上限、PS/2 の E0 の key の capability（矢印が届かなかった原因）、prompt の ~、Tab の補完（GNU Readline の名前で libedit に）。実機の確認と BUG-103 の resolved は実機の後 |
| [WS088](ws088/ws.md) | MG006 | Windows で動く Kei-nightly.zip を CI で配布する（元の zip を clang の cache と同じ Release `rev-0` に置いて再利用し、CI が hdd-image.img を入れる）（2026-09-29 ユーザー） | incomplete | 2026-09-29: p001 は draft（整理した元の zip 56 MB、LICENSES・THIRD-PARTY、DLL は MSYS2 と一致）。fork の commit と zip の中身の確認がユーザー待ち。次: p003 の準備 |
| [WS089](ws089/ws.md) | MG006 | 設定のアプリ（Settings: 左に項目の pane、右に設定、浮いたすりガラスの pane）（2026-09-29 ユーザー、デモの優先事項） | incomplete | p010（回帰と棚卸し、候補 C1〜C17）・p012（検索の key・touch の scroll・Wi-Fi の待ちの slot・文言）は済み。p019（titlebar の検索欄の Down・Up、compositor 側）。標準 app の開発は WS131 の後に組み直す |
| [WS090](ws090/ws.md) | MG006 | widget・control の共有 library（少なくとも慣性の smooth scroll、独自の部品）（2026-09-29 ユーザー） | incomplete | 2026-09-30: p001〜p006・p013・p008（PDF Viewer・Image Viewer）・p011（Terminal・Notes の窓）・p014（file chooser を親の title bar にぶら下がる sheet、不透明）cleared。KUI_VERSION 11・KEILAND_VERSION 20。統合の試験（demo-s8-s9.sh、QEMU）PASS。残り: p015（案、scroll を kui_scroll へ）・p009・p010（Files）・p007（Settings）、Terminal の p088 の切り分け。ラップアップ（2026-09-30 夕） |
| [WS091](ws091/ws.md) | MG006 | 画像 viewer（2026-09-29 ユーザー） | completed | 2026-09-29 完了（QEMU）: `/bin/imageview`、PNG・JPEG（EXIF の向き）・GIF（動く）、fit・拡大・pan・pinch・慣性、前後の画像、全画面。Files からの起動は WS093、実機の確認は残り |
| [WS092](ws092/ws.md) | MG006 | text editor（simple）（2026-09-29 ユーザー） | completed | 2026-09-29 完了（QEMU）: `/bin/textedit`、共有の file chooser（libkeiland、KEILAND_VERSION 12）、touch・PRIMARY・clipboard・Files からの起動。実機は未実施。touch の drag での選択は無し |
| [WS093](ws093/ws.md) | MG006 | Files から app の起動（画像・text の double click、file の種類と app の対応）（2026-09-29 ユーザー） | completed | 2026-09-29 完了（QEMU）: Files の double click・Enter・double tap で png・jpeg・gif → Image Viewer、text 系 → Text Editor、html → Browser、pdf → PDF Viewer。Always Open With と Use System Default（`~/.config/keiland/open-with`）。実機は未実施 |
| [WS094](ws094/ws.md) | MG006 | desktop の file の icon（`~/Desktop`）（2026-09-29 ユーザー） | incomplete | p014（規約の指摘）cleared。残り p012（5330 の実機）・p007（全回帰） |
| [WS095](ws095/ws.md) | MG006 | IME（Wayland の input-method-v2・text-input-v3、単一の IME・複数言語、まず日本語、REmacs の辞書）（2026-09-29 ユーザー） | incomplete | p012（辞書 1,478 見出し）・p013（変換中の文字を本文と同じ大きさで inline）・p005（候補の窓・右上の IME の status・key repeat）cleared（QEMU）。実機の目視はユーザー |
| [WS096](ws096/ws.md) | MG002 | Qt6（core・gui・widgets）の互換の書き下ろし（API の interface だけ、zlib）（2026-09-29 ユーザー、デモの後） | canceled（2026-10-05） | デモの後 |
| [WS097](ws097/ws.md) | MG002 | GTK4 の互換の書き下ろし（API の interface だけ、zlib）（2026-09-29 ユーザー、デモの後） | canceled（2026-10-05） | デモの後 |
| [WS098](ws098/ws.md) | MG006 | IME の変換のニューラル化: 辞書で候補を作り、小型のモデル（15 MB 未満）で同音異義語の選択（語の番号の並び）とひらがな列の形態素解析（語の境界と品詞、BiLSTM か小型の Attention）を評価する（2026-09-29 夜 ユーザー、IME の最後の仕上げ） | planning | WS095 の基本の辞書の後。学習の corpus と license はユーザーの判断 |
| [WS099](ws099/ws.md) | MG006 | Keiland の compositor（zdesktop）のデモの基準: 窓の操作・App Home・Wiseview・全画面と最大化の解除・greeter から Log Out と Shut Down・すりガラスの上の文字の contrast・回帰の試験の全通過（2026-09-30 ユーザー、WS035 の後継。基準は ws.md） | incomplete | p023（BUG-136/137、直す前からの C 基準の失敗）cleared。p020（BUG-125）と p021 は BUG-147 の試験の頑健化（p024、P2 が作業中）の後に判断 |
| [WS100](ws100/ws.md) | MG006 | system bar の音量: 右上の通知領域の音量の icon、音量の slider と mute、変えたときの確かめの音（2026-09-30 ユーザー。動画の再生はデモの後） | incomplete | 2026-09-30: L1 がそろった（A1〜A6、Settings の Sound の頁）。L2 は 5330 の実機（A7、ユーザー）。L3 の p008 cleared: 確かめの音の遅れは QEMU の guest の中で中央値 31〜37 ms（≤ 50 ms、合否は実機で）。kernel の fragment を小さくする直しは実機で 50 ms を超えたとき |
| [WS101](ws101/ws.md) | MG006 | GPU の compute: i915 の Vulkan の compute（dispatch・shared memory・barrier・atomic）、libglesv2 の GLES 3.1 の compute、Noct の自動並列化（accel_opengles）が 5330 の GPU で動く（2026-09-30 ユーザー、10/17 のデモまで、最優先ではない） | incomplete | 2026-09-30: L1 で S13 が 5330 で通った。L2: p016（時間の分解）cleared、p017（buffer の使い回しと copy の削減、QEMU の GPU の call 835 → 212 ms、CPU の 10.6 倍遅い）uncleared。ユーザーの判断「今のまま」で S13 は今の見本、最適化はここで止める（5330 の p017 の値は P1 が追記） |
| [WS102](ws102/ws.md) | MG006 | スクリーンキーボード: 右下の角の swipe で右側に flick の panel（英字・記号・日本語）、左下の角の swipe で下側に QWERTY と手書き（認識は stub）。compositor に直接（2026-09-30 ユーザー） | incomplete | 2026-09-30: L1 を満たした。L2: p006・p007・p008・p009・p015・p016（右の列の道具の面）・p017・p018・p020・p021・p023・p024（履歴の tab。受け入れの手順だけ PASS、全手順の回帰・C9・boot test は未実施）、L3 の p019（色付きの絵文字）cleared（QEMU）。ユーザーの指示で優先を下げてラップアップ（2026-09-30 夕）。保留: p022（絵文字の tab）・BUG-125・p010・p011（速さ）・p012（IME、人間） |
| [WS103](ws103/ws.md) | MG006 | compositor を libvulkan だけにする（GPU の UAPI の直の ioctl を無くす）（2026-09-30 ユーザー「規則にして今移す」、規則は Guardrail） | completed | 2026-10-01 完了（p001〜p007、q508〜q514）: V1〜V4 を満たす（QEMU の Venus と 5330 の passthrough、単独の実機の起動は未実施）。Linux・FreeBSD の backend は F-065。試験は plan/tools/gpu-boundary |
| [WS104](ws104/ws.md) | MG006 | Keiland の OS の境界の整理: desktop の公開の header を `userland/desktop/include/` へ、libkeiland と compositor の OS の部分を `zedbsd/` の module へ、install の path を macro に。zedBSD の振る舞いは変えない（2026-10-01 ユーザー「Linux移植を進めます」、WS105 の準備） | completed | q515〜q522 / A1〜A6 verified。全文規約と全必須回帰 PASS、Linux は WS105 へ |
| [WS105](ws105/ws.md) | MG006 | Keiland を Linux で動かす（`/opt/keiland`）: `make keiland-linux`、libvulkan-compat（独自の WSI から system の libvulkan へ chain）、compositor の Linux の module（KMS・evdev・linux-dmabuf・logind）、主な app、gdm、wpa_supplicant・ALSA（2026-10-01 ユーザー、F-065 の Linux の分） | completed | L1〜L9/最終source conformance verified、q538 finished。Linux host/ownDebian13guest・zedBSD回帰、BUG-125/127は未修正trackingのユーザー許可。GitHub publication pending、次の実装なし |
| [WS106](ws106/ws.md) | MG001 | base/desktop の test/probe/demo 30件を userland/tests/ へ移し、package/config/install と既存の動作を維持 | incomplete | q540 partial cleared、p002 uncleared（ime-probe回答待ち）、p003未実行。 |
| [WS107](ws107/ws.md) | MG006 | engine の source を libbrowser に所属させ、Wayland無し・標準Vulkan/抽象入力の component と browser shell を整備 | completed | B1〜B5 verified / q544、API v2/public Vulkan client/最終boot。GitHub deferred（2026-10-05 Q1 の担当。p172 で browser2 を取り込み済み） |
| [WS108](ws108/ws.md) | MG007 | CI で Debian13/Ubuntu26.04 の Linux Keiland .deb を別々に作成/検証/artifact保存 | completed | P1〜P5 / q549、2OS native deb＋QEMU runtime、CI/release定義。remote未実施 |
| [WS109](ws109/ws.md) | MG006 | Linux版の共通描画を利用した native FreeBSD15 Keiland、audio/network/WiFi backend | completed | q574 native実機build/install+全文規約、p008 user「完璧に動作しました」でF6受け入れ合格 |
| [WS110](ws110/ws.md) | MG006 | 通常compositor起動を既定にし--testingで試験用有限modeを明示 | completed | 2026-10-05 完了（引数なしで通常の session、試験は --testing、試験の script 233 file を置き換え、試験は plan/tools/compositor） |
| [WS111](ws111/ws.md) | MG006 | Linux/FreeBSD共通console keiland-desktop、GDMはdirect維持 | completed | q575/q576: 共通console launcher/両native install/全source確認、GDMdirect不変。--login検討のみ |
| [WS112](ws112/ws.md) | MG007 | Linux5種類のbinary packageを指定make/CIで作成しreleaseへ添付 | incomplete | p001/q585契約調査uncleared、D1 Fedora/Arch boot回答待ち。RPi arm64、CI runtime不要、FreeBSD source-only。q591は候補のみ |
| [WS113](ws113/ws.md) | MG006 | zedBSD i915 hotplug/Vulkan Displayから複数画面・Settings/libkeiland・窓の全体移動 | incomplete | p001/q586設計調査uncleared、D-ATOMIC未決、A3成果回収/終了。全拡張/全mirror、pointer越境で窓一括移動。実装未投入 |
| [WS114](ws114/ws.md) | MG006 | Linux標準GTK4互換性を調査し機能表レビュー後にXDG-shell/portal等を選択改善 | incomplete | p007（Linux 本物の GTK4 の CSD）・p008（KDE の server decoration、宣言の無い窓は SSD）cleared |
| [WS115](ws115/ws.md) | MG002 | upstream GTK4をzedBSD `packages/desktop/gtk4`へ移植し知見を記録 | incomplete | p001・p004〜p009・p002（GTK 4.18.6 と依存一式の build）cleared。p010（zedBSD で起動）は ld.so の上限で止まり、ユーザーの判断で後回し（再開点は phase010） |
| [WS117](ws117/ws.md) | MG006 | Linux の本物の Qt6 を調査し、素の Qt6 アプリが動くよう compositor を改良（WS115/116 の前） | planning | WS115・WS097 の後 |
| [WS118](ws118/ws.md) | MG003 | Latitude 5320 で Kei を動かす（LCD の制御の不具合、sshd の遠隔 log 用 image、ユーザーと実機） | planning | p001（遠隔 log の image A は QEMU で SSH・collect まで）。実機は RTL8156 の USB の LAN でユーザーと |
| [WS119](ws119/ws.md) | MG003 | インストーラの作り直し | planning | p001 planned（要件の案） |
| [WS120](ws120/ws.md) | MG006 | 音楽アプリ（fg019） | planning | p001（設計、m4a だけ・AAC を独自実装）。標準 app の開発は WS131 の後 |
| [WS121](ws121/ws.md) | MG006 | Web ブラウザでのアクセラレーションつきのビデオ再生（fg019） | planning | p001（2026-10-05 Q1 の担当、P2 が q775 で設計中、ベータ2） |
| [WS122](ws122/ws.md) | MG006 | 動画プレーヤアプリ（fg019） | planning | p001 |
| [WS123](ws123/ws.md) | MG006 | VA-API のライブラリ | canceled（2026-10-02 user、アプリが Vulkan Video を直接使う） | — |
| [WS124](ws124/ws.md) | MG002 | GNU Emacs の package（fg019） | planning | p001・p002 planned |
| [WS125](ws125/ws.md) | MG002 | vim の package（fg019） | planning | p001・p002 planned（p002 は package の tree を image に入れる共通の仕組み） |
| [WS126](ws126/ws.md) | MG002 | Python 3 の package（fg019） | planning | p001 planned |
| [WS127](ws127/ws.md) | MG006 | Files のベータ1 のブラッシュアップ（最重点）（fg019） | incomplete | p001（棚卸し）cleared、p002（BUG-140・Move To・New Window・重ね表示の scroll bar・spring-loaded・PDF の thumbnail）は eject を除き済み。eject は WS132。標準 app の開発は WS131 の後 |
| [WS128](ws128/ws.md) | MG006 | 標準アプリ全般のベータ1 のブラッシュアップ（fg019） | incomplete | p001（棚卸し、候補 C1〜C15）・p002（Notes の Open・Save As）・p003（Text Editor の Replace・Open Recent）cleared。標準 app の開発は WS131 の後 |
| [WS129](ws129/ws.md) | MG007 | ベータ1 のリリース作業（版・release notes・既知の問題・CI の release・最終回帰）（fg019） | incomplete | p009（デモの image を CI 土台に）・p010（全 desktop app と base の program を config へ）cleared。版 zedbsd-0.1.0-beta1、Prerelease を user が手で昇格 |
| [WS130](ws130/ws.md) | MG005 | IPv6 の network stack（ベータ1 は計画だけ、実装はベータ2 以降。DHCPv6 は `dhcpc -6`） | planning | p001 設計 |
| [WS131](ws131/ws.md) | MG006 | libkeiland を GUI toolkit 兼 desktop 機能の抽象化層にする（app の窓の作成を含む GUI の構築を共通化） | incomplete | p003〜p005 cleared、p006（Linux は T1-042 PASS、zedBSD の T2-007 は未実行）・p007（demo-s8-s9 未実行）・p008（試験未依頼）は uncleared。次は p008 の試験と p009。設定の部分は WS135 へ |
| [WS132](ws132/ws.md) | MG006 | /dev/system の電源管理と PnP の通知（subscriber が事象を指定）、自動 mount、Files の eject | planning（ベータ1 に入れる、2026-10-04 ユーザー） | p001 設計（/dev/system に電源管理と PnP の通知、subscriber が事象を指定） |
| [WS133](ws133/ws.md) | MG003 | 安定版 S1 の実機試験（安定版の image を実機で起動し SSH で複数の試験を詰め込む。最初の項目は ws005-p020・p024 から移した WiFi） | completed（S1、2026-10-03。2026-10-04 の UAT は WS の外で [uat.md](uat.md)・[uat/](uat/) に記録） | 安定版 S1 の内容と試験の一覧はユーザーと決める |
| [WS134](ws134/ws.md) | MG006 | システムモニターのアプリ（Analytic Spatial UI、中央の状態コア、層構造、2026-10-03 ユーザー） | incomplete | p001・p002 cleared、p003 uncleared（sim の fps 4.7）、p004 uncleared（`interact.c` まで） |
| [WS135](ws135/ws.md) | MG006 | 設定の読み書きを libkeiland に一本化（libkeiland が直接か compositor の拡張で解決、監視と通知の API、desktop.conf は compositor の内部で session の開始・終了だけ読み書き。BUG-162、2026-10-03 ユーザー） | completed（2026-10-04） | — |
| [WS136](ws136/ws.md) | MG007 | 試験の image を「WS の tests/ の config.mk ＋個別の file の複写」に揃え、過去の build/ を入力にしない（2026-10-04 ユーザー） | completed（2026-10-04） | — |
| [WS137](ws137/ws.md) | MG006 | FreeBSD の試験の VM（QEMU+KVM）を T1・T2 で使えるようにし、libkeiland-backend の FreeBSD の build と試験を流す（2026-10-04 ユーザー） | completed（2026-10-04） | — |
| [WS138](ws138/ws.md) | MG006 | 背景の画像を PPM から PNG に（F-071。起動の decode は prefetch の thread へ） | completed | 2026-10-05 完了（PNG・JPEG の背景、PPM の読みを削除、試験は plan/tools/wallpaper） |
| [WS139](ws139/ws.md) | MG006 | desktop の性能の台帳と改善（F-072。開発の host の Venus は lavapipe の CPU、E2 の 5330 の測定は未実施） | planned（p001 から。判断 U2・U3・U5 待ち） | — |
| [WS140](ws140/ws.md) | MG002 | ld.so の依存・object・handle の上限を動的に（F-070。main は 32・16 のまま） | completed | 2026-10-05 完了（ld.so の上限を動的に、amd64、試験は plan/tools/rtld） |
| [WS141](ws141/ws.md) | MG006 | Raspberry Pi 4 のグラフィックス driver（VideoCore VI: HVS・pixelvalve・HDMI と V3D 4.2）。Linux の vc4・v3d の初期化の順と command の順を先に文書にし、i915 の書き換えを手本に我々の interface へ。framebuffer に段の印。Linux の vc4・v3d は GPL なので code は写さない（2026-10-04 ユーザー） | planned（p001 から、q691） | 独立（他の WS と並走） |
| [WS142](ws142/ws.md) | MG006 | デスクトップのアプリの切り替え: Windows キーでアプリの一覧、タッチパッドの端からの 2 本指（Wiseview・仮想デスクトップ）、上部のバーのアプリの一覧とプレビュー、3 本指のタップ・Alt+Tab の切り替え（2026-10-04 ユーザーの要望） | incomplete | p001〜p006 cleared（QEMU）。5330 の UAT 待ち（実機は第 1 段の外） |
| [WS143](ws143/ws.md) | MG006 | Bluetooth（Settings の stub の頁の実体、HCI・daemon・desktop の経路）。ベータ2 の実装の項目、時期は未定（2026-10-04 ユーザー） | planning（Queue なし） | — |
| [WS144](ws144/ws.md) | MG005 | VPN（bridge・tunnel などの汎用の network の基盤と、選んだ VPN の protocol、Settings の stub の VPN の頁の実体）。ベータ2 の実装の項目、時期は未定（2026-10-04 ユーザー） | planning（Queue なし） | — |
| [WS145](ws145/ws.md) | MG006 | 印刷: printer の daemon（IPP・LPD で PDF）、libkeiland の印刷の口と printer の一覧（compositor 経由、backend が daemon を起動）、Settings の Printers の頁（IP・port・protocol）、後に PostScript・vendor の filter（2026-10-04 ユーザー） | planning（p001 から、Queue なし） | — |
| [WS146](ws146/ws.md) | MG006 | SSH を使う独自のオンラインストレージ（server に agent を転送、file の木と metadata の database、差分の同期、手元に本体の無い on-the-fly のアクセス、OneDrive のような機能）。「あとで」の要望（2026-10-04 ユーザー） | planning（Queue なし） | — |
| [WS147](ws147/ws.md) | MG006 | Microsoft OneDrive の client（Settings の Sharing の頁から設定、OAuth・Graph の API・同期、on-the-fly は WS146 と共有）（2026-10-04 ユーザー「独立WSにします」） | planning（Queue なし） | — |
| [WS148](ws148/ws.md) | MG006 | Settings の Privacy の頁の検討（要らなければ頁を削除、要るなら設計と実装）（2026-10-04 ユーザー） | planning（p001 の検討から、Queue なし） | — |
| [WS149](ws149/ws.md) | MG006 | Settings の Security の頁の検討（要らなければ頁を削除、要るなら設計と実装。WS148 の Privacy と項目を分け合う）（2026-10-04 ユーザー） | planning（p001 の検討から、Queue なし） | — |
| [WS150](ws150/ws.md) | MG004 | userland の file system の kernel の枠組み（FUSE に当たる物）。クラウドストレージ（WS146・WS147）の前提（2026-10-04 ユーザー） | planning（Queue なし） | — |
| [WS151](ws151/ws.md) | MG006 | Settings の Accessibility の頁の検討（要らなければ頁を削除、要るなら設計と実装。見る・聞く・操作・読み上げ）（2026-10-04 ユーザー） | planning（p001 の検討から、Queue なし） | — |
| [WS152](ws152/ws.md) | MG007 | system の更新（Settings の Updates の頁の実体）。ベータ3 の実装の項目、p001 は方式の検討（2026-10-04 ユーザー） | planning（p001 から、Queue なし） | WS129 |
| [WS153](ws153/ws.md) | MG007 | Settings の Apps の頁と third-party の app の repository（userland/packages とは別）。p001 は package の仕組みの検討（2026-10-04 ユーザー） | planning（p001 から、Queue なし） | — |
| [WS154](ws154/ws.md) | MG006 | Settings の Languages の頁で IME を選ぶ（日本語・SKK・なし=英語）と、SKK の IME の新しい実装（辞書は Emacs の物）（2026-10-04 ユーザー） | planning（p001 から、Queue なし） | WS095 |
| [WS155](ws155/ws.md) | MG006 | Keiland の app: カレンダー・スケジューラ・オーガナイザ（まず簡単な物）（2026-10-04 ユーザー） | planning（2026-10-05 デザイン案を受領、mock から） | — |
| [WS156](ws156/ws.md) | MG006 | app の通知: 画面の下の中央を流れる headline の popup（右から中央、3 秒、左へ fade-out、× で消す）と hotkey の ring の log（すべて消去、個別に消した物は残さない）（2026-10-04 ユーザー） | planning（p001 から、Queue なし） | — |
| [WS157](ws157/ws.md) | MG006 | Keiland の app: 写真の管理（p001 は要件の検討）（2026-10-05 ユーザー） | planning（p001 から、Queue なし） | — |
| [WS158](ws158/ws.md) | MG006 | Keiland 本体と Keiland の app の翻訳（英語が基準、日本語はベータ2、Settings の Languages の頁で選ぶ。F-068 を昇格）（2026-10-05 ユーザー） | planning（p001 から、Queue なし） | WS154 |
| [WS116](ws116/ws.md) | MG002 | upstream Qt6の範囲をGTK4移植後に検討し `packages/desktop/qt6`へ移植 | planning | WS115の知見後。旧WS034 p030移管、Queue none |
| [WS159](ws159/ws.md) | MG006 | native のタッチパッド（LPSS I2C・I2C-HID・HID の digitizer、evdev の MT）と compositor のタッチパッドの層（tap・tap-drag・押し込み・2 本指のスクロール）（2026-10-05 ユーザー「ACPI AMLを実装したあと、I2C-HIDを実装しましょう。compositorのtouchpad層も作りましょう。」） | planning（p001 から、**ベータ1**） | WS049 |
| [WS160](ws160/ws.md) | MG002 | su・sudo・passwd（2026-10-05 ユーザー「su, sudoを実装してください。」・passwd も実装） | planning（**ベータ1**、q721） | なし |
| [WS161](ws161/ws.md) | MG006 | YubiKey のサポート（最初は USB の FIDO2、目標は NFC の CTAP2）（2026-10-05 ユーザーの追加） | planning（**ベータ2**、2026-10-05） | — |
| [WS162](ws162/ws.md) | MG006 | FIDO2 の login（greeter・lock の画面で security key で login）（2026-10-05 ユーザーの追加） | incomplete（WS172 に吸収、未着手の Phase は canceled で WS172 p003 へ） | — |
| [WS163](ws163/ws.md) | MG006 | 数字 6 桁の login（PIN、greeter・lock の画面）（2026-10-05 ユーザーの追加） | completed（WS172 に吸収、WS172 p002 の PASS） | — |
| [WS164](ws164/ws.md) | MG006 | OS の起動時の Welcome の画面（既存の Start で足りるかの検討から）（2026-10-05 ユーザーの追加） | planning（**ベータ2**、2026-10-05） | — |
| [WS165](ws165/ws.md) | MG006 | 手書きの入力（goal の設定が難しいので段階化する）（2026-10-05 ユーザーの追加） | planning（**ベータ2**、2026-10-05） | — |
| [WS166](ws166/ws.md) | MG006 | IME の予測変換（2026-10-05 ユーザーの追加） | planning（**ベータ2**、2026-10-05） | — |
| [WS167](ws167/ws.md) | MG006 | GPU の command の protocol を Venus の番号の流用から独自の名前と番号に（Venus と一致する内容から始め、Venus の番号を再利用したことを header に書き、Google の著作権の表示を外せるようにする）（2026-10-05 ユーザーの追加） | planning（**ベータ2**、2026-10-05） | — |
| [WS168](ws168/ws.md) | MG006 | プレビュー（縮小表示）を作る隔離された専用の command（chroot・他の file の open・network・fork の禁止、fd 0・1 だけ）（2026-10-05 ユーザーの追加） | planning（**ベータ2**、2026-10-05） | — |
| [WS169](ws169/ws.md) | MG006 | メーラの app と compositor のメールの API（許可された app が受信の通知を受ける、browser の認証 code の自動入力、IMAP4・SMTP から Gmail・Outlook へ）（2026-10-05 ユーザーの追加） | planning（**ベータ2**、2026-10-05 ユーザー） | — |
| [WS170](ws170/ws.md) | MG006 | Phone の app（連絡先・SMS/MMS/RCS・VoIP を統合したタイムライン、compositor のメッセージの API、モデム・スマホの bridge の backend。最初は連絡先からタイムラインの表示まで）（2026-10-05 ユーザーの追加） | planning（**ベータ2**、2026-10-05 ユーザー） | — |
| [WS171](ws171/ws.md) | MG008 | hal.h の全ての関数に HAL v2 と同じ水準の契約の comment を書く（comment の差分もユーザーの review の後に当てる）（2026-10-05 ユーザー「このコメント、すべての関数につけてほしいです。今でなくていいので」） | planning（**ベータ2**、合間の仕事、2026-10-05 ユーザー） | — |
| [WS172](ws172/ws.md) | MG006 | passkey の認証の枠組み: sessiond は外部の `/sbin/passkey`（base）で password・PIN・FIDO2（将来はセキュリティチップ）を確かめる、root だけの `/etc/passkey`、PIN の失敗の回数は sessiond の memory（2026-10-05 ユーザー） | incomplete（p002 cleared、p003 FIDO2 へ） | WS161 |
| [WS173](ws173/ws.md) | MG006 | AAT（Agent Acceptance Test）: /dev/input-inject のマウスとキーボード、試験の image だけの画面の撮影、SSH の host の道具で、エージェントが素の 5330 を操作して受け入れを確かめる。UAT はデバイス系と使用感に絞り AAT の後に遅らせる（2026-10-05 ユーザー） | incomplete（p004・p006 は T1-202 待ち） | — |
| [WS174](ws174/ws.md) | MG003 | 起動時に Ctrl で kernel の message を console に、Shift で console の login に（UEFI の bootloader だけ、BIOS は後日）（2026-10-05 ユーザー） | incomplete（p001・p002・p005 cleared、p003 は T1-213 待ち） | — |
| [WS175](ws175/ws.md) | MG006 | Notes で PDF の画像と文字を編集（移動・大きさ・差し替え・挿入、文字の編集・削除・挿入・font、複数頁）（2026-10-06 ユーザー） | planning（ベータ2 の案） | — |
| [WS176](ws176/ws.md) | MG006 | Canvas: pen と touch のイラスト制作・画像編集（layer・brush・素材集・CLIP STUDIO と PSD の互換）（2026-10-06 ユーザー、ベータ3） | planning（ベータ3） | — |
| [WS182](ws182/ws.md) | MG006 | 電源ボタンで Log Out・Shut Down などのメニュー（ベータ2 の最後、2026-10-07 ユーザー） | planning | p001 設計 |
| [WS183](ws183/ws.md) | MG003 | I2C HID の Extended Interrupt と TGL の GPIO の group（5320 の touchpad を割り込みで、ベータ2・後回し） | planning | p001 |
| [WS184](ws184/ws.md) | MG006 | 左手デバイスの OSK（クリエイターモード: ダイヤル・ホイール・ボタン 2×5、左上の swipe で出す、ベータ2） | planning | p001 設計 |
| [WS185](ws185/ws.md) | MG006 | ゲームパッドの OSK とゲームコンソールモード（両上隅の同時 swipe、Xbox の pad を模す、段 1 は mview、ベータ2） | planning | p001 設計 |
| [WS186](ws186/ws.md) | MG003 | Realtek RTL8822CE（5320 の PCIe の WiFi、ベータ3、ベータ2 が早く終われば前倒し） | planning | p001 調査と設計 |
| [WS187](ws187/ws.md) | MG006 | ロック画面（大きな時計、下部から上へのスワイプ・wheel での解除、自動の lock の猶予、Password・PIN・Hardware Key の選択、2026-10-08 ユーザー） | incomplete | p001（q864、P2）→ p002 → p003 |
| [WS188](ws188/ws.md) | MG006 | app の OS の操作を libkeiland → compositor → backend へ（Settings の About・Storage・Users・Languages の残り）と境界の検査の強化（2026-10-08 ユーザー） | planned | p001 設計 |
| [WS189](ws189/ws.md) | MG006 | app の間の drag and drop（画像、受け入れの見た目、dock の spring-loaded、desktop に file、画面をまたぐ）（2026-10-08 ユーザー） | planned | p001 設計（q892） |
| [WS190](ws190/ws.md) | MG006 | 文字の欄と Text Editor の指での選択（ダブルタップ・端の drag・コピー・切り取り・貼り付け・すべて選択の popup）（2026-10-08 ユーザー、WS189 の後） | planned | p001 設計 |
| [WS191](ws191/ws.md) | MG006 | 再生の音を libkeiland の audio stream の口へ（compositor・backend、zedBSD audiod・Linux PipeWire・FreeBSD OSS）（2026-10-08 ユーザー） | planning | p001 設計 |
| [WS192](ws192/ws.md) | MG006 | 右上の状態の島を tap で開く glass の操作パネル（WiFi・音量・IME などを大きく、タブレット向け）（2026-10-09 ユーザー、ベータ2） | planned | p001 設計と実装 |
| [WS193](ws193/ws.md) | MG006 | make menuconfig のメニュー階層の作り直しと Build boot image（進捗の bar、-j$(nproc)）（2026-10-09 ユーザー、ベータ2） | planned | p001 実装 |
| [WS194](ws194/ws.md) | MG006 | make keiland-linux（apt・yum・pacman）・keiland-freebsd（pkg）の必要な package の確認と導入、build 後の install の確認（2026-10-09 ユーザー、ベータ2） | planned | p001 実装 |
| [WS195](ws195/ws.md) | MG006 | zedBSD でも userland/desktop を /opt/keiland/ に、account-admin を base から Keiland へ（2026-10-09 ユーザー、ベータ3） | planning | p001 設計 |
| [WS196](ws196/ws.md) | MG002 | useradd・usermod・userdel（2026-10-09 ユーザーの問い、ベータ3 以降の提案） | planning | p001 範囲 |
| [WS197](ws197/ws.md) | MG006 | Bluetooth のスマホ連携: MAP（SMS）・Integration（WS170）・PBAP・HFP（通話、SCO）、約 129 LW、ベータ3（code は branch agent/p1-ws197） | incomplete | p001・p002 cleared、p003 i03 の途中（再開の手順は ws.md） |
| [WS198](ws198/ws.md) | MG002 | zedBSD の上で zedBSD を self-build（host の clang、2026-10-09 ユーザー、優先度低、ベータ2 の見込み） | planning | p001 前提の調べ |
| [WS199](ws199/ws.md) | MG006 | Settings のセキュリティキーの独立の頁とウィザード（2026-10-10 ユーザーの UAT、ベータ2） | planned | p001 |
| [WS200](ws200/ws.md) | MG006 | Settings の Users のパスワード変更のウィザードと認証方式の選択（2026-10-10 ユーザーの UAT、ベータ2） | planned | p001 |
| [WS201](ws201/ws.md) | MG002 | /home の暗号化（UFS の先頭の key slot、FIDO2 の hmac-secret／PRF と回復のパスワード、master key は初回に /dev/random）（2026-10-10 ユーザー、ベータ3、今は検討しない） | planning | p001 設計 |

完了した WS の Phase の記録は 2026-09-24 に plan から削除した（git の履歴に残る）。

## Upcoming Work Outlook

見込みであって、約束や実行許可ではない。

<!-- master:outlook:start -->
2026-10-05 夜 Q1:

| 線 | 順 |
| --- | --- |
| T1 | T1-206 IPv6 → T1-203 の再実行（P1 の直しの後）→ T1-202 AAT → T1-205・T1-207 → 5330 の AAT の image の boot-test → WS101 の 5330 の測定（合間、lock の下） |
| P1 | T1-203 の解析と修正 → WS168 p002 の残り → WS130 p003 → WS172 p003 → WS161 p005〜p007 |
| P2 | AAT の直し（T1-202 の後）→ WS158 p004 → WS154 → WS145 → WS169・WS170 |
| ユーザー | 5330 の AAT の USB の起動 → UAT（時刻は追って）。WS153 の U2〜U15 |
| 後 | WS074 の B1（ユーザーの指示）、ベータ1 の release（WS129） |
<!-- master:outlook:end -->

---

# 付録

## 判断の記録（新しい順）

ユーザーの決定と Q1 の技術の決定の記録。決まった判断は先頭の open-decisions からここへ移す。各 WS の phase.md・ws.md が正本で、ここは索引。

<!-- master:decisions-log:start -->
- 2026-10-08 午後 ユーザー:「サウンドはlibkeiland-backendに入れてください。libkeilandのAPIはkl_audio_がいいです。」→ WS191 の音の出力は libkeiland-backend の中、公開の API は `kl_audio_*`。
- 2026-10-08 午後 ユーザー（クリック）: WS191 の Linux の再生の経路は「alsa-lib を dlopen」（ALSA の default、普通は pipewire-alsa で PipeWire）。Q1: D4 は stream ごとの接続のまま限りは pid ごと、p004 は Linux・FreeBSD の正弦波の試験の client を入れる（plan/ws191/phase001/phase.md）。
- 2026-10-08 夜 ユーザー:「SSHのホストキーは無視するか登録を削除していいです。」「N=2で再開します。」→ P1（BUG-267 の 2 本指の gesture → WS143 p005 i02b）、P2（WS177 M の T1 依頼 → WS191 の Linux の uid → BUG-269）を新しい世代で起動。
- 2026-10-08 午後 ユーザー:「SSHキーは自動で削除して受け入れてオーケーです。master.mdに書いておいてください。」→ 実機（5330・5320）の SSH の host key が変わっていたら、Q1・T1 は known_hosts の古い行を消して新しい key を受け入れてよい（聞かない）。ただし利用者の鍵（~kei/.ssh/authorized_keys に Q1 の公開鍵が無い）の時は Q1 から登録できないので、ユーザーに登録か image の作り直しを頼む。
- 2026-10-08 午後 ユーザー:「5330はつけっぱなしですので、Videoのテストで使ってよいです。アップデートや再起動は自由にどうぞ。」→ T1-435（WS083 の実機）を T1 に。UAT の USB-C DP は BUG-256 のまま（ユーザー「ディスプレイは点灯せず。Settingsに認識されていないです」）、P2 に割当（q898、WS191 は後）。
- 2026-10-08 午後 ユーザー:「ブラウザはベータ3に移します」→ WS074（Web ブラウザ）と q893（Browser の合成の確定・OSK の content type、T1-425 の残り）はベータ3。P2 は q893 を止めて WS083 へ。
- 2026-10-08 午後 ユーザー:「じゃあP2はi915 videoに回して」→ P2 は q893 を安全な地点で区切り WS083 Vulkan Video（q897）へ。WS191（再生の音）はその後。
- 2026-10-10 ユーザー:「ベータ3でホームディレクトリの暗号化を行います。WSだけ追加してください。検討は今は不要です。」（UFS の先頭の key slot、FIDO2 の PRF、/home だけ、inode の flag、master key は初回に /dev/random）→ [WS201](ws201/ws.md)（原文を記録）。
- 2026-10-10 ユーザー:「ws083-p005のテストをやってもらえますか？実機で」→ Q1 が 5330 で p005（bring-up・I frame 3 本）と p006b（P・B 3 本）を流し全部一致、hang なし。release の config の Vulkan Video はクリック「OFF のまま（推し）」（使う program は vkvideo-probe だけ、利用者は ZEDBSD.CFG に i915.debug=video で ON にできる。Video Player が使うようになったら ON）。
- 2026-10-10 ユーザー（クリック）: WS197 p004 の SMS の設計の判断 P1〜P7 は「全部推しどおり」（P8 は Q1 が推しで）。WS197 の branch を c56043c2b（p003 i06 まで、T1-526 の HID の回帰 PASS）まで main に merge。p004 を p004a・b・c に分けた。
- 2026-10-10 ユーザー:「P1はWS197に戻る前に、WS170のlibkeiland-backendにおけるMAPの実装について、bluetoothdをどう叩くのか、それから、bluetoothdに何が実装されるべきか、このあたりを明確にしておいて、別なセッションで判断の違いが生じないようにしてください。全般的に、SMS利用におけるPhone app, libkeiland, libkeiland-backend, bluetoothdの流れを明確にして、どのようなインタフェースになるか、関連WSに記載してください。」→ P1 が T1-523 の FAIL の後に plan/ws197/phase004/phase.md（main）に層の流れと各境界の interface を確定して書き、WS170 から link、design-reviewer。その後 WS197（T1-524 の HID の回帰が先）。
- 2026-10-10 ユーザー:「保留中のコードは随時mainに入れてOKです。」「時間的にも使用量的にも、デバッグしきると思いますので。」→ agent/p1-p045（WS001 p045 の POSIX の header: sys/socket.h の SO_*・MSG_*・struct linger、fcntl.h、小さい関数 22、cpio.h・tar.h）を main に merge（19b52e2f4、vmunix・libc.so の build rc 0・warning 0）。agent/p1-ws197 は P1 が i03 の後に随時 merge。
- 2026-10-10 ユーザー: 16 日までに使えるのは MAX プラン 3 つ分、「難しいバグもない状況なので、ほぼ確実に実装が終わります。SMSの送受信がスムースかどうかはわかりませんが。」体制はクリック「N=1 のまま」→ P1 が WS200 の後に WS197（branch agent/p1-ws197 に main を merge して p003 i03 から）。ベータ2 に入れるので、i03 が通ったら区切りごとに main へ merge する（release の bluetoothd が変わる、HID の回帰は T1 で確かめる）。
- 2026-10-10 夜 ユーザー:「WS197はbeta2.mdで必須に入れておいてください。」→ beta2.md の必須に WS197（約 100 LW の残り）。UAT: BUG-283・284・285・286 close、WS199 の頁・ログイン画面のキー OK（PIN 不要・タッチ不要は image が古く未確認）、WS200 は未実装、BUG-222 は 4.9 MB/s。
- 2026-10-10 merge 5515a4dab（WS199）: R3「置きっ放しもタッチ」を passkey-fido2 に、kernel の smartcard.c の drv_smartcard_card が card の出入りで KERN_SYSTEM_EVENT_USB の CHANGE（detail card=0|1）を post（Q1 の許し、include/uapi/system.h は注釈 1 行だけで layout 不変）、host-kl-system.c の printers の stub、fido2-p003-guest.sh の段 4 を security-keys の頁に。
- 2026-10-10 ユーザー（クリック）: WS199 R3 は「置きっ放しもタッチ」→ NFC の reader に載せたままの鍵もタッチと見なす（P1 の推しの「当て直し」は採らない）。
- 2026-10-10 Q1 判定: WS199 i02 の kernel の src/drivers/generic/smartcard.c（WS161 の file）の変更を許す: card の出入りで既存の KERN_SYSTEM_EVENT_USB の CHANGE（subject smartcardN、detail card=1|0）を post。UAPI・HAL は変えない。試験の整理: plan/ws172/tests/run-host-settings-keys.sh を削除（Users の頁の鍵の欄が消えた、master・未完了の Phase の参照なし）。fido2-p003-guest.sh は T1 の回帰で使うので段 4 を security-keys の頁に直す（P1）。
- 2026-10-10 ユーザー（WS199 の review の判断）:「Loginはタッチが必要。Unlockはタッチ不要。Unlockでは、スライドしないと認証画面に入れないので、キーが刺さったままでも自動認証される問題はない。」、keypad は推し、範囲は「全部ベータ2」、他は「全部推しどおり」→ plan/ws199/ws.md。
- 2026-10-10: T1-435 の ESP の書き込み（T1 の sshpass＋sudo＋nohup の script）が Claude Code の安全の判定で拒否された（回避せず停止）。ユーザー:「イメージごと自分で作り直して置き換えます。トップレベルのconfig.mkで入るようにしておいてください。」→ Q1 が top の config.mk（untracked）に `ZEDBSD_BOOT_EXTRA_LINES += i915.debug=video`・`ZEDBSD_USER_PROGRAMS += vkvideo-probe`・試験の stream 13 個を /root/ws083/ へ（ZEDBSD_TEST_EXTRA_FILES）を足した。ユーザーの image の後に T1-435 の A〜E を SSH で（ESP に書かない）。hang の F1・F2 は試験の kernel が要るので今回は外す。
- 2026-10-10 ユーザー:「Vulkan VideoよりもSecurityキーを優先します。」→ P1 は T1-521・522 の結果の debug の後すぐ WS199 の実装（T1-435 の結果を待たない）。T1-435 で FAIL が出ても WS083 の直しは WS199 の後。
- 2026-10-10 ユーザー（クリック）: T1-435（Vulkan Video の 5330 の実機、ESP の試験の kernel・cfg の切り替え・再起動）は「今流してよい」→ T1-521・522 の後に T1 が流す。ESP に書く前の一言の手順は省いてよい（ユーザーの許可）が、5330 が止まったら Q1 へ。
- 2026-10-10 ユーザー: passkey と fidoctl は「現在のあなたの設計がすぐれているので、そのままにします」。Settings のセキュリティキーの頁（Software Security Key を含む）とログイン画面のキーの自動のログイン（PIN 不要・タッチ不要の設定、0.5 秒の確認の表示、OSK を PIN の欄の下）をベータ2 で → WS199 に統合（決定: ロック画面のタッチ不要はロック後に挿したキーだけ、組み合わせ 3 通り、キーからユーザーを選ぶ、変更にパスワードと警告）。「現在走っているバグ修正、テストをすべて終わらせてから、セキュリティキーの新規実装に移ります。」BUG-285 は「(B) 登録も数える」（c803b9291）。
- 2026-10-10 ユーザー「これで一通りUATの確認事項は確認したと思います。CloseできるものはCloseしましょう。USB LANの遅さ、だけが残りました。」 → WS192 completed（ベータ2 の範囲）、ws183-p002 cleared、WS187・WS161（USB）・WS090 の UAT を記録、BUG-278 close。
- 2026-10-10 ユーザー:「Bluetoothはキーボード、マウスについて接続確認、利用可能であることを確認できました。」 → BUG-275 close、ws143-p008 cleared。Bluetooth の HID はベータ2 に入る（10/16 に OFF にしない）。
- 2026-10-10 ユーザーの UAT: FIDO2 の鍵の登録と login ができた（BUG-279 close）。SSH 越しの kernel の更新と reboot を確認（BUG-269）。ifconfig は 2500Mbps（BUG-222）。セキュリティキーの独立の頁とウィザード → [WS199](ws199/ws.md)、Users の頁のパスワード変更のウィザードと認証方式の選択 → [WS200](ws200/ws.md)、時期はクリック「ベータ2 に入れる」。ロック画面の button の高さ → BUG-283（ベータ2）。
- 2026-10-10 Q1 の記録: WS141 の別 session が main の checkout の main を自分で 2 回 fast-forward した（4d1e34392、c603fd475＝64007b3e7・b2a07f3d5 と Q1 の 7ca3c5e40 の merge）。中身は plan/ws141/ と platform/arm64/vmunix.mk の bcm2711/vulkan-input.c の 1 行だけで、release（amd64）に影響なし。2026-10-09 のユーザーの取り決めは「パッチをあなたに提供するので、Q1がマージします」なので、ユーザーに確かめる。
- 2026-10-10 ユーザー:「ue0のスピードですが、ifconfigやSettingsでLink Speedも表示するのがいいです。まずはそこからです。」→ BUG-222 の最初の作業として P1。
- 2026-10-10 ユーザー（クリック）: BUG-280（App Home への遷移が Linux より滑らかでない）は「ベータ2 は既知の問題（推し）」。2026-10-10 ユーザー: BUG-276 の症状は「範囲選択した状態でダブルタップしようとすると、最初のタップで選択が解除されるバグです。」（P1 が直し直した、e0ca68bdd）。
- 2026-10-09 ユーザー（クリック）: WS197 の Q16（MAP 1.4・PBAP 1.2 は GOEP 2.0／L2CAP ERTM が必須と P1 が仕様で確かめた）は「(a) 1.1 で RFCOMM だけ（推し）」→ MAP 1.1・PBAP 1.1、ERTM と GOEP 2.0 は Future Work。
- 2026-10-09 ユーザー（クリック）: WS197 の設計の判断 Q1〜Q16 は「全部推しどおり」（スマホ 1 台、中継だけで保存は ~/Documents/Phone、過去 30 日・最大 500 通、MMS は範囲外、CVSD の後に mSBC、echo の打ち消しなし、lock 画面で着信に応答、Linux・FreeBSD は作らない、Android が先で iPhone は HFP の後、logout で切る、通話中の蓋は suspend、MAP・PBAP 1.1 と RFCOMM）。見積もり約 154 LW。
- 2026-10-09 ユーザー（クリック）: WS198 は「ベータ3 で S1、推しどおり」→ D1 ベータ3、D2 base の make を直す、D3 tar と圧縮を base に作る、D4 S1（base と desktop の image、外の package なし）が完了の条件、D5 toolchain の規則の変更は Q1 の許可で WS198 が行う。
- 2026-10-09 ユーザー:「これは優先度は低いですが、zedBSD上でzedBSDをセルフビルド可能にするWSを作りましょう。このケースではホストclangのみ使えばいいですね。工数もそれほど大きくないので、ベータ2に入りそうです。」→ [WS198](ws198/ws.md)。P1 は menuconfig の Noct・Emacs の後に p001（前提の調べと工数）、その後 WS197 の設計。
- 2026-10-09 ユーザー:「make menuconfigで、Noctはuserland/base/noct/にあるけど、Baseメニューにないようなので、追加してください。base/emacsもBaseメニューに追加です。Emacsの依存はbase/noct/に修正です。」「PackagesメニューからからNoctを削除してください。Baseに移動するためです。」→ WS193 の追加の作業として P1（WS197 の設計より先）。
- 2026-10-09 ユーザー:「ld.soの変更は承認します。マージしていいですよ。」→ c03fac053 を main に merge（上の保留を取り消し）。ws074 の旧の rtld-dlopen・rtld-probe の試験を削除。T1-507（rtld の回帰）・T1-508（python3-guest）を依頼。
- 2026-10-09 Q1 判定: P1 の T1-495 の直し c03fac053（src/rtld/rtld.c の dlopen が slash を含む任意の path を開く。BUG-083 で agent が決めた「/lib 以外の絶対 path は断る」を覆す、POSIX の dlopen の形）は、release の image の ld.so を RC の直前に変えないため 10/17 の後に merge（branch agent/p1-rtld）。Python はベータ2 の release に入っていない。merge の時に plan/ws074/tests/rtld-dlopen.sh・rtld-dlopen.c・rtld-probe.c（旧の断りを確かめる、Tools に無い）を削除する。
- 2026-10-09 ユーザー:「Bluetoothは、SMSのMAP, CallsのHFP, PBAP, を実装したいですが…今はANCSは実装しなくていいですが、見積もりだけでも。」「OBEX, MAP, Integration, PBAP, HFPの順で実装しますか。beta2.mdの必須が終わってからです。」→ [WS197](ws197/ws.md)（約 121 LW、OBEX の下の RFCOMM を p002 に含める）。Q1: 着手は beta2.md の必須の後、code は 10/17 まで保留の branch（release の bluetoothd を RC の後に変えないため）。
- 2026-10-09 ユーザー（クリック）: WS195 の D1〜D7・WS196 の E1〜E6 は「全部推しどおり」。Q1: Noct v2.0.3 の取り込みで、この checkout の target の source tree が「同じ commit の時だけ置き換える」規則で止まったので、userland/base/noct/Makefile の置き換えの条件を「別の release か別の patch」に広げた（toolchain の build の規則、ユーザーの取り込みの指示の範囲）。共有の build/NoctLang は 2.0.3 に入れ替え済み（旧は build/NoctLang.old-2.0.1、T1 の build の確認の後に消す）。host・target とも build rc 0、interpreter.c の -Wreturn-type は消えた。
- 2026-10-09 ユーザー:「指摘のあったNoctのmissing-returnのバグは、上流でv2.0.3にアップデートして修正済みです。取り込んでいただければと思います。」→ Q1（toolchain の変更はユーザーの指示）: userland/base/noct/version.mk を v2.0.3（f6efa83a8、前の snapshot fcf5759 からの差は interpreter.c の missing-return の直しだけ）に。共有の build/NoctLang は T1 の build の合間に Q1 が入れ替える。2.0.1 を決め打ちした plan/ws035/tests/baseline-toolchain.sh（完了した WS の試験、参照なし）は試験の整理の基準で削除。
- 2026-10-09 ユーザー:「WS143, WS083を必須項目に移して、beta2.mdを更新してください。BUG-184は確認できたのでCloseです。」→ beta2.md の必須に移し（計 39.5 LW）、BUG-184 を resolved。
- 2026-10-09 ユーザー:「userland/desktop/のインストール先を、zedBSDでもLinux/FreeBSDに合わせて、/opt/keiland/にします。account-adminはKeilandの必須バイナリとして、/opt/keiland以下に移します。baseから移動してください。」時期はクリック「ベータ3（RC の後）」→ [WS195](ws195/ws.md)。「useradd/usermod/userdelは別途、実装が必要な認識」→ [WS196](ws196/ws.md)（POSIX に同等の utility は無い）。Q1 判定: P1 の ws001-p045 の d42615bc7（sys/socket.h の SO_LINGER 等・fcntl.h）は RC の直前に package の挙動を変える危険があるので、10/17 の後に merge（branch agent/p1-p045）。
- 2026-10-09 ユーザー:「beta2.mdの必須の項目は、P1担当分はこのまま消化しきってください。そのあと、UATは少し遅れるので、WS143, WS083もP1で完了を目指してください。」「BUG-237, BUG-271, BUG-219, BUG-232, BUG-235, BUG-179, BUG-180は確認できたのでCloseです。」→ 7 件を resolved。P1 は beta2.md の必須 → WS143 → WS083 の順、ws001-p045 は後。
- 2026-10-09 ユーザー（クリック）: ベータ2 の FFmpeg 9.0.2（LGPL）の source の提供は「ffmpeg.org への link だけ」→ release notes に版と ffmpeg.org の link、zedBSD の patch と configure の引数は GitHub の tree にある旨を書く。release の asset に tarball は載せない（Q1 は tarball を推したが、ユーザーの決定）。
- 2026-10-09 ユーザー:「WS143 Bluetooth の HID（BR/EDR・LE のキーボード・マウス）ですが、現在の判断ではベータ2に入れます。間に合わなければ直前でOFFにします。」「WS083 Vulkan Video（H.264）ですが、現在の判断ではベータ2に入れます。間に合わなければ直前でOFFにします。」「機能の凍結の日は10/13を目標にしますが、ベータなので、UATフィードバックのバグを直しきれなくてもいいです。そういう意味では、code freezeはぎりぎりまでやらないかもしれないです。」→ plan/beta2.md を更新（LW 表記、計 53.5 LW）。
- 2026-10-09 ユーザー（クリック）: ベータ2 の release の config に Bluetooth（WS143、intelbt-firmware）を「入れる、動かなければ既知」→ config/release/config-amd64-beta2.mk に足す（P1 q920）。10/12 の凍結までに HID が 5330 で動かなければ release notes の既知の問題に書く。
- 2026-10-09 ユーザー（WS193 の設計の問い）: 原文に無い項目（Variant の disk の形・kernel option・driver の選択・試験の hook・Noct の GPU accel）はクリック「menu から外す」。user program の分類は「Firmwareはトップレベルに階層を作る。X11とTestsはメニューから削除し、直接記述のみにする。」→ menu から外した物は config.mk の直接の記述だけ（読んだ値は保つ）。Firmware は toplevel の Packages の次（Q1 の判断）。
- 2026-10-09 ユーザー: make menuconfig のメニュー階層の変更（toplevel: CPU / Board・Boot Option・Development・Base・Desktop・Packages・Build boot image・Exit、原文は [WS193](ws193/ws.md)）、Build boot image は進捗の bar と対象の名前、「このメニューに限り、nprocの数だけ-jしてOKです」。make keiland-freebsd・keiland-linux は必要な package を確かめてから導入、build 成功後に install を確かめて実行、「テストはaptだけでいいです」→ [WS194](ws194/ws.md)。時期はクリック「両方ベータ2」。q918・q919 P1（WS192 の後、ws001-p045 より先）。
- 2026-10-09 ユーザー（クリック）: WS192 の島の press は「全部パネルに統一（P1 の案）」→ mouse・touch とも島の press はパネル。個別の popup はパネルの行の「›」から。mouse の近道（network の Alt+click、音量の wheel）は残す。
- 2026-10-09 ユーザー:「右上の通知アイコン領域は、タブレットでは個別のアイコンのタッチが難しかったです。…通知アイコンの島をクリックやタッチすると、ポップアップが画面右上に表示されて、そこに大きめのメニューで、WiFiボタン、音量スライダー、IMEアイコン、などを表示して、操作可能にしたいです。ポップアップはglassエフェクトがいいです。」、時期はクリック「ベータ2（RC 10/13 まで）」→ [WS192](ws192/ws.md)、q917 P1（UAT の debug の次、ws001-p045 より先）。
- 2026-10-09 ユーザー:「WS141を別なセッションで実行します。ws141/ws.mdはそのセッションが排他的に更新しますが、master.mdは更新しません。同じソースツリーを使いますが、作業は別なディレクトリで行い、パッチをあなたに提供するので、Q1がマージします。」→ WS141 は別 session が実行。plan/ws141/（ws.md と Phase）はその session だけが書き、Q1・P1 は書かない。master.md・queue.md などの共有の記録は Q1 だけが書く。届いた patch は Q1 が main に適用する（commit は WIP）。QEMU は host で同時に 1 つの規則があるので、T1 と時間が重ならないよう Q1 が調整する。
- 2026-10-09 ユーザー（クリック）: WS126 p004 の D1（依存の package の要る追加の module）は「今は足さない」→ p004 は保留、今の module で p005 の image へ。
- 2026-10-09 ユーザー（クリック）: libc の wint_t（uint32_t、clang は int）は「WS001 p045 で直す」→ libcxx の作り直しと一緒に ws001-p045 で。今は変えない。
- 2026-10-09 ユーザー:「WS001に、POSIXのヘッダがすべてそろっているチェックして揃えるPhaseを入れておいてください。」→ [ws001-p045](ws001/phase045/phase.md)（planned、ベータ3 の P1 の列）。
- 2026-10-09 ユーザー:「<sys/socket.h> ですが、libcに入れてくれますか？」→ ws126-p002 の SOMAXCONN（128）は libc の include/libc/sys/socket.h に置き、uapi と kernel の unix-socket.c は変えない（Q1 の uapi 案を取り消し）。
- 2026-10-09 Q1 判定: P4（Sonnet 5.5・effort low）の ws183-p003 規約の見直しは合格（fbd1da9e7）。手順（違反の一覧・build warning 0・host 試験・記録・cleared 候補で返す）を省かず、動作の変更なし、comment は正確。Haiku 4.5 low は手順を省いた（前記）。規約の見直しは Sonnet 5.5 low で足りる。P4 は終了。
- 2026-10-09 ユーザー:「では、P3はラップアップします。P4は作業ができたかどうかを評価したら終了します。N=1でP1のみで継続します。」→ P3 は安全な地点で終える。P4（Sonnet 5.5 low の試し、WS183 の規約の見直し、.claude/agents/p3-conformance-sonnet-low.md が読み込まれたら起動）は 1 回の評価で終わる。以後 N=1（P1 だけ）＋T1。
- 2026-10-09 Q1 の評価: P3 の Sonnet 5.5 medium の 1 回目（ws189-p005）は合格。指示（cleared にしない、試験の file も、Linux の build と host 試験）を全部守り、約 40 か所を直し、Haiku の注釈を確かめ、他の WS の既存の違反は記録だけにした。以後 P3 は Sonnet medium で続ける。
- 2026-10-09 ユーザー（クリック）: P3 の規約の見直しは Haiku Low の 3 回の評価（機械的な直しはできるが、指示を守らず自分で cleared・依頼した build と試験を流さない・試験の file の指摘を残す・注釈の誤り）の後「Sonnet 5.5 に変える」→ P3 は conformance-reviewer（Sonnet、effort medium）で続ける。
- 2026-10-09 未明 ユーザー:「N=2で作業してください。変則的に、P1, P3で処理します。」「下記をベータ2のデバッグの合間にスケジューリングします。ベータ2のUATデバッグが優先です。スケジュール先はP1です。ベータ2のすべての作業をP1でスケジューリングして行います。WS031, WS075, WS068, WS052, WS172, WS095, WS155, WS046, WS004, WS001 (上限4LWで止める), WS126, WS171, WS009 / WS026 / WS106, WS139 / WS094 p009, BUG-255」「P3は、下記の作業のみをHaiku 5.5 Lowで実施しましょう。…最初の作業のときにモデル能力による失敗があるかQ1が評価してみてください。Coding-standard reviews」→ P2 は止める（残りの BUG-273・272・173・242 は P1 へ）。P1 = ベータ2 の全部（UAT のデバッグが先）と合間のベータ3 の上の項目。P3 = 規約の全文の見直しだけ（Haiku 5.5 は無いので Haiku 4.5・effort low、Q1 が最初の結果で能力を評価）。
- 2026-10-08 夜 ユーザー:「では、その残りを片付けましょう。AATで判別できずUATが必要なところはブロッキングにしてください。」→ ベータ2 の残りを続ける。AAT（QEMU）で判定できず実機の UAT が要る Phase・Bug は Status を blocked（UAT 待ち）にして先へ進む（cleared にしない、UAT の一覧に載せる）。
- 2026-10-08 夜 ユーザー:「下記は実装の変更が多かったので、いったんすべてcloseします。」→ BUG-247・249・251・252・254・261・258・259・265・269・270・172・191・176・183・185・187・193・195・196・208・209・211・213・214・221・226・229・230・231・233・234 を resolved。BUG-222 は同じ一覧にあったが直前に「直ってないです。継続」と言われたので tracking のまま（Q1、ユーザーに確かめる）。
- 2026-10-08 夜 ユーザー（network の Bug）: BUG-157・169・183・185・187・213 は close、BUG-174 は「NATの向こう側だからです。closeします。」、BUG-189・BUG-212 は「継続。再現しなければclose」、BUG-222 は「直ってないです。継続」。
- 2026-10-08 夜 ユーザー:「BUG-221, 226, 239はcloesします。」→ resolved（P2 が Board と ticket に反映）。
- 2026-10-08 夜 ユーザー:「Touchpadは動作確認できたので、下記をすべてclose します。BUG-156・166・167・178・190・211・215・216・228・247・254、BUG-218 の touchpad の部分」→ resolved（P2 が Board と ticket に反映、BUG-218 の Phone の padding は T1-481 待ち）。
- 2026-10-08 夜 ユーザー:「BUG-220, BUG-236,BUG-227,  BUG-241,はcomplete.」→ resolved（P2 が Board と ticket に反映）。
- 2026-10-08 夜 ユーザー:「UI関連のバグは、なんだかほとんど片付いているのに残っている気がします。まとめてAATを流して、再現できなかったらcloseしたいです。」→ q911（P2）: UI の Bug を QEMU の AAT（aat-input の pointer・key・touch）でまとめて再現を試し、再現しない物はユーザーの決定で resolved（unreproduced、AAT の証拠つき）に。実機の device（touchpad の物理の click・5330 の network など）が要る物は QEMU で閉じず 5330 の UAT の一覧へ。
- 2026-10-08 夜 ユーザー: BUG-225・BUG-101・BUG-201 は completed（resolved）、BUG-130 は「今テストしたら動いているので、もう問題ないかも」→ resolved、BUG-013・023・024・025（PC-98・LX6）はベータ4。
- 2026-10-08 夜 ユーザー（クリック）: 窓の整列（WS181）の記憶は「前の形と窓の位置を覚える」→ 同じ desktop で整列を開き直して同じ形を選ぶと前と同じ窓を同じ枠に戻す（compositor の中だけ、logout・再起動では消える）。設計 §5.3 の「整列の記録は残さない」を変える。
- 2026-10-08 夜 ユーザー:「ネットワークプリンタはこれしか持ってないので、今はIPPのテストは難しいかも？LPDが動く事実があればいいよ。POSIX.1-2024はlpしか求めてないし」→ 実の printer での IPP の再試験はしない。BUG-271 の chunked の直しは mock の試験だけで（実機の確認は無し、IPP の実機は「未実施」と記録）。
- 2026-10-08 夜 ユーザー（クリック）: Brother（10.0.30.6）に残った試験の job 213・214 は「Q1がIPPで取り消す」→ P2 が host から IPP の Cancel-Job を送る。
- 2026-10-08 夜 ユーザー（ベータ2 の残りの表への回答）: WS113 は実機でディスプレイの切り替えを確認、WS050・WS051・WS090・WS183 は実機で動作を確認。WS156 は未テスト。WS083 は Q1 が 5330 で SSH で試す（5330 は ON）。WS191 は Linux の後に FreeBSD も（T1-448 で FreeBSD も済み）。WS177: printer は 10.0.30.6 を使ってよい。P・P2（Phone）は Bluetooth で通信する Android の app にしたいので Bluetooth の後。U は完了かの問い（Q1: WS181 の本体は済み、U は p003〜p005 の準正常系で未着手）。W はベータ3。A2（Settings の Notifications の頁）はやってよい。T（video の container・GPU の decode）は通常の優先度で。
- 2026-10-08 夜 ユーザー（クリックの自由記述、BUG-267）:「一番外側の指が画面端にあれば、ほかの指は画面上のどこにあっても、画面端の2本指スワイプにします。あと、Settingsのスクロールが2本指でできることを確認しました！」→ 端の複数本の指の swipe は、一番外側の指が端の帯にあれば他の指の位置は問わない（P1 の 192 px の制限を外す）。真ん中だけの複数本の swipe は切り替えにしない。
- 2026-10-08 午後 ユーザー（クリック）: Mail の添付（ws189-p004）より先に **WS143 p006 Bluetooth の desktop**（Settings の頁・system bar・pairing の確認の窓、app → libkeiland → compositor → backend）を P1 で。p004 はその後。（Q1 の推奨は WS083 Vulkan Video だった）
- 2026-10-08 午後 ユーザー（クリック）: WS189 の DnD で、drop の前の wl_data_offer.receive を compositor が空にする（データは落とした窓にだけ渡す。drop の前に中身を読む GTK4 などは空を読む）→「これでよい」。ws189-p001 は Q1 の判定で cleared。
- 2026-10-08 朝 ユーザー:「/dev/hid-hostは、/dev/input/bridgeに変更し、/dev/bt0は、/dev/bluetooth0に変更できますか？」→ クリックの回答「揃える」: node を `/dev/input/bridge`・`/dev/bluetoothN` に。UAPI も揃える（include/uapi/hid-host.h → input-bridge.h、HID_HOST_* → INPUT_BRIDGE_*、struct も input_bridge_*）。bluetooth.h と BT_IOC_* は元から Bluetooth の名前なのでそのまま。P2 が i01c の前に行う。
- 2026-10-08 昼 ユーザー（クリック）: WS189 の DnD は専用の受け渡し・同じ app の中は app で・画像（PNG）から・見た目は compositor と app で分担・dock の spring-loaded・desktop に file・画面をまたぐ。WS190 の popup はコピー・切り取り・貼り付け・すべて選択。Files の mount の表は kl_system へ、再生の音は libkeiland の audio stream の口へ（WS191）。
- 2026-10-08 朝 ユーザー（クリックの回答）: USB メモリは最新 main で UAT の image を作る（Q1 が build、書き込みはユーザー）。WS143 B6: account が無い install では bluetoothd は起動しない（今の暫定のまま）。ws143-p005 Q4: ペアリングの後に自動で接続する。Q5: 人が切断した機器からの再接続は断る。Q2: `/dev/hid-host` に HID_HOST_GET_DEVICE を足す。Q1: i2c-hid も共有の HID の glue（hid-input）に乗せる（推しと逆、F-084 を実施に）。libbrowser に要素を探して focus する口を足してよい（描画の改善は止めたまま）。BUG-241 はベータ3。BUG-225 は実機の UAT で見てから。Music のプレイリストなど（backlog-p2 111）は範囲外（後で）。
- 2026-10-08 夜 ユーザー:「5330はsudoを勝手に使ってよいです。SSH鍵も勝手に更新してください。アップデートして再起動もお願いします。」「BUG-244: はい、優先度を上げてください。」「BUG-200:は解決でOKです。」「BUG-201は保留します。」「BUG-224はいったん閉じてください。」「切り替えのつまみがなんなのかは保留です。」「いったん寝ます。判断事項は起きたらお願いします。ブロッキングしても自走をお願いします。」
- 2026-10-08 ユーザー: WS005 completed（記録のミス）。規約の整形（各 WS の全文規約の Phase）はベータ3。WS172 はベータ3。WS187 にロック画面の解除（下部から上のスワイプ・wheel の上、自動の lock の後の一定時間は認証なし、手動の lock は常に認証、Password・PIN・Hardware Key の選択）を追加。
- 2026-10-08 ユーザー: (1)「2番目以降のディスプレイのdock barには、その画面に置いた window の window icon を出し、時計・状態・App Home・切り替えのつまみも表示する。」（ws113-p015）(2)「5330はいつでも再起動OKです。アップデートもOKです。」(3) UAT: タップの判定の遅れ → ws183-p002（q863）、USB メモリ → BUG-258、PDF viewer のリサイズの重さ → BUG-259。(4) 緊急のラップアップの後、週間の使用量 99% で別のセッションへ引き継ぎ（plan/agents/wrapup-20261008.md）。
- 2026-10-08 ユーザー「ベータ3にします：WS009・026・106 文書・試験の整理・試験アプリの集約、WS139 デスクトップの速さ」 → 4 つの WS の Target をベータ3 に。
- 2026-10-08 ユーザー:「『そのほか』は見積もりが甘いです。releaseの作業は明らかに10/13以降です。Linux/FreeBSDも10/13以降です。翻訳はベータ3に回します。」→ WS129（release）と Linux・FreeBSD の作業（WS112、WS131 の 3 OS の回帰ほか）は 10/13 以降、WS158（翻訳）はベータ3。
- 2026-10-08 ユーザーの UAT（5330、HDMI）: p007 の mouse の跨ぎと窓の移動は OK → ws113-p007 cleared。2 つ目の display のリサイズ不可、display ごとの dock の bar・docked/floating/整列の状態、App Home の時は他の display を背景だけに → [ws113-p015](ws113/phase015/phase.md)（P1、WS113 は優先順の 1 番なので WS090 の今の単位の後すぐ）。「次はUSB-C DPにしてみます。」
- 2026-10-08 ユーザー:「sample.mp4はffmpegで変換してOKです。sudoが使えるのでインストールしてOKです。」→ Q1 が /home/awe/zedbsd-media/ に H.264 の Constrained Baseline・Main・High（1920x1080、150 frame、MP4 と Annex B の .h264、High＋AAC の MP4）を作った（ffmpeg 7.1.5・libx264、SHA256SUMS・README.txt）。repo には入れない（大きさと出所）。WS083 の実機・WS122 の player の試験に使う。
- 2026-10-08 ユーザーの UAT（5330 の実機、p011 の kernel・main の compositor・settings）:「HDMIに出力されました。extendもmirrorも動いています。マウスはまだextendに移動できません。extendのとき、個別のディスプレイをオフにできる設定を作ってください。そうすれば、タッチパネルのテストができるのではかどります！」→ ws113-p004b・p006・p011 は実機で動いた（mouse の移動は p007 で作業中）。[ws113-p014](ws113/phase014/phase.md)（拡張の時の display ごとの off、D-MODES を置き換え）を P1 の p007 の後に。
- 2026-10-08 Q1: 5330 に kernel（P2 の ws113-p011、cksum 1836440229）・compositor（3117189562）・settings（1903582430）・libkeiland.so（459659771）を入れ（元は /esp/vmunix.prev・/bin/wayland.orig・/bin/settings.orig・/lib/libkeiland.so.orig）、SSH で reboot（効いた）。HDMI を挿した起動で `display head: lit (connector 1, HDMI, 1920x1280, pipe B)`・`first frame 1920x1280 shown`、`resident display: lit again for two pipes`。ユーザー: 動画の試験に ~/sample.mp4（HEVC Main 1920x1080 150 frame、AAC。WS083 は H.264 だけなので用途を確認中）。
- 2026-10-08 ユーザー:「では、Alt+Shift+左右で仮想デスクトップ移動、ではどうですか？私はEmacsユーザなので、標準テキストエディタがどう使われているのか知りませんでした。」→ ws181-p010 の key を Alt+Shift+左右に。
- 2026-10-08 ユーザー:「Super+Shift+左右、使いやすくてすばらしい設計ですね！…あとでいいので、Ctrl+Shift+左右で、Virtual Desktopを移動できるようにお願いします。」→ ws181-p010（後で）。Ctrl+Shift+左右は app の単語の選択と重なる点をユーザーに確認中。
- 2026-10-07 ユーザー:「そもそもハードウェアがおかしいかも。蓋を閉じても画面が消えないのに、蓋をちょっと開けると画面が消えます。方針を変更して、蓋を閉じたら自動で変更するのは後回しにして、そのためのトークンのリソースをSettingsからのディスプレイ変更に使いましょう。」→ BUG-255（蓋で HDMI へ移る）と N8 の蓋の自動の切り替えは後回し（ベータ3、WS052 と同じ）。P2 は BUG-255 を止め、ws113-p011（Settings の拡張・mirror に要る i915 の 2 出力）へ。
- 2026-10-07 ユーザー（優先順位）:「え、Vulkan Videoよりもシェーダーを優先してたんですか？…Videoが優先だと伝えたはずですし、シェーダーは優先度が低いと伝えたはずだったのですが。気をつけてください。改めて話すと、Settings (display), USB-C & DP Alt, Vulkan VIdeo, widget, 通知, タッチパッド割り込み, YubiKey Passkey, 写真, カレンダー, Bluettoh, 左手デバイスOSK, ゲームパッドOSKくらいの順で、そのあとに残りですね。明確に優先度を下げて、空き時間にやるのは、IME, RTL8822C, Vulkan executor, i915高度化、です。そのほかは両者の間くらい。」
  → **ベータ2 の優先順**: 1 WS113（Settings の Display）、2 WS051・WS050（USB-C・DP Alt）、3 WS083（Vulkan Video）、4 WS090（widget）、5 WS156（通知）、6 WS183（touchpad の割り込み）、7 WS161・WS172（YubiKey・passkey）、8 WS157（写真）、9 WS155（カレンダー）、10 WS143（Bluetooth）、（11・12 の左手 OSK・gamepad OSK はベータ3 へ移した）、その後に残り。**空き時間だけ**: WS095（IME）、WS186（RTL8822C）、WS031（Vulkan の executor）、WS075（i915 の高度化、shader の compiler を含む）。他はその間。
- 2026-10-07 ユーザー「下記をベータ3に移動します。・左手デバイスOSK、ゲームパッドOSK, 写真の続き, カレンダーの続き, IMEの続き、POSIX, NVMe, make, RTL8822C, Sleep」→ WS184・WS185・WS157（続き）・WS155（続き）・WS095（続き）・WS001・WS004・WS046・WS186・WS052 の Target をベータ3 に。
- 2026-10-07 ユーザー:「修正完了後に、5330のカーネルとコンポジタを入れ替えてください。再起動もやってほしかったのですが、rebootコマンドはリセットができていないことを確認しました。」→ BUG-255 の後に Q1 が 5330 に kernel（BUG-249 の reboot の fallback 入り）と compositor を入れ、SSH で再起動する。今の 5330 の kernel（cksum 2877465438）は BUG-249 の前なので、その reboot は効かない見込み（BUG-249 は 5330 でも起きる）。新しい kernel が入った後の reboot から効く。
- 2026-10-07 ユーザー:「P2はP1の仕事を少し分担できますか？…もし分担しても効率化できなそうなら、左手デバイスですね。」→ Q1: P2 は BUG-254 の後に WS113 p011（i915 の 2 出力の同時、DBUF は「足す時に 1 つ目を点け直す」）を受け持つ（i915 の側、P1 の compositor の p004b〜p007 と並べられる）。P1 は p004b → p005 → p006 → p007。
- 2026-10-07 ユーザー:「RTL8822Cは、WSを立てて、ベータ3にしておきます。けど、ベータ2が期日前に完成したら、やるかもしれません。」→ [WS186](ws186/ws.md)。
- 2026-10-07 ユーザー（ws113-p011 の DBUF）:「2 つ目の画面を足す時に、1 つ目の画面を点け直す、でお願いします。」→ 1 出力の時の DBUF・既存の eDP の run は変えず、2 つ目を足す時に resident を点け直す（一瞬消える）。Q1: 5330 に kernel（P2 の build/bug250、cksum 2877465438、BUG-251・253・eDP の connected）と compositor（main、cksum 184482127、BUG-252・ws181-p009）を入れた（元は /esp/vmunix.prev・/bin/wayland.orig）。
- 2026-10-07 Q1: ws075-p007b b2（d23a536b9）を merge。GS の無い draw の dword も変わる（PUSH_CONSTANT_ALLOC_VS 16→8 KiB・ALLOC_GS・CLIP bit5）ので、**main の kernel を実機に入れる前に T1-363（5330 の passthrough の vkx・vke・vkc・zdesktop の capture）**。5330 が zedBSD で動いている間は passthrough ができない。BUG の確かめで 5330・5320 に入れる kernel は d23a536b9 の前（P2 の build/bug250 の系統）か T1-363 PASS の後に。
- 2026-10-07 ユーザー:「シェーダはやることがないときに取り組んでほしいです。Settingsのディスプレイ設定を、拡張・ミラーありで実装する作業に切り替えてください。」→ P1 は ws075-p007b の b2 を区切り、q855: WS113 p011（i915 の 2 出力の同時）→ p004b（compositor の複数の出力、全拡張・全 mirror）→ p005（kl_system_displays・明るさ）→ p006（Settings の Display の頁: 拡張・mirror の二択、配置の drag、明るさ）→ p007（窓の出力の所属）。シェーダ（WS075）は他にやることが無い時だけ。
- 2026-10-07 ユーザー:「今後、実機のホスト鍵は無視してクリアして接続してください。」→ 実機（10.0.30.3 の 5330・10.0.30.5 の 5320）の SSH の host 鍵が変わっていたら `ssh-keygen -R` して `StrictHostKeyChecking=accept-new` で繋いでよい（他の host は今までどおり確かめる）。
- 2026-10-07 Q1: ws051-p004b の code（c1dd58f19、D1〜D8、host 試験 PASS、build warning 0）を merge。判断: (a) present.c の `display_failed` が 1 度の run の失敗で以後の presentation を全部失敗にするのは p011a の「失敗したら元の出力へ戻す」と食い違う → 次の P1 が直す（ws113 の file、Q1 の許可）。(b) `userland/tests/display-control` に `--index=N` を足す（DP-2 を選んで claim するため、P1、Q1 の許可）。素の 5330 の確認に「display-control で DP-2 を claim して present → release → 10 秒で eDP に戻る、link 162000 kHz x4 24 bpp、USB-C→HDMI の adapter も」を足す。
- 2026-10-07 ユーザーの UAT（5320）→ [ws181-p009](ws181/phase009/phase.md)（P2 の q852 に足す）。5320 で HDA の音が鳴った（ユーザーの報告）。
- 2026-10-07 Q1: ws113-p011a（9036a7ad4、i915 の resident の出力の付け替え・connector ごとの ID と generation・R1〜R4、compositor の蓋で外部へ移る R4）を merge。q850 は finished（試験待ち: T1-357 の QEMU、5330 の HDMI の UAT（phase011a/phase.md の「確認」(1)〜(4)））。P1 は ws075-p007b を区切って ws051-p004b の code へ。
- 2026-10-07 Q1: ws051-p004b の設計（f03f85f55）・p005a（25a49906c、HPD の long pulse → detect → GPU_DISPLAY_EVENT_CHANGE、2 秒の猶予、IRQ_HPD の確かめ）を merge。素の 5330 の確認に「起動の後に TC2 へ monitor を挿す・抜く・挿す」（HPD-EVENT DP-2 の connected/disconnected、sequence が進む、PHY が返る、HPD storm が無い）を足す。p004b の code は P2 の ws113-p011a の後。P1 はその間 ws075-p007b（b1 から）。
- 2026-10-07 Q1: ws051-p004a（6f9f7e7d8、TC の AUX・外部 DP の object・H7・M2）を merge。素の 5330 の確認（USB-C の DP の monitor を TC2 に、dmesg に `DP-ext TC2: connected`・rate 540000 lanes 4・EDID 1920x1280・PHY の返却・`hpd DP detect DP-2: connected`、抜いた起動で timeout 無し）はユーザーの操作が要る → ws084 の 10 回の起動と一緒に。P1 は p004b の設計 → p005a（HPD の detect と事象）、p004b の code は P2 の ws113-p011a の後。
- 2026-10-07 Q1: ws052-p012 は電源ボタンの短押しで眠らせない（ユーザーの「電源ボタンのハンドリングは、あとで…メニュー…WS182」が N5 の既定の案を置き換え）。sleep button・蓋・無操作は p012 で眠る。Q1 が誤って古い P2 の世代に message を送り resume させた → すぐ TaskStop、worktree は新しい P2 の未 commit だけ（被害なし）。
- 2026-10-07 ユーザー: UAT（ws181-p008）の担当は「後で」、WS184・WS185 は「他のベータ2 WS と同じ」優先度。T1-353: c5-hw は 5330 で login 画面（`Wrong password`）で止まり FAIL（自動 login されない image、harness の問題、未解析）、kernel 側は picture up・stop done・error 0 → ws118-p006 cleared。c5-hw の login の直しは c5-hw を次に使う時に。
- 2026-10-07 ユーザー:「5320のカーネルは変えていいです」→ Q1 が main 173fa2115 の kernel を /esp/vmunix に（前は vmunix.prev、最初は vmunix.orig）、再起動はユーザー。UAT（App Home の遷移を iOS の奥へ・奥から、touchpad の端の 2 本指 swipe は 1 本が端なら、上端の swipe down を App Home へ）→ [ws181-p008](ws181/phase008/phase.md)。機能追加（ベータ2）: 左手デバイスの OSK → [WS184](ws184/ws.md)、ゲームパッドの OSK・ゲームコンソールモード → [WS185](ws185/ws.md)。
- 2026-10-07 ユーザー:「電源ボタンのハンドリングは、あとで実装でいいです。ログオフ、電源オフ、などのメニューを表示できるようにしたいです。独立WSにして、ベータ2の最後に実装しましょう。」→ [WS182](ws182/ws.md)。「5320の/bin/waylandは更新してOKです。」→ Q1 が main 5f6da8bba の compositor を入れた（元は /bin/wayland.orig）。「タッチパッドは独立WSにして、ほかベータ2WSと同じ優先度で後回しにします。」→ [WS183](ws183/ws.md)（P3 は立てない）。
- 2026-10-07 ユーザー:「そのGPU/GOPの設計をdocs/のドキュメントに残しておいてください。」→ docs/architecture/kernel-and-hal.md に「Display path」の節（text console と /dev/graphics は board の対、GPU driver は通知も console の引き継ぎもしない、表示は lease の claim で初めて引き継ぐ、その後 console は firmware の framebuffer のメモリに描き画面には出ない）。未決（ユーザーに WS を立てるか確認中）: GPU が表示している間・手放した後の text console（console の login・panic の表示）。`drv_i915_n1_mirror_console()` は呼ばれていない。
- 2026-10-07 ユーザー:「タッチパッドが動いたらP3は畳んでいいです。」→ ws118-p007 の後、P3 は次の Queue を入れずに終える。
- 2026-10-07 ユーザー（クリック）: N8 は「ベータ2 に入れる」（WS113 p003・p004 を ws052-p012 の前に、WS113 p004 を 1 出力の切り替え（先）と複数の同時の出力に分ける）、蓋を開けた時は「内蔵の画面に戻す」（拡張表示の後は拡張へ）、N3〜N7・N9 は「既定の案のままでよい」。P2 の順: ws052-p011 → WS113 p003 → p004 の 1 出力の切り替え → ws052-p012 → p013。
- 2026-10-07 ユーザー（ws052-p007 第 4 版の N、クリック）: N1「A1: UAPI に flag を足す」（system_power_info の reserved[0] を flags、KERN_SYSTEM_POWER_FLAG_CAN_SLEEP）、N2「利用者なら誰でも」、N8「外部画面のみに切り替えて通常の利用を継続する」（蓋を閉じても外部の monitor があれば眠らず外部だけで続ける。compositor の複数出力が要る）、N10「作らない」。N3〜N7・N9 は既定の案でユーザーに確認中。
- 2026-10-07 Q1: P2 の ws052-p007 設計 第 4 版（e97fb018b）。人の判断 N1〜N10 はユーザーへ。docs/architecture/power-management.md の 52-54 行・91 行が今の code と違う（P2 の指摘、N の決定の後に直す）。p010 を先に着手。
- 2026-10-07 ユーザー:「UCSIとDP alt modeってもう動いてるんですか？シェーダコンパイラより優先してほしいです」→ クリック「P1 を移す」（P1 は ws075-p007a a3 を安全な地点で止めて WS051 p003 から、P2 は WS052 を続ける）、M-3「5330 で採取する」（`/tmp/i915-hw.lock` の下で iGPU を一時的に host の i915 へ（`plan/ws031/tests/host/igpu-mode.sh host`）、USB-C の DP の monitor をユーザーが挿し、debugfs・intel_reg で採取、vfio に戻す。design §14.5）。WS075 p007a の残り（a3〜）は後に。
- 2026-10-07 ユーザー: 5320（10.0.30.5、zedBSD）で Keiland が立たない → Q1 の調査（TGL の takeover の停止の失敗、DPLL の管理が空）。「サブエージェントP3を立てて、5320のDPLLに対応してほしいです。できれば世代やバリエーションにかかわらず表示できるようにフォールバックも実装してほしいですが、難しければいいです。」→ WS118 のブロックを解き p006・q846 を P3 に。
- 2026-10-07 ユーザーの UAT:「アレンジメントは完璧ですね。整列方法の選択ポップアップは、glassを適用してほしいのと、広がって大きくなるようなアニメーションとともに透明度が高くなる演出で表示してほしいです。」→ ws181-p007（q845、P2、優先）。
- 2026-10-07 ユーザー:「5330を再起動しましたので進めてみてくれますか？」（WS084 の d1・d3 の質問への答え）。5330（chaos、10.0.30.3）は SSH の host key が変わっていた → ユーザー（クリック）「新しい鍵を受け入れてよい」で Q1 が known_hosts を更新（ED25519 SHA256:+Hex0MHFbPMAH2mBJ7CxP/qw0Likn7XvqM832CqpEA4）。保留の T1-284（WS139 E2）・T1-334（ws051-p002 の ktest）を解除。
- 2026-10-07 ユーザー:「また、仮想デスクトップのアイコンは、猫、鳥、ウサギにします。真ん中が鳥です。シルエットのアイコンで、選択されると、アクセントカラーになります。」→ 左 猫・真ん中 鳥・右 ウサギのシルエット、今いる desktop は accent の色（ws181-p006）。
- 2026-10-07 ユーザー:「仮想デスクトップの円は、何番目、という覚え方が得意な人にはそれでいいのですが、それが苦手な人もいる気がしました。かといって、この位置に色のついた円を配置すると、Appleのウィンドウのボタンと似てしまい、役割が違うことで混乱するとも思いました。そこで、真ん中の仮想デスクトップを基本にして左に行くか、右に行くか、という操作にして、3つの仮想デスクトップにしようと思います。」クリック: 表示は「3 つの形を左・中・右に、今いる所を明るく」、起動・login の時は「真ん中」から。→ ws181-p006 の (3) を置き換え（desktop は 4 → 3、真ん中が基本）。
- 2026-10-07 ユーザーの UAT（ws181-p005 の撮影へのコメント、原文）:「アレンジメントの選択のポップアップは、幅を2倍、高さを5倍にします。重要な選択だという印象を持たせるためです。1行目に、水平分割、垂直分割。2行目に、左1列＋右垂直分割、右1列＋左垂直分割、3行目に上1行＋下水平分割、下1行＋上水平分割、4行目にタイル。ホーム画面は、アイコンを画面の下に寄せます。ホーム画面はスワイプで左右にページ送りできます。1枚目には時計を置きます。これは、重心を下にすることで、上から順にアプリを並べただけのメニュー画面という印象を壊し、時計のあるホーム画面、ここが何もしないときの居場所なんだというユーザの安心感を作るためです。仮想デスクトップの円は、右と下が切れて正円になっていないように見えます。また、選択された仮想デスクトップも正円にして、色で区別しましょう。」→ ws181-p006（q844、P2、優先）。整列の形は 7 つに（上 1 行＋下の水平分割、下 1 行＋上の水平分割を追加）。
- 2026-10-07 ユーザーの UAT（WS181 の撮影へのコメント、原文）:「App Home画面のとき、画面上部のドックバーを表示せず、でも右上の通知アイコン領域と時計領域を、背景色による塗りつぶしなしで、白い文字とアイコンで描画してほしいです。クリックされたときの動作は、App Home以外と同じでよいです。アレンジメニューのポップアップですが、アイコンだけにして、テキストは不要です。また、操作はcurrent virtual desktopに対して行われることにして、1,2,3,4のテキストは不要です。ポップアップにglassを適用してください。アレンジメント使用中に、virtual desktopのアイコンに、アレンジメントの選択が表示されていますが、これは不要です。また、仮想デスクトップの選択状態を表すカプセル領域は、半分の横幅でよく、それぞれのデスクトップを表すアイコンは、正円でいいです。」クリック: desktop の切り替えは「pill の tap は常にメニュー、切り替えは swipe・key だけ」。→ ws181-p005（q843、P2、優先）。
- 2026-10-07 ユーザー（クリック、WS083 design.md §10）: H1 UAPI の差分（include/uapi/gpu-op.h に video の 14 opcode 0x10000〜0x1000d、GPU_OP_PROTOCOL_VERSION 2、plan/ws083/proposed/gpu-op-video.diff）を「承認する」。H2・HD6「1.0 のまま、sync2 を翻訳で足す」（規格に合わない点を記録して名乗る）。HD2「reset 無しで進め、既定では出さない」（i915.debug=video の時だけ、engine reset は p007 の後）。H3・H4・H5・HD1・HD3・HD4・HD5「全部このまま」。
- 2026-10-07 ユーザー（クリック、WS181 の第 2 回の review の追加）: N1 docked の窓を閉じて他に窓が無い時も「docked mode を終える」（10-06 の「tablet mode は session の状態」を置き換え、次に開く app は floating）。N2 上端 10 px からの touch の下への drag は docked の title の上でも Wiseview「そうする」。N3 見えない desktop の docked の窓が裏で閉じた時は「何もしない」。
- 2026-10-07 ユーザー（クリック、WS181 design.md §7）: D3 touchpad は「変えない」、D5 整列のメニューは「pill のどこでも」（中に desktop の切り替えも置く）、S6「整列モードを終える」（詰め直さない）、S8 上端の帯は「touch だけ」、D1・D2・D4・D6・D7・S9 は「全部このまま」（既定どおり）。
- 2026-10-07 ユーザー（クリック、WS178）:「完全に分ける」（libGL.so は GL だけ、glX* は xserver の package の libGLX.so だけ、外から移植する X の GL の program は link の修正が要る）。T1 は再起動し、削除は全ての担当で禁止・Q1 が行う（plan/agents/protocol.md 末尾）。
- 2026-10-07 ユーザー（クリック）: WS181 は「ベータ2、UAT として優先」、整列の形は 5 つ（水平に等分、垂直に等分、右に 1 つ・左に縦の分割、左に 1 つ・右に縦の分割、格子）。WS180（Emacs の graphical な editor）と WS117（Qt6 の Linux の互換）は「どちらもベータ3 以降」。WS178 の libGL・libGLX の分割はユーザーが互換性の説明を求めた（Q1 が回答）。
- 2026-10-07 ユーザーの UAT（挙動の調整、4 点: docked から floating の時の他の窓、docking の隠れと最小化の区別、App Home を独立のモードに・gesture の遷移、整列のメニューと整列モード）→ [WS181](ws181/ws.md)。M-3（5330 の iGPU を host の i915 に付け替えて正解値を採る）の質問はユーザーが閉じた（未決、指示を待つ）。
- 2026-10-07 ユーザー（クリック、WS179 design.md §10）: 1 既定の blue は「今のまま、例外として記録」（0x2f7cf6 と白い文字、contrast 3.94）、b compositor の 0x4085fa は accent に「揃える」、c「desktop の icon だけ従う」（greeter は既定の青）、a・d・e は「3 つともこのまま」（red・green はそのまま、yellow は light 0xa48207・dark 0xf5c518、dark の purple・pink・red・graphite の主の button の文字は黒）。
- 2026-10-07 ユーザー: Emacs の拡張（Emacs をベースにしたグラフィカルなエディタ、エージェント開発の次世代のエディタ、`Keiland.*` の NAPI・`emacs -g`・tab のチャット）の WS だけを作るよう依頼 → [WS180](ws180/ws.md)（planning、段・見積もり・Phase は未定、Queue なし）。
- 2026-10-07 ユーザー:「了解です。USB-C, DP Alt mode, i915 videoを含め、スケジューリング優先度を下げていたベータ2のハードウェア関連の実装項目について、優先度を通常にして、作業開始を許可します。」→ 第 3 段の hardware の WS（WS050・WS051・WS083・WS075・WS084・WS052・WS031 の残り）を通常の優先度に。WS083 は p001・p002 に限らず、host で作れる所まで進めてよい（実機の確認は 5330 が戻ってから）。
- 2026-10-07 ユーザー（WS083 の割り当ての質問への回答）:「ほかのアプリ実装が終わってからハードウェア関連の作業に進みたいのですが、もうハードウェア関連の作業も開始しないとP1,P2にスケジューリングできなくて空きが生じる水準ですか？もしそうなら開始していいです。」→ Q1: app の実装は終わり、hardware 以外の残りは WS179（アクセントカラー）と WS178（OpenGL・GLX）だけ（約 4.5 LW）。P1 は WS179 → WS178 を先に、その後 hardware（WS083 は p001 設計・p002 libvulkan の骨組みまで、10-02 の「別セッション」の指示はこの回答で置き換え）。P2 は WS050 を続ける。
- 2026-10-07 ユーザー:「UIのボタンなどのアクセントカラーは、何色かから選べるようにしましょう。現状のUIが非常に完成度が高く、ちょっとよくばりな意見を持ってしまいました。満足している裏返しと思ってください。」クリック: 範囲「ベータ2（今の作業の後）」、色「8 色の固定」→ WS179 を作成。
- 2026-10-07 ユーザー（クリック、WS031 p020・p039）: 「Int16 今、Int64 は後の Phase」（shaderInt16 を今、Int64 の 32 bit の組の模擬は後の Phase、Float64・Float16 は Future Work）。
- 2026-10-07 ユーザー: 「make menuconfigの X11 -> OpenGL and GLX ですが、Desktop -> OpenGL に移動して、GLXは Desktop -> X11 server for the compositorのライブラリの1つに統合しましょう。ベータ2の範囲にして、優先度は低くていいです。」→ WS178 を作成（ベータ2、低い優先度、第 3 段の最後に）。
- 2026-10-07 ユーザー（クリック、WS031 p032）: dual source blend は「実装、実機で確かめるまで 0」（compiler・state は実装、dualSrcBlend・maxFragmentDualSrcAttachments は 5330 で確かめるまで 0）。logic op は有効に。
- 2026-10-07 ユーザー（クリック、WS157）: データベースは「月ごとの TSV＋album ごと」（~/Pictures/Library/db/photos/YYYY-MM.tsv と db/albums/<id>.album）。取り込みは「複写、同じ中身は取込まない」（SHA-256 で重複を判定、同じ名前で中身が違えば 名前-1、縮小画像は ~/.cache で sync しない）。
- 2026-10-07 ユーザー（WS157 写真の要件、P2 の既定案への回答）:「~/Pictures/Library 以下で管理します。取り込み機能ありです。データベースあり、サムネイル管理あり、フォルダ名は img/year/month/day で、ファイル名は維持。アルバムはデータベースのメタデータでリンクを管理。直接ファイルを置くのではなく、取り込みで管理。データベースはクラウドsyncしやすい形式（サイズが小さい、分割されてる）。」→ P2 の既定案（~/Pictures を読むだけ、album は folder、photos.conf）を置き換える。新しい Phase で作り直す（q835）。
- 2026-10-07 ユーザー（クリック、WS120 音楽）: AAC は「今は libavcodec、独自は後」（videoplayer と同じ libavcodec の dlopen の add-in、decoder の口を分け、独自の AAC-LC は後の Phase で表の出典を決める）。既存の service（YouTube Music・Spotify・Apple Music）との連携は「その機能はベータ4へ。今は外部サービスのアイコンは権利の関係でいらないです。」→ Services の欄・外部の service の icon は作らない。
- 2026-10-07 ユーザー（クリック、WS112）: D-a build の環境は「mmdebstrap の rootfs」（target ごとの固定の rootfs の中で native に build、arm64・RPi は host の qemu-aarch64 の binfmt、VM なし。2026-10-02 の QEMU の guest で build の指示を WS112 では置き換える）。D-b 確認は「生成＋形式・依存の解決」（apt-get install --simulate と dpkg-deb・ELF・manifest、導入・GUI の起動はしない）。
- 2026-10-07 ユーザー（make menuconfig）: emacs は base（packages/editors の分類をやめた）。desktop の下の試験の program は tree ごと tests へ（userland/desktop/ime-probe → userland/tests/ime-probe、Desktop の menu にあった venus-frame も userland/gpu/venus → userland/tests/venus-frame）。zedinst は削除（userland/retro/zedinst、config の一覧からも）。X11（Xzed・zterm・zshell・zwm・zgears・glxtest・libGL・libX11・session）は userland/retro → userland/x11、menuconfig の Desktop と同じ階層の X11 の submenu へ。Packages に Multimedia の分類を足した（libavcodec が menu から漏れていた）。
- 2026-10-06 夜 ユーザー: 「トップレベルのmake downloadは、ソースツリーにすべての依存ファイルをフェッチして、完全なソースツリーを作成し、フリーズして安全なメディアに保存する目的のコマンドです。CIで実行するようなものではありません。そして、CIも含めて、ビルドは、トップレベルのmake downloadは絶対に使わず、コンフィグで指定されたパッケージだけを、個別にmakeするときに、対象のMakefileがダウンロードを行います。」→ ci.yml・release.yml から `make download` の step を外した。docs/howto/build-from-source.md も直した。
- 2026-10-06 夜 ユーザー（クリック、WS169）: メーラの password は「0600 の file に平文で仮置き」（~/.config/keiland/mailer-accounts、秘密の store ができたら替える）。Gmail・Outlook の OAuth2（p006）は「今は IMAP/SMTP だけ」でベータ2 から外す。
- 2026-10-06 夜 Q1: 第 1 段の 1 パスの間は、各 WS の全文規約の見直しの Phase（conformance）を後に回す（全ての実装の 1 パスの後、WS177 と一緒にまとめて）。正常系の実装を先に。
- 2026-10-06 夜 ユーザー（記録だけ）: desktop の mode はスマホモード（単一の app を最大化だけ、複数の窓は出さない、約 6 inch まで、Dock Bar は狭く menu を出さない）とタブレットモード（今作っている物、最大化と窓の mode を切り替え、最大化で title bar が Dock Bar に dock、約 7 inch 以上の tablet・laptop・desktop、touch 指向で trackpad・mouse・keyboard も可）。通知 compositor → app: background（process を止める）・foreground（再開）・terminate（oom kill・手動の kill）・画面の大きさの変更。app → compositor: 実行できる mode（スマホ・タブレット）、background でも実行の要求（タブレットだけ）。→ 目標の設計として docs/architecture/keiland.md の「Desktop modes」に記録。実装の Queue は作らない。
- 2026-10-06 夜 ユーザー（WS165 の H5）:「いったんacceptして、追加のフェーズを第2段でやりましょう。」→ p002 の認識率でいったん受け入れ、組を分ける改善は新しい Phase（第 2 段）。
- 2026-10-06 夜 ユーザー: 試験の整理の基準（AGENTS.md の検証の節）。一斉の棚卸しはせず、変更に追従する時にだけ適用:「本当に再利用するテストか」を判断し、master の Tools・試験の一覧・未完了の WS・tests/ のシナリオ・T1 の未実行の依頼から参照されない試験は直さず削除（文書の参照も外す、削除は Q1）、回帰に使う物は master に登録、回帰に使いそうな物は tests/ のシナリオへ。
- 2026-10-06 夜 ユーザー（クリック）: WS112 の新しいゴールは「3 つ（amd64・arm64・RPi）」の deb（Debian 13・Ubuntu 26.04 共通の amd64・arm64、Raspberry Pi OS の arm64）、段は「第 2 段（ベータ2）」。Fedora・Arch は取りやめ。
- 2026-10-06 夜 ユーザー: WS139（速さ）は最適化なので第 2 段へ。WS085・WS088・WS114 は「すでにできており、Completeにしてください」→ completed（Q1 が ws.md を完了の形にし、Phase の directory を削除、WS114 の試験は plan/tools/gtk4-linux/ へ）。WS112 は Debian の package だけに目標を設定し直す（下の質問の後に記録）。
- 2026-10-06 夜 ユーザー: 不確実性のある hardware 関連は第 3 段へ（WS083 を第 3 段に）、WS130 IPv6 の残りは第 2 段へ。WS115 GTK4・WS116 Qt6・WS126 Python はベータ3 へ（理由: OS の価値は tablet と desktop の融合した UI/UX、既存の toolkit は core competence でない、script 言語は Noct がある）。
- 2026-10-06 夜 ユーザー（クリック）: WS172 の QEMU での鍵の試験は「(c) 実機の鍵だけで確かめる」（P5 の kernel の応答器は作らない、実機の YubiKey の UAT で確かめる）。
- 2026-10-06 夜 ユーザー（クリック）: Guardrail の OpenSSL の例外を「広げる」→ libpasskey を使う base の道具 fidoctl（WS161）も範囲に（期限は同じ、リリースの前に独自実装へ）。
- 2026-10-06 夜 ユーザー:「WS152はベータ3に先送りします。」→ WS152（system の更新）は第 1 段から外しベータ3 へ。p001 の検討（U1〜U12）は保存。
- 2026-10-06 夜 ユーザー（クリック）: Settings の Privacy（WS148）・Security（WS149）・Accessibility（WS151）は 3 つとも「頁を無くす」。Privacy は推奨どおり Files の Recents に「履歴を消す」と Storage に「最近の項目を残す」の switch。Security・Accessibility は頁を取り除くだけ（lock の設定・動きを減らす などは作らない）。q824（P2）。WS152 は第 1 段（ユーザーの列）なので段は「ベータ2」に揃える。
- 2026-10-06 夜 ユーザー（クリック）: ws089-p013（About の memory と Storage の使用量）を「採る（第 1 段で）」。Q1 が libkeiland の system の照会の API の追加（plan/ws089/proposed/libkeiland-system.md）を許可。Q1 の割り当て: WS152 は第 1 段に含む（ユーザーの列に在る）、WS161・WS172 は P2、WS164 の compositor の起動と WS156 p002 は p022 の merge の後に P2。
- 2026-10-06 夜 ユーザー: 積み残しは「専用の1つのベータ2積み残しというWSに入れてください」（元の WS に置かず WS177 の backlog-p1.md・backlog-p2.md へ）。「ベータ1は難なく前倒しできるので、実際には最初のベータはベータ2で、2026年10月17日に公開するのはベータ2に変更です。」→ fg019 を 10/17 のベータ2 の公開に。版の名前と tag（zedbsd-0.1.0-beta1 → beta2、About の 1.0.0 Beta 1 など）は WS129 で P2 が確かめて直す。
- 2026-10-06 夜 ユーザー:「積み残しWSはベータ2積み残しという形でWSを作りましょう。」→ [WS177 ベータ2 積み残し](ws177/ws.md) を作成。各担当は phase.md の「積み残し」の節に書き、第 1〜3 段の後に Q1 が WS177 の Phase に集める。
- 2026-10-06 夜 ユーザー: 作業の順を第 1 段（ベータ1 の WS ＋ベータ2 の一部）→ 第 2 段（app・Linux）→ 第 3 段（kernel・base）→ 積み残しの WS → デバッグに専念、と決定。正常系だけを実装、準正常系・異常系の未実装は積み残しに（master の priority、q820・q821）。
- 2026-10-06 ユーザー（クリック）: Files・Settings の残りの自前の UI 部品も「置き換える（別の Phase）」→ [ws090-p023](ws090/phase023/phase.md)（q818、P1、q817 の後）。
- 2026-10-06 ユーザー（クリック）: Files・Settings の欄は「今 libkeiland の canvas へ移す」→ ws090-p007（Settings）と p009・p010（Files）を前倒し（q817、P1、WS131 p022 より先）。それまでの間の IME は P1 が自前の欄に入れた（q816 の (a)）。
- 2026-10-06 ユーザー:「Interの削除はお願いします」→ Q1 が Inter.ttf と Inter-OFL.txt を git rm、試験・道具の 61 file の Inter.ttf の参照を Mahora-Regular.ttf に、licenses-index を更新（main f657b82f3）。履歴の証拠の json・md は元のまま。WS074 の Chrome との画素の比較の試験は font が変わったので基準が変わる（WS074 の描画の改善は止めたまま）。
- 2026-10-06 ユーザー: 最大化の時に compositor が中身の rect を四方に 4 px（[ws099-p038](ws099/phase038/phase.md)、スクショを見せる）。**px は既定の DPI での論理 px**（後で DPI scaling を入れる）。テキスト入力のある全ての app の IME の受け付けの確認と、libkeiland の UI 部品でない自前の text box の洗い出し（理由が無ければ libkeiland の部品へ）。Mahora の採用は「うまくいったと思います。見た目がとてもいいです。」
- 2026-10-06 ユーザー（クリック）: WS131 の p021〜p026 を「全部進める」（p021 kwl_ の改名、p022 log の接頭辞 KWL と試験 201 本、p025 browser の shell の窓（WS074 の描画は止めたまま）、p023 互換の除去、p026 公開の header の移動（sysroot の変更は Q1 が toolchain の lock を外して一人で流す）、p024 全文規約と 3 OS の回帰）。q811 で keiland-os-boundary の B2 を D4 の許可の表の拡張（compositor が libkeiland の kl_tr* を使う）で解決 → ユーザーに報告。
- 2026-10-06 ユーザー: 全ての app の窓の中身の padding を 0 に（title bar と同じ幅）、title bar と中身の間の高さは compositor の定数で全 app 同一 → [ws090-p021](ws090/phase021/phase.md)（q813、P1）。画像は build/review/ に複写し project の top からの相対 path で見せる（remote control のため）。
- 2026-10-06 ユーザー: fallback は 2 つ残す（可変ピッチ 1・monospace 1）、遠い将来に fallback 無しでも動くように。クリック「Droid Sans Fallback と JetBrains Mono」→ Inter は使わない。ws090-p020 に記録。
- 2026-10-06 ユーザー: Mahora の font（ユーザーの著作、tree の Zlib）を追加、UI を Mahora Regular・Terminal を Mahora Mono・太字を Mahora Bold に。外観が良ければ他の font を消す → [ws090-p020](ws090/phase020/phase.md)（q812、P1）。Q1 の確認: Mahora は ASCII の 95 字だけ。
- 2026-10-06 ユーザー（クリック）: rm の規則の範囲は「make の規則の rm は可」（自分の worktree の build/ の中の make の出力）。AGENTS.md と protocol に記録。P1・P2 が気づかずに走らせた rm を含む host の script 15 本は Q1 が rm 無しに直した（fresh-out.sh・build/tmp・q1-clean.sh）。
- 2026-10-06 ユーザー（クリック）: WS168 の sandbox の isatty は「(a) TCGETS だけ ENOTTY」（承認済みの許可の set の変更、ws168-p002 に記録）。
- 2026-10-06 ユーザー（クリック）: WS131 の app の移行 p016〜p020 を「承認、p016 から順に」（kl_app へ、Q1 の最初の質問の書き間違いを訂正した上で）。ws095-p007 の Notes は「Notes に text box を作る（別の Phase）」→ [ws079-p017](ws079/phase017/phase.md)。Q1 の許可: WS175 p006 の `userland/base/libz-compat` への deflate の追加（design の「libz-compat の path は Q1 の許可」）。
- 2026-10-06 ユーザー:「慣性スクロールはlibkeilandに実装してほしいのですが、Settingsも含め、各appで独自実装してしまっていませんか？」→ Q1 が確認（Settings の減速は自前、Files・Terminal・Browser・PDF Viewer・Image Viewer・Notes・Text Editor は全て自前）。[ws090-p019](ws090/phase019/phase.md) で libkeiland に一本化（q806、P1）。
- 2026-10-06 ユーザー（クリック）: BUG-223 の game mode は「B backend に direct の口」（Guardrail の例外の表に記録）。ws128-p004（PDF の文字の検索と選択）は「採る（ベータ2）」。WS131 p016〜p025 の質問に「appはlibkeilandしか使わず、これはコンポジタのクライアント側です。libkeiland-backendはコンポジタ実装のファウンデーションのバックエンドです。何か混同してませんか？」→ Q1 の質問の書き方の誤り（p016〜p020 は app を libkeiland の新しい API `kl_app` へ移す Phase で、libkeiland-backend とは無関係）。訂正して聞き直す。
- 2026-10-06 ユーザー（クリック）: P1 の Files の host の画素の一致の確かめ（安全の判定に止められた）は「この確かめはやめる」。q801 は canceled。
- 2026-10-06 ユーザー（クリック）: BUG-214 は「slider と switch に分ける」（slider は窓の中身の不透明度 85〜100・既定 100、「Frosted glass」の switch は既定 on・off で不透明の地）。BUG-220 の列の幅は「保存する」（Files の設定に）。BUG-223:「フルスクリーンモードではappのbufferをscanoutしているはずです。確認して教えてください。」→ Q1 が確認（ws099-p015 が 2026-09-30 のユーザーの指示で fullscreen mode の直の scanout を削除、display.c の注記、code は GitHub の old）→ ユーザーの選択「動画・game mode だけ戻す」: app が明示に頼む全画面（game mode）の時だけ直の scanout を戻す、普通の全画面は合成のまま・端の swipe を保つ。
- 2026-10-06 ユーザー: light の bar は montage-5 の「このまま実装」。app の icon の中抜きは「デスクトップ背景が透けて見えるとうれしいです。ライトもダークも、Apps一覧も。」（bar の light・dark と App Home の Apps の一覧で、記号の部分から壁紙が透ける）。
- 2026-10-06 ユーザー（クリック）: session からの Power Off・Restart は「wheel だけに限る」（root と wheel、他の利用者の有無に関わらず）。ws131-p027 の新しい attempt（q793-i02）。
- 2026-10-06 ユーザー（クリック）: BUG-227 の残り（challenge.js に要る typed array・crypto・Worker/Blob・fetch の POST・canvas）は「ベータ2 の後に回す」。header の直しで止める。
- 2026-10-06 ユーザー:「モンタージュの画面上部のバーですが、ダークモードではこの黒い色でOKです。ライトモードでは、ウィンドウタイトルバーと同じ色味にしてほしいです。」→ 上部の bar は dark では黒い glass のまま、light では窓の title bar と同じ色味（前の「light でも暗い bar」を置き換え）。ws099-p034b（q794）。
- 2026-10-06 ユーザー（クリックの回答）: BUG-227「ChromeのUAでデバッグを続けます。」（UA は Chrome のまま、Amazon の WAF の challenge を通す方向で調べを続ける）。App Home の stage は **案 A（弱い）**、light の外観でも暗い stage。ws099-p037 の Power Off・Restart は「sessiond に口を足す（別の Phase）」→ [ws131-p027](ws131/phase027/phase.md)。
- 2026-10-06 ユーザー（クリックの回答）: 素早い Alt+Tab の残った UI は既定のまま（Tab・Shift+Tab・矢印で選ぶ、Enter・click・tap で切り替え、Esc・外の click で閉じる）。bar の参考の画像 bar-1〜4 は tree に残す。WS175 の D1〜D7 は全部 phase001 の表の推奨どおり（ws175-p001 を cleared にでき、WS175 はベータ2 の実装の列へ）。
- 2026-10-06 ユーザー（クリックと文の回答）: (1) ws142 の dock の (b):「最大化の状態でほかの窓を閉じる操作はできないです。窓が自分から閉じることはあります。最大化状態で今の窓を閉じたときは、次の窓は最大化状態にします。最大化はウィンドウの状態というより、デスクトップ環境がタブレットモードであるという解釈をします。」(2) BUG-214: slider の初期の位置を磨りガラスの実際の値に合わせる（既定の見た目は変えない）。(3) BUG-222: ue0 は RTL8156 の USB LAN。(4) ws099-p019:「その表現の認識が違うだけで、緑色の抽象的な背景はすでに入っていましたよ。」→ 抽象版の探索は終了（既存の緑の抽象の壁紙がそれ）。ws049-p017 ⑤ は 2026-10-05 に不要として閉じていた（open-decisions から削除）。
- 2026-10-06 ユーザー（クリックの回答）: BUG-209 の Alt+Tab の 4 つの仮定を全て推奨どおりに決定（端で反対の端に回る、Shift+Alt+Tab は 1 つ左で最初は今の app、3 本指の tap の切り替えも今の app から、短い Alt+Tab は今の app のまま）。今の実装と同じ。BUG-212 の構成: 有線は 10.0.0.1 の router に直結、WiFi は 10.0.30.1 の WiFi router（NAT）経由、WiFi は AX211。
- 2026-10-06 ユーザー: q780〜q784 を承認、N=2（P1・P2・T1）。T1 の model を Sonnet 5.5 medium に。ゴールはベータ2 までの範囲の全消化。順は UAT の指摘 → ベータ2 の未実装 → UAT 以外の Bug。T1 の結果は待たず、試験中の Phase に試験待ちの印（[protocol](agents/protocol.md) の 2026-10-06）。
- **2026-10-05 夜 ラップアップ（ユーザーの指示で P1・P2・T1 を終了）**: 全部の担当が終了。main 0041dbfd 以降。T1-203（WS172 p002 の PIN の login）が **FAIL**: greeter の password の login が起きない。WS172 p002 は main に merge 済みなので、**次の作業の最初に graphical login の image が壊れていないかを確かめる**（P1 の解析）。未 merge: agent/p1 の WS130 p002（67801dab、T1-206 待ち）と WS168 p002 の kernel（730e8f55、arm64・sparcv9・x68k の build の確認も）。未実施の T1: T1-206・T1-202・T1-205・T1-207。5330 の AAT の image は T1-202 と直しの後に作り直す。WS168 の libc の `<sandbox.h>` の sysroot への追加は Q1 が許可する（toolchain の lock）。

- **2026-10-05 夜 ユーザーの決定（まとめての質問への回答）**: 次を**担当の推奨どおり**に決定。
  - WS156 通知: H1 popup の大きさは案、H2 log は Super+N、H3 memory で 100 件、H4 全画面・lock 中は log だけ（URGENT は全画面にも）、H5 重なりは 1.5 秒に縮める、H7 bar の媒体の icon を通知に置き換える。
  - WS164 Welcome: H1 各 account の最初の login だけ、H2 Settings の mode で終わりに Today、H3 5 段。
  - WS165 手書き: H1 段 1 の目標、H2 $P 型の点群の照合、H3 Hershey と自作（KanjiVG は段 2）、H4 段 2・3 は段 1 の後。
  - WS167 GPU の命令: H1 Google の著作権の表示を外す、H2 include/uapi/gpu-op.h を足す（UAPI の追加の承認）、H3 名前は GPU_OP_CREATE_INSTANCE の形。
  - WS158 翻訳: ① 英語の文、② 独自の UTF-8 の catalog、③ 英語と日本語、④ greeter は system の既定、⑤ ws089-p015・ws127-p005 を WS158 に吸収（元は canceled）。
  - WS154: SKK の >・/・#・Tab・注釈を Future Work へ。
  - WS145 印刷: D2 利用者ごと、D3 path・queue 名を詳しい設定で入力可、D5 CUPS の printer は出さない、D6 PDF Viewer の印刷を含める、D7 login 名を送る、D8 spool の上限は案、D9 同じ利用者の app に job が見える。D4（受け入れの printer の機種）は未決。
  - BUG-194: 全画面から戻る key は **F11 と Super+↓ の両方**。
  - WS169・WS170: **ベータ2**（WS170 は連絡先からタイムライン、WS169 は IMAP・SMTP まで。Gmail・Outlook と本物の SMS・通話は以降）。
  - 段ごとの見積もりの表を**今作り直す**（Q1）。
  - WS168 sandbox の縮小表示: H1〜H7 は**全部推奨どおり**（`sandbox_spawn` の system call と include/uapi/sandbox.h の UAPI の追加を承認、子は呼び出し側の uid、断った call は SIGKILL、対象は Files と Settings、Linux・FreeBSD は seccomp・Capsicum だけでよい、子は静的 link、name space を持たせない）。p002 から実装してよい。
  - WS066: 受け入れの文を案 ①〜③ に改める（同じ回の中の静的との差を半分以下に、cc t.c は p003 へ、以後の速さは同じ回の中の比べで書く）。
  - WS145 D4: 受け入れのプリンタは **Brother MFC-L3770CDW**（IPP Everywhere・AirPrint 対応の機種。PDF を直接受けるかは p002 で確かめ、受けなければ PWG raster の変換を足す）。
  - WS153 U2〜U15: **ユーザーが検討中**（質問しない、決まるまで WS153 の p002 以降は止める）。
  - WS130 p002 の UAPI の差分（plan/ws130/phase002/uapi.diff、493fb758、p001 の表からの 3 点の変更を含む）を**承認**。P1 が適用する。
  - WS162・WS163 は **WS172 に吸収**（WS163 は WS172 p002 で達成として完了の形、WS162 の未着手の Phase は canceled で WS172 p003 へ）。
  - WS101 p017・p012 の 5330 の測定は **T1 が 5330 の passthrough で**（合間、lock の下）。UAT・AAT には入れない。
  - WS171 は**ベータ2**（合間の仕事）。
  - 5330 の最初の AAT: **T1-202（QEMU の smoke・full）の後に、runner と補助を直してから main の最新で image を作り直す**（build/aat-0505a は使わない）。作れたらユーザーに USB の起動を頼む。

- **2026-10-05 夕 ユーザーの決定（続き）**:
  - WS121: 目標は「Vulkan Video の hardware decode」のまま（「hardware だけ」）。software decode の <video> の設計（P2 の第 2 版 3df53896）は記録として残し、実装は WS083 の Vulkan Video の後。WS121 は WS083 を待つ。
  - WS130 IPv6: H1〜H8 は**全部推奨どおり**（UAPI の形の承認、V6ONLY の既定 0、既定 on、RFC 7217、DNS の順、断片の再組立て無し、DUID-UUID、Wi-Fi ごとの設定は後）。P1 が p002 へ。
  - WS143 Bluetooth: §9 の D1〜D18 は**全部推奨どおり**（音と PAN は後回し、HID が先）。
  - ws074-p178: 手順に「自前の browser で描いた Acid3 の参照を Chrome の描画と比べ、参照の側と test の側の誤りを分ける」段を足す。
  - INPUT_INJECT_KIND_MOUSE の絶対の pointer の形は、承認したマウスの注入（試験だけ・root だけ）の範囲として Q1 が扱う（ユーザーに報告）。

- **2026-10-05 夕 ユーザーの決定（AAT の後の質問への回答）**:
  - AAT の素の起動: **毎回ユーザーが起動**（USB を差して起動、エージェントは起動の後に SSH で入る）。BootNext・内蔵 disk の案は採らない。
  - BUG-171: **B**（利用者が明示に 100 を選んだ時だけ app の panel を不透明に、既定は今の frosted のまま、title bar は対象外）。
  - WS172 の P1〜P10: **全部推奨どおり**（libpasskey を base・OpenSSL の例外を passkey-fido2 と libpasskey に広げる（Guardrail の行を直す）、再起動の後は一度 login するまで PIN を出さない、key の PIN 必須、`_passkey`、署名する loopback、WS163 の mock を外す、reset で PIN と key も消す、HIDRAW_GRAB を前提に、登録は key 1 つの時、autologin は数えない）。
  - WS161: **V1 は HIDRAW_GRAB（UAPI の追加）、V2（YubiKey の OTP）はそのまま**。

- **2026-10-05 夕 ユーザーの決定（passkey）**: /sbin/passkey と /etc/passkey の形（[WS172](ws172/ws.md)）。passkey は base、暗号は当面 OpenSSL でリリースまでに独自の実装へ（Guardrail の例外）。PIN の失敗の回数は sessiond の memory。WS162・WS163 の ~/.config の mock はこれで置き換える（WS162 の K1〜K3、WS163 の G1 は不要に）。
- **WS162 の判断（2026-10-05、P1 の mock の設計 第 2 版 1845e3fc、§9.7。ユーザーが /etc/passkey と /sbin/passkey の形を考え中で、その形になれば置き換わる）**: K1 greeter で key の login: (a) mock は lock だけ、(b) 登録の時に利用者の password を key の CTAP2 hmac-secret で AES-256-GCM に包み greeter が読める file（~ を 0711 に）[P1 の推奨]、(c) sessiond の request（BSD Auth・passkey の形の後）。K2 key と key の PIN（UV）[可]、K3 kl_system_account_v1 に add_passkey・remove_passkey（manager version 11）。
- **WS161 の決定（2026-10-05 夕 ユーザー）**: U1「UAPI の include/uapi/hidraw.h を承認します。」U2「UAPI の include/uapi/ccid.h と、ノードの名前を承認します。名前は `/dev/smartcardN` がいいです。」U3「sessiond の座席のデバイスの一覧に `/dev/input/hidraw*` と `/dev/ccid*` を足し、ログイン中の利用者に渡してよいです。」（node の名前は U2 の `/dev/smartcard*` に合わせる）U4「試験用の kernel だけに、模擬の装置を入れてよいです。」U5「…それでもベータ2 に入れます。」→ P1 が p002 以降（hidraw・usb-ccid の /dev/smartcardN・seat の一覧・loopback）を実装してよい。
- **WS163 G1（2026-10-05、P1 の設計の改訂 4cd87d18、§9）**: greeter は `_greeter` で動き利用者の ~/.config（home は 0700）を読めず、login は sessiond の AUTH（password）でしか始まらない。sessiond を変えずに greeter で PIN の login をするには、PIN で暗号化した password を `_greeter` が読める所に置くしかなく、どの利用者でも 10^6 通りを試して本当の password を取り出せる。(a) mock は lock の解除だけ、greeter は後の鍵管理・PAM の設計まで password [推奨]、(b) PIN で包んだ password を置きその危険を受け入れる、(c) sessiond に request を足す（ユーザーの方針に反する）。
- **2026-10-05 夕 ユーザーの変更（WS089 p017）**: 「ライトモードとダークモードだけでいいです。アクセントカラーの変更は不要です。」→ accent の選択はやめ、light と dark の切り替えだけ（既定 light、今の見た目）。P2 の段は p017a（key `appearance.dark` と theme の口、Settings の switch、libkeiland・compositor・Settings・Files の dark）→ p017b（残りの app の dark）に組み直す。 追加（ユーザー）「アプリにテーマ変更による再描画を通知するためのWayland拡張がほしいですね。libkeilandを通じて利用します。」→ compositor が theme（light・dark）の変化を app に知らせる Wayland の拡張（Keiland の内部の拡張）を足し、libkeiland の口（変化の callback と今の theme の取得）から使う。app は直接 settings を見張らない。
- **2026-10-05 夕 ユーザーの決定（YubiKey の USB）**: 「OTPはとりあえず非対応でいいです。USB HIDでFIDOが使えるように、raw reportのインタフェースを足しましょう。推奨の方法でOKです。ただ、/dev/input/hidrawXがいいです。/dev/fidoNはなくなった認識でいいですよね？」→ 案 A: 標準の usb-hid に生の report の口（入力の装置にしない HID の interface、まず FIDO の用途 page 0xF1D0、interrupt OUT と出力の report）を足し、node は **`/dev/input/hidrawX`**。**`/dev/fidoN` は無くす**（WS161 の H1 の `include/uapi/fido.h` も不要、hidraw の UAPI に替える）。OTP（YubiKey の keyboard の interface）は当面非対応。CCID（PIV・OpenPGP）は NFC の reader と共通の CCID の driver。libpasskey は hidraw と NFC の APDU の上に共通の FIDO2。
- **WS153 U15（2026-10-05、P2 の設計 第 5 版、U1 = system 全体の反映）**: system 全体の導入（`/apps/<abi>/`）は特権の helper `app-admin`（setuid root、account-admin と同じ形、管理者の password を毎回、署名・hash・展開を自分で検め直す）が書く。root の口なので承認が要る（p005a の前）[案: account-admin と同じ形で可]。U2〜U14 は未決。
- **2026-10-05 夕 ユーザーの決定（WS153・libpasskey）**:
  - WS153 U1: 「アプリはシステム全体で入れましょう。単独ユーザが使うタブレットを想定しているからです。また、ユーザ単位のアプリ管理は、ユーザがホームディレクトリで自由にやればいいと思います。」→ app はシステム全体に導入。利用者ごとの app の管理は repository の仕組みでは扱わない（利用者が home で自由に）。U2〜U14 は未決（P2 の設計 第 4.1 版 §0）。
  - libpasskey: 「NFC汎用のデバイスドライバを作るとして、USBと共通のFIDO2層もlibpasskeyで問題ないですか？また、暗号はOpenSSLの呼び出しでいったん作れますか？」→ Q1 の答え: 可。kernel は NFC の汎用の driver（USB CCID の reader から ISO-DEP の APDU の交換を出す）と USB の FIDO の HID の node だけ、libpasskey が transport（CTAPHID・NFC の APDU）の下の層と共通の FIDO2（CTAP2・CBOR・PIN）を持つ。暗号はまず OpenSSL（package の libcrypto、3.5.8）を呼ぶ形で作る。libpasskey は Keiland の側（package の境界）に置くので base の全自前の方針（master-design-policy §2.1）とは衝突しない。自前の暗号は後の候補。
- **2026-10-05 夕 ユーザーの決定（FIDO2 と kernel の大きさ）**:
  - 「FIDO2周りは、libfido2, libcborも含めて、独自のライブラリ libpasskey にまとめて、独自に作ります。NFCはドライバを作ります。」→ 外部の libfido2・libcbor は使わない。userland の独自の library **libpasskey** に CTAP（CTAPHID）・CTAP2・CBOR・PIN の protocol・暗号を置く。NFC は kernel の driver（ACR1252U の USB CCID）。WS161・WS162 の設計を libpasskey を前提に直す（P1）。
  - 「カーネルの16MBの制限は、15-16MBホールを意識したもので、でもabove 16MBにロードして回避して、もう制限にしておく必要はないはずですね。いつでも制限を外してよいことを記録してください。」→ AMD64_KERNEL_MAX_BYTES（16 MiB）は**いつでも外して（広げて）よい**。外す時は `bootloader/include/amd64-kernel-image.h`・`platform/amd64/vmunix.ld` の ASSERT・`src/hal/amd64/space.c` の `system_kernel_pt`（W^X の leaf の表、1 枚 2 MiB）の数・`image.c`・`handoff-validation.c` を揃え、UEFI と BIOS の boot を確かめる（[BUG-198](bugs/BUG-198.md)）。
- **2026-10-05 夕 ユーザーの決定（仮眠の後）**:
  - WS163 PIN: 「~/.configの中にPINを保存してOKです。sessiondに難しい制御をさせたくないです。移植ができなくなるからです。これはまずモックアップとしての実装で、あとで鍵管理やPAMのような仕組みをきちんと考えます。」→ sessiond に PIN の口を足さない。PIN は利用者の ~/.config に、mock として実装（P1）。
  - WS162 FIDO2 の login: 「sessiondに制御を入れず、libfido2をコンポジタのgreeterが直接叩くモックアップを作ってください。設定は~/.configの中でOKです。あとで鍵管理やPAMのような仕組みをきちんと考えます。」→ greeter が libfido2 を直接使う mock、設定は ~/.config（P1、WS161 の device の後）。
  - WS161: 「Windowsで標準ドライバで使えるACR1252Uをターゲットにします。」→ 対象を ACR1252U（USB の CCID の NFC の reader、Windows の標準の CCID の driver で動く）に。libfido2・libcbor を自前にした時の工数はユーザーの問い（Q1 が答えた、下）。
  - WS143: 「HIDが先で、オーディオとPANもほしいですが、ほかの開発項目より後回しでいいです。」→ 最初は HID。audio（A2DP）と PAN も要るが他の開発より後。D2・D3 の UAPI など §9 の残りは未決。
  - WS156: 「Linuxではlibkeiland-backendにD-bus機能を入れて通知を取ればいいですね。実装はあと回しでいいです。設計だけ記録してください。」→ Linux は libkeiland-backend に D-Bus の org.freedesktop.Notifications の受け口を入れる設計を記録、実装は後。
  - WS166: 「予測変換はインラインの通常IMEではなく、オンスクリーンキーボードのことでした。…もし作ってくれてしまったなら、通常IMEでは、オプションで有効にできるようにしましょう。」→ 予測は screen keyboard の候補の列が本題。IME の予測（ja-predict.c）は option（既定 off）。
  - WS089 p017: 「p017はベータ2に入れます。ただし、従来の調整を壊さないようなデフォルト値で開始できるようにします。」→ ベータ2。既定値は今の見た目（blue、dark off）。
  - WS122: 「GPUデコードはあとで、libavcodecをdlopenしてソフトウェアデコードにする仕様で、まず完成させます。GPUデコードが別WSで完成したら、それをVulkan Video Extensionで利用します。」→ p004 は dlopen の libavcodec の software decode で完成させる。
  - BUG-171: 「保留を解除します。」→ 窓の不透明度 100% で app の glass の panel も不透明に（P2）。
  - WS068 OpenGL 3.3 以降・WS101 GPU compute: 「保留を解除します。ただし優先順位が低く、やることがないときに作業します。」→ 保留を解除、合間の仕事（i915 の lowering・規約の見直しと同じ扱い）。
  - bar の USB の icon は三叉（実装済み 662d4778）。

- **bar のデバイス（USB 媒体）の icon は C（USB の三叉の記号）に決定**（2026-10-05 夕 ユーザー「アイコンはUSB の三叉の記号がいいです。」）。P2 が media.c に実装（q763）。
- **BUG-194 全画面から戻す compositor の key（2026-10-05、P2 の案、今は仮に F11）**: A F11、B Super+↓（Fn が要らない）、C Esc の 1 秒の長押し、D 画面の上端からの swipe [P2 の案: A+B か B+C]。実機で F11 が効かなかったのは 5330 の上の列が既定で Home/End（Fn+F11 か Fn Lock）の見込み、次の UAT で Fn+F11 を試す。
- **WS145 印刷の判断（2026-10-05、P2 の設計 第 3.1 版、3 回目の敵対的レビューで重大なし、plan/ws145/design.md）**: D2 printer の設定を利用者ごとか system 全体で共有か [利用者ごと。共有なら root の口が要り別の設計、p003 の前に要る]、D3 IPP の path と LPD の queue 名を詳しい設定で入力できるか（既定は /ipp/print → /ipp → /、LPD は lp）、D4 受け入れの printer の機種（PDF を受けない機種なら PDF → PWG raster の filter の WS が先に要る）、D5 Linux・FreeBSD で CUPS の既存の printer を一覧に出すか、D6 PDF Viewer の File > Print を含めるか、D7 printer に login 名を送ってよいか、D8 spool の上限（1 文書 256 MiB・合計 512 MiB・16 job）と複写せず app の file から直接送る案、D9 同じ利用者の全ての app に他の app の job の題名が見え取り消せること。
- **2026-10-05 午後 UAT の後、ユーザー「ちょっと仮眠します。その方針でよいので、自走をお願いします。」**: 方針どおり自走。P1 は BUG-195・196・197（実機の ACPI）、P2 は UAT の所見（ws132-p009・ws099-p033・BUG-194（仮に F11）・BUG-181・BUG-193・bar の device の icon）。直しが揃ったら新しい UAT の image を作り T1 で boot-test。判断が要る点は記録して先へ。
- **WS158 の判断（2026-10-05、P2 の p001 の設計、plan/ws158/phase001/phase.md）**: ① key は英語の文（gettext と同じ）か ID か [英語の文]、② catalog は独自の UTF-8 の text（Zlib）か gettext の .po 互換か [独自]、③ ベータ2 の言語は英語と日本語 [可]、④ greeter の言語は system の既定、wheel が Settings で変える [可]、⑤ ws089-p015（Settings の日本語の UI）と ws127-p005（Files の日本語の UI）を WS158 に吸収し元の Phase を canceled（吸収）[可]。
- **WS165 の判断（2026-10-05、P1 の p001 の設計、plan/ws165/phase001/phase.md §7）**: H1 段 1 の目標（1 文字ずつ約 250 字: 数字・英字・ひらがな・カタカナ・記号、筆順・画数を問わない、候補 4、変形した sample で top-1 ≥ 90%・top-4 ≥ 98%・20 ms 以下、利用者の 100 字はユーザーが判定）[可]、H2 方式 [$P 型の点群の照合、学習なし]、H3 template [Hershey の font（license は p002 で監査）＋自前、KanjiVG（CC BY-SA）は段 2 で]、H4 段 2（漢字）・段 3（続け書き・変換）は段 1 の結果の後に計画 [可]。
- **WS169・WS170 の段（2026-10-05 ユーザーの追加）**: ベータ2 に入れるか、それ以降か [Q1 の案: WS170 の最初の範囲（連絡先からタイムライン）はベータ2、WS169 はベータ2 で IMAP・SMTP まで、Gmail・Outlook は以降]。
- **WS164 の判断（2026-10-05、P1 の p001 の設計、plan/ws164/phase001/phase.md §6）**: H1 各 account の最初の login だけ（`welcome.done`、Settings の About から再表示）[可]、H2 新しい app でなく Settings の mode で、終わりに Files の Today を開く [可]、H3 5 段（Welcome・Network・Look・Keys・Done、Back・Next・Skip、Languages は WS154 の後に入れる）[可]。H4（Settings の hook を WS164 が書く）は Q1 が調整として承認（P2 の WS089 の file に触れる時は Q1 が順を決める）。
- **WS154 の判断（2026-10-05、P2 の p001 の設計、plan/ws154/phase001/phase.md）**: SKK の範囲から 接頭・接尾辞（>）・abbrev（/）・数値の変換（#）・補完（Tab）・注釈 を後回し（Future Work）にしてよいか [後回し]。Q1 が技術の裁量で決めた物: 設定の変更で IME を起動し直す（数百 ms IME が無い、protocol は変えない）、SKK の mode を言語の ID（skk・skk-katakana・skk-latin・skk-wide）にする。
- **WS156 の判断（2026-10-05、P1 の p001 の設計、plan/ws156/phase001/phase.md §9）**: H1 popup の大きさ（幅 20%・320〜640 px、高さ 76 px、下中央の 48 px 上）[可]、H2 log の hotkey [Super+N]、H3 log の保存 [memory だけ、100 件]、H4 全画面・lock 中 [log だけ、URGENT は全画面の上にも出す]、H5 重なった時 [待ちがあれば保持を 1.5 秒に縮める]、H6 Linux・FreeBSD の D-Bus の通知 [v1 では無し]、H7 bar の USB media の icon を通知に置き換え [外す]。
- **WS167 の判断（2026-10-05、P1 の p001、ws167-p001 の phase.md §6（git の履歴、2026-10-08 の WS の完了で削除））**: H1 license: 番号の表を独自の名前で書き直し、番号の再利用を明記し、Google の著作権の表示と LICENSE-PROTOCOL を外す（番号は interface の事実で、Venus の文・名前・構造は写していない）[外す]。H2 新しい UAPI include/uapi/gpu-op.h（kernel の i915 の実行器・libvulkan・venus-frame で共有、第 1 版は Venus の番号のまま）[足す]。H3 名前 GPU_OP_CREATE_INSTANCE か GPU_OP_vkCreateInstance か [前者]。
- **WS066 の目標の文（2026-10-05、P2 の案、ws066-p002 の phase.md（git の履歴、2026-10-08 の WS の完了で削除））**: 測定の回の間のばらつきが大きい（変えていない true-static が 517 → 608 µs）。同じ回の中では静的との差が 266 → 10 µs で /bin/true は「差の半分」を満たした。案: ① 受け入れを「/bin/true と sh -c : の静的 link との差を同じ回の中で半分以下に」に改め、sh は静的な sh を足して T1 で確かめる、② cc t.c -o t は完了の条件から外し p003（ld.so の cache、clang 入りの image が作れる時）へ、③ 以後の速さの受け入れは同じ回の中の比べで書く。
- 2026-10-05 ユーザー「本日12時にUATを行います。11時半にマージできている内容で、テストUSBイメージの作成をお願いします。」→ 11:30 に main で build/uat-0505c を作る（Q1、send_later を設定）。「ベータ2の実装をすべて、P1,P2にスケジューリング可能にします。作業を継続してください。」→ ベータ2 の WS（ブロック・アイディアの Phase の物を除く）を P1・P2 の Queue に入れてよい。
- 2026-10-05 ユーザー「今朝追加したWSはベータ2に入れてください。」→ WS161〜WS168 をベータ2 に（見積もり計 20 LW、Q1 の概算）。
- **2026-10-05 ユーザーの指示（段の整理）**: 要検討・ブロック WS098・150・039・037・038・141・080・044・048・118。最初にアイディアの Phase を実行してユーザーと議論 WS144・119・147・082・146・ニューラル IME（WS098、ブロックの一覧にも有るのでアイディアの Phase の後にブロックのままとする扱いを確認中）。WS061 は completed。残りのベータ3・ベータ4 以降の WS（WS001・026・031・046・068・074・101・115・116・117・120・121・130・145・153・157・143・152・126・124・125・112）をベータ2 に移動。
- **2026-10-05 ユーザーの指示**: キャンセル WS034・WS096（Qt6 の互換）・WS097（GTK4 の互換）・WS007。優先 WS138。追加 WS161〜WS167（YubiKey・FIDO2 の login・6 桁の PIN の login・Welcome の画面・手書きの入力・予測変換・GPU の command の protocol の独自化）。新しい WS の段（ベータ2 か以降か）と見積もりは未定。
- **2026-10-05 朝 ユーザー「HALの変更以外は承認します。」**→ 次を案のとおり決定: ws089-p025（sessiond の wheel だけの SERVICE sshd）、ws089-p022（(a) 有線の設定を network の group に開く、(b) ベータ1 の MTU は読むだけ、設定は別の WS）、タッチパッドの既定（100%・中のまま）、BUG-190 の規則（tap は離した時の click、押し込みは押した瞬間、UAT の後に resolved）、ws132-p004（通知の代わりに bar の媒体の icon）、WS160 D1〜D4（最短 8 文字・sudoers 無し・毎回認証・wheel は gid 0）、WS052 p007（外部の monitor の時は蓋で sleep しない（AC に依らず）・無操作 AC 30 分/電池 15 分で画面はその半分・greeter の電源ボタンは sleep・sessiond が session から suspend を受ける（ws131 D12 の改訂、poweroff・reboot は greeter のまま））。HAL の H1〜H4 は専門家のレビュー待ち。BUG-171 は選択肢に案が無いので別に確かめる。
- **HAL v2 は 2026-10-05 ユーザーが承認、hal.h に適用（29f954f5）、実装（WS052 p006）も承認**。記録は Guardrail の承認済みの表。
- 2026-10-05 未明 user「では、私は寝ます。自律駆動で自走をお願いします。」→ 10 時ごろまで自律。新規の実装を優先（P1 = WS160 → q722、P2 = T1-102 の IME の FAIL → WS122）、尽きるか止まったら Bug の一覧から。判断が要る点は uncleared にして記録し次へ。
- 2026-10-05 未明 決定: WS122 の libavcodec をベータ1 の image に入れ、簡単な player もベータ1（RC 10/13）に（段をベータ1 へ。見積もりの表の段の改訂は Q1）。
- 2026-10-04 17 時に決定: BUG-166 のタッチパッドは「押し込み＋タップドラッグ」、5330 の ACPI の table の読み取り専用の取り出しと commit を許可（queue.md の決定 (1)(2)）。
- WS138 の U4（PNG と JPEG だけ）・U7（reset は thread の道）・U8（黒で合成）に合わせて p001・p002 を直す（17 時以降の担当の最初の作業）。WS140 は U3（他の固定の上限も入れる）に合わせて p001・p002 を直す。
- 5330 の host の設定: 2026-10-04 user が iwlwifi の blacklist と再起動・設定の変更を許可（P4 が実施）。
<!-- master:decisions-log:end -->

## 体制・予定の経緯（新しい順）

先頭の「現在の状況」から外した古い体制・予定・指示。指示としては読まず、経緯として読む。

<!-- master:history-log:start -->
- 2026-10-05 夜まで先頭にあった「担当」の記録（2026-10-03〜10-04 の体制）:
  - **2026-10-05 未明〜10 時ごろ: 夜の自律**（user）。P1 = WS159（native の touchpad）、P2 = Files の新規（p011・p010）と p021 の 1 分の有効期限、T1 = 試験の依頼がある時。新規の実装が尽きるか止まった時だけ実機の要らない Bug を投入してよい。判断が要る点は uncleared にして記録し、次へ。
  - **2026-10-04 19時40分 全サブエージェント終了**（セッションの利用枠 94%）。動いている担当は無い。再開の候補: T1 で T1-094（ws141-p003 の N0 の QEMU）と T1-095（BUG-158 の修正の 5330 passthrough の確認）、P3 で q685〜q690、P1 で WS050 p002 の前提・WS051・WS052・q682、P2 で ws141-p003 と WS037。ユーザーの判断待ち: WS037 の 5〜18、WS051 §13、WS052 §10、ws049-p017 ⑤（hal.h）。
  - **2026-10-04 17 時からの体制（user）**: 設計と実装は P1・P2（phase-runner、high）。試験は Q1 が集めて T1 に送り、T1 は積まれた依頼をできるだけ 1 回の QEMU の起動にまとめて流す。T1 が忙しい時は T2 を立ててよい。P1・P2 は試験を待たず、依存を満たす別の Queue へ移る（WS ごとでなく流れ A〜D の WS 群をまんべんなく進める）。対象は流れ A〜D（q677〜q692）。週間の使用量は 17 時にリセット（0%）。最初の割り当て: P1 = q677（BUG-165）、P2 = q683（BUG-170）。**優先（user 最新）**: P1 = WS049（p007〜p009、BUG-165）・WS050・WS051・WS052、全て塞がった時だけ WS132。P2 = BUG-170 の後は WS141・WS037。Bug の修正は P3（Fable の BUG-158 の解析の後に phase-runner-mid で立て直し）: BUG-158 の実装・流れ B・BUG-165 の残り（P1 の区切りの後）。詳細は [queue.md](queue.md) の「2026-10-04 17 時以降の予定」。
  - 体制: 単一 session の Q1 ＋固定名サブエージェント（実装 P1〜P8、試験 T1・T2）。2026-10-03 夜 user「これによりすべての作業をソフトに停止します。」で P1・P2・T1・T2 はラップアップして終了、全ての成果は main に統合済み（P4 の WS118 の source と記録も Q1 が取り込んだ）。動いている担当は無い。
  - 再開の時の割り当ての候補（user が決める）: P1 = WS131 の p008 の試験と p009 以降、P2 = WS134 の p003 の直しと p004、T1・T2 = 台帳の未実行の予約（`plan/agents/T1/requests.md`・`T2/requests.md`）。WS135（設定の一本化、BUG-162）の担当と時期は未定。
  - **2026-10-04 夜の自律の指示**: user「私は寝ます。昼までには起きると思います。テストが全部終わったら、P2は優先度の高いものから、実装タスクを進めてください。WSが完了できないときは他のWSに移ることで、どんどん先に進めてください。」→ 体制は P2・T1・T2（P3 は WS137 の後に終了）。P2 は WS135 の後、優先度の順に実装の Phase を進め、判断・実機・依存で止まる WS は記録して次の WS へ。Q1 の解釈: WS131 の p009 以降（設計はユーザーのレビュー済み）もこの指示で始めてよい。新しい製品の判断・HAL の API・toolchain・push は従来どおりユーザーの承認まで止める。
  - **2026-10-04 夜〜朝の成果**（全て main に統合、QEMU と FreeBSD・Linux の guest の確認、実機は未実施）: WS135 completed（設定の一本化、BUG-162 resolved）、WS136 completed（試験の image の作り方）、WS137 completed（FreeBSD の試験の VM）。WS131 p004〜p011 cleared（p012 はユーザーの判断待ち）。WS134 p001・p002・p004〜p006・p008・p011〜p013 cleared（p003 の fps・p007 の i915・p009 の ACPI・p010 の残りは実機待ち）。Bug resolved: 033・052・053・103・129・135・139・143・162・163・164（163・164 は今夜の発見）。T1-076 で main 039b00f の boot-test・C1・settings-p007・files-p004 PASS。体制は P2（待機）・T1（待機）、T2 は終了。
  - 2026-10-04 user「任せます」→ Q1 の決定: 標準 app のベータ1 の作業（WS127 p003・p006、WS128 p005・p006）を先にし、WS131 の app の移行（p012 以降）はベータ1 の後。WS134 p003 の fps は実機の値で判定。
  - **2026-10-04 昼 ユーザーの指示**:
    - 「WS13,WS128は進めてOKです。PE/COFFローダも進めてオーケーです。記録してください。」→ Q1 の読み: 直前の切れた「WS131,」と合わせて **WS131（app の移行 p012 以降。ベータ1 の後に回す Q1 の決めを取り消し、進めてよい）・WS128（標準 app の残り、p004 の PDF の文字の検索を含む）・WS080（PE/COFF の動的ローダ `ld.coff`）** を進めてよい。
    - 「F-071、F-072、F-070、はWSを立てて計画を作り、他の能力が低いセッションで処理できるようにしてください。」→ WS138（F-071 PNG の背景）・WS139（F-072 性能）・WS140（F-070 ld.so の上限を動的に）の計画を P1 が作成中。
    - **トークンが余った時にいつでも進めてよい WS**（ユーザー「WS066、WS061、WS061はトークンが余ったときにいつでも進められる内容としてmaster.mdに書いておいてください。」、3 つ目は重複の書き間違いと読んだ）: [WS066](ws066/ws.md)（動的 link の program の起動を速く）、[WS061](ws061/ws.md)（expat の configure と compile を Linux と同等に）。
    - 実機の UAT: 2026-10-04 13 時（観点と手順は [uat.md](uat.md)）。
  - 2026-10-04 11時 user「サブエージェントについて、実装は現在の内容を完成させたらラップアップして終了し、計画と設計のみに移行してください。P2は終了、P1,T1のみにします。P1は設計と計画のみに専念。残りの使用量が少ないので、設計より計画が優先。」（週間の使用量 96%、水曜 6:00 にリセット）→ P2 は p014 を仕上げて終了、T2 は終了、P1 は ws080-p004 を仕上げた後は計画だけ、T1 は残す。
  - **次に流す試験の一覧と手順は [plan/test-queue.md](test-queue.md)**（2026-10-04、試験の担当のラップアップの後。能力の低いセッションでも流せる手順。最初は TQ-1: ws131-p013・p014 の 3 OS の回帰、source は agent/p2 40b242a）。
  - 2026-10-04 全サブエージェントをラップアップ（P1・P2・T1・T2 終了）。未 merge: agent/p2 40b242a（p013・p014、TQ-1 の PASS の後に merge）。
- 2026-10-05 夜まで先頭にあった「Focus」（2026-10-04 17 時からの予定、流れ A〜D）:
  - **fg019 ベータ1 のリリース（公開 2026-10-17、RC の commit 10/13 が事実上の機能の締切、名前「Kei/zedBSD 1.0.0 Beta 1」、版 1.0.0-beta1）**。決定は [ws129/release.md](ws129/release.md) の §9。内容は下の「Current Focused Goals」。
  - fg018 Linux 標準 GTK4（WS114 は p007・p008 まで達成、GTK4 の zedBSD 移植 WS115 は後回し）。
  - **17 時（2026-10-04）からの予定**（2026-10-04 user「次の新規実装項目は、USB-C の DisplayPort Alternate Modeの実現を目標にします。その次が電源管理です。これらは併走できると思います。共通のpredecessorがAMLですね。そうすると、AMLと併走できる開発項目は、UATで見つかったバグだと思います。スケジューリングだけしてmasterやqueueに記録してください。実行は17時以降に行います。」）。詳細と依存は [queue.md](queue.md) の「2026-10-04 17 時以降の予定」（q677〜q690）。
    - **新規実装の目標: USB-C の DisplayPort Alternate Mode（WS051、WS050 の UCSI の上）**、次に**電源管理（WS052）**。両者は並走し、共通の前提は **WS049（AML）**: q677 ws049-p008（BUG-165 の DSDT）→ q678 ws049-p007（電源ボタン・GPE・EC）。WS132（ベータ1、全部）も同じ前提（q682）。
    - **AML と並走: UAT の Bug**（q683 BUG-170 → q684 BUG-158 → q685 BUG-168・169 → q686 BUG-175 → q687 BUG-173 → q688 BUG-171・176・157 → q689 BUG-166・167（仕様の決めと DSDT の後）→ q690 BUG-172・174（実機））。
    - **流れ C（完全に独立）: [WS141](ws141/ws.md) Raspberry Pi 4 のグラフィックス**（q691 p001 の文書から）。
    - **流れ D（完全に独立）: [WS037](ws037/ws.md) nvrtx（NVIDIA RTX 2000 以降）**（q692 p001 の文書から）。
    - その次（担当に余裕があれば）: WS129 p003〜p005（release）、ws131-p014 の残り・TQ-1 の残り → p026、WS138・WS139・WS140、WS080 p004。
- 2026-10-05 夜まで先頭にあった「止まっている物」:
  - [WS155](ws155/ws.md)（カレンダー・スケジューラ・オーガナイザ）: 2026-10-05 ユーザーがデザイン案を提出、blocked を解除。まず UI の mock（3D の animation つき）を見せて再指示を受ける。
  - 5330 の AX211 の passthrough は 2026-10-04 に再開を許可（iwlwifi を blacklist して再起動が要る、Guardrail）。iGPU は i915 の driver の改善の Phase だけで使い、iGPU と AX211 の同時は禁止。
  - 2026-10-04 の UAT（[uat.md](uat.md)、証拠 [uat/2026-10-04/](uat/2026-10-04/)）: 実機で OK は BUG-145・152・161・143・139・103・160・153。再現は BUG-158・119・156・157。新規 BUG-165〜176。**最優先は BUG-165（DSDT が読めない: 電源が切れない・タッチパッド・電池の根の候補）・BUG-158（WiFi 未接続で kernel のフリーズ）・BUG-170（音量の slider のフリーズ）**。タッチパッドの操作の仕様（BUG-166）はユーザーと決める。
  - GitHub への記録の公開は保留（.sync が無い）。push はユーザーの指示の時だけ。
<!-- master:history-log:end -->

## リポジトリの作り直し（2026-10-03 夜、持ち越し）

2026-10-03 user「リポジトリを作り直します。build/以下は削除されます。.git/も削除されます。持ち越したい情報があれば、plan/以下のドキュメントに記載が必要です。」

- **git の履歴**: plan の中の commit の SHA（`6ecf801cc`・`f94b1b633` など、2026-10-03 以前の全て）は古いリポジトリの物で、作り直した後は引けない。記録の意味（どの変更か）は文で残っている。
- **build/ の証拠**: plan が指す `build/…` と `/home/awe/zedBSD-worktrees/*/build/…` の PNG・log・image（S1 の image `build/s1-pre/hdd-image.img` を含む）は消える。結果と判定は各 phase.md・Bug の ticket・`plan/agents/T1|T2/requests.md` に文で残っている。消えた証拠を指す行を「証拠の file が残っている」と読まない。
- **作り直した後の手順**（Q1）:
  1. commit の hook: `cp plan/tools/git-hooks/commit-msg .git/hooks/ && chmod +x .git/hooks/commit-msg`（メッセージが `WIP` ちょうどでない commit を拒否。AGENTS.md「git」）。`.claude/settings.json` の `attribution`（Co-Authored-By などの付記を無効）は tree にあるので残る。
  2. remote は `git@github.com:awemorris/zedBSD.git`、author は `Awe Morris`。push はユーザーが指示した時だけ（AGENTS.md の確かめの手順）。
  3. サブエージェントの worktree: 古い `/home/awe/zedBSD-worktrees/*`（agent/p1〜p4・t1・t2、codex/a1〜a3・p8〜p10）と `/home/awe/zedBSD-rpi4/.claude/worktrees/*` は古い `.git` に結び付くので使えない。担当を起こす時に main が `git worktree add /home/awe/zedBSD-worktrees/<名前> -b agent/<名前>` で作り直す（[protocol](agents/protocol.md) の 1）。古い worktree の directory の削除はユーザーが行う。
  4. toolchain: `make toolchain` などで `build/llvm`・`llvm-source`・`llvm-build`・`NoctLang` を作り直した後に `plan/tools/toolchain-lock.sh lock`。
  5. Linux の試験の guest（WS105・WS131、QEMU+KVM）: `plan/tools/keiland-linux/build-guest.sh`（base と gdm）で `build/keiland-linux/guest`・`guest-gdm` を作り直す。guest の SSH の鍵 `plan/tmp/guest/` は tree にある。
  6. 試験の image の入力（2026-10-04 Q1 が作り直した）: `build/ws035-fonts/`（`userland/desktop/fonts/` の Inter.ttf・Inter-OFL.txt → OFL.txt・JetBrainsMono-Regular.ttf・JetBrainsMono-OFL.txt・DroidSansFallbackFull.ttf・DroidSansFallback-LICENSE.txt の複写）、`build/ws035-wallpaper/`（wallpaper.ppm・wallpaper-1080.ppm = `userland/desktop/wallpapers/Birch-Lake.ppm`）、`build/ws071-fonts/`（DroidSansFallbackFull.ttf・LICENSE・Apache-2.0.txt）。Venus の renderer `build/ws035-sq-venus/install` は Latitude 5330（10.0.30.3）から scp（`plan/ws035/tests/zdesktop-guest.sh` の注記、sha256 も）。host の `sudo modprobe vgem && sudo chmod 0666 /dev/dri/renderD128` は host の起動ごとに。
  7. FreeBSD の試験の guest: `plan/tools/keiland-freebsd/build-guest.sh` で `build/keiland-freebsd/guest` を作る（鍵は OUT の中に作る）。
  8. 統合の前の残り: 無い（全ての agent の branch は main に入っている。`worktree-agent-aefedcaf…` の 2 commit は BUG-066 の決定で merge しない物）。P1 の `build/p1-q640/p007-wip.patch` は p007 が commit 済みなので不要。
- **tree に残るが git に入らない物**（削除の対象外の前提）: `.wifi`（WiFi の認証情報、ユーザーが作成）、`.claude/settings.local.json`、`config.mk`、`plan/*/temp/`。
- **host の状態**（repo の外）: sysctl の一時の設定（`vm.dirty_background_bytes=512M`・`vm.dirty_bytes=2G`・`vm.swappiness=10`、2026-10-03 user「sysctl の調整はやってみてください」）は再起動で戻る。残すかは user の判断待ち。fstrim は 2026-10-03 に実行済み（35 分、204.6 GiB）。


## Tools

回帰と観察の道具は `plan/tools/` に置く。完了した WS の試験は、ここへ移したもの以外を削除した。Phase に固有の試験は各 WS の `tests/` にある。

注意（2026-10-09 P1 の所見）: BUG-274 の直しで `ps -o args` が command line の全体を出すようになった。guest の試験で `ps -A -o pid,args | grep <語>` で選んで kill する形は、その語が試験自身を走らせる shell の行にも出ると自分を kill する（ws172 の passkey の FAIL の原因）。新しく書く試験は `ps -A -o pid,comm` で選ぶ。既存の約 380 箇所は一斉に直さず、FAIL した時に追従する（試験の整理の基準を当てる）。

| tool | 用途 | 使い方 |
| --- | --- | --- |
| [q1-clean.sh](tools/q1-clean.sh)・[fresh-out.sh](tools/fresh-out.sh)・[files/host-clean.sh](tools/files/host-clean.sh)（2026-10-06） | 削除は Q1 の pipeline（ユーザーの規則）。host の試験の script は rm を持たず、`fresh_out NAME`（新しい `NAME.run.*` を作り NAME を symlink で向ける）か `build/tmp/` の mktemp を使う。Q1 が `q1-clean.sh WORKTREE` で古い run・tmp・old を消す | `. plan/tools/fresh-out.sh; fresh_out "$out"`、Q1: `sh plan/tools/q1-clean.sh /home/awe/zedBSD-worktrees/p1` |
| [gtk4-linux/](tools/gtk4-linux/README.md)（WS114 から移した、2026-10-06） | Linux の Keiland の上の標準 GTK4 の装飾（CSD・SSD）の試験と session の起動 | `decoration-wire.py`・`start-session.sh` など、README を参照 |
| [ws125/tests/subtree-host-test.sh](ws125/tests/subtree-host-test.sh)（2026-10-09、WS125 の完了時に plan/tools/ へ移す） | package の staged tree を image に入れる `--subtree DEST=DIR` の回帰（Makefile の tree の規則・UFS・FAT の道具と検査、一覧と中身の一致） | host で `plan/ws125/tests/subtree-host-test.sh`、PASS で終わる |
| [compositor/](tools/compositor/README.md)（WS110） | compositor の起動の role（--testing・--session・--greeter）の試験 | `run-host-role.sh`、`roles-guest.sh` |
| [rtld/](tools/rtld/README.md)（WS140） | ld.so の多数の object・依存・handle・TLS の試験 | `rtld-many.sh BUILD`（`config-amd64-rtld.mk` の SSH の image、BUILD/sysroot の symlink） |
| [wallpaper/](tools/wallpaper/README.md)（WS138） | 背景の PNG・JPEG の復号と読み込みの時間・greeter の背景 | host `run-host-wallpaper-decode.sh`、guest `wallpaper-time.sh`（Settings の image）・`greeter-wallpaper.sh`（criteria の image） |
| [media/](tools/media/)（WS122 から移した、2026-10-08） | 動画・音の読みと decode の host 試験: `run-host-mediafile.sh`（mediafile の MP4 などの reader、`make-media.py` の試料と `sample.mp4`）、`run-host-codec.sh`（libmedia の decoder と libavcodec の add-in、`host-layout.c` で libavcodec の構造体の配置を確かめる）。`sample.mp4` は WS191 の音の image・WS121 の試験も使う | `sh plan/tools/media/run-host-mediafile.sh`、`sh plan/tools/media/run-host-codec.sh` |
| [files/run-host-files-recents.sh](tools/files/run-host-files-recents.sh)（WS148 から移した、2026-10-08） | Files の Recents（止めた一覧・Clear Recents）と Storage の Keep recent items の host 試験（WS177 の recents-p008.sh が呼ぶ） | `sh plan/tools/files/run-host-files-recents.sh [OUTPUT]` |
| [mail/fake-mail-server.py](tools/mail/fake-mail-server.py)（WS169） | 偽の IMAP4・SMTP の server（自己署名の TLS・STARTTLS、--bind・--arrivals） | Mail の host 試験（plan/ws169/tests の run-host-mail-backend.sh ほか）と AAT の apps.mailer.read-compose・sign-in-code（helpers_mailer.py） |
| [ws001/tests/tabs-stops.py](ws001/tests/tabs-stops.py)（WS001） | tabs の escape の流れを止めの列の集合に直し、ncurses 6.5 の tabs と比べる（host） | `python3 plan/ws001/tests/tabs-stops.py`（55 件） |
| [ws051/tests/host-vbt-pll.sh](ws051/tests/host-vbt-pll.sh)（WS051） | i915 の takeover.c の VBT の DVO の code と TC PLL の enable の番地を Linux の値と比べる（host） | `sh plan/ws051/tests/host-vbt-pll.sh`（PASS 3） |
| [ws051/tests/host-tc.sh](ws051/tests/host-tc.sh)（WS051） | i915 の Type-C の核（tc.c を fake の register で）と AUX の domain・DKL の window・HPD の long pulse の表（host、ASan/UBSan） | `sh plan/ws051/tests/host-tc.sh`（host-tc 112・host-tc-tables 34） |
| [gnu-utils/](tools/gnu-utils/)（WS045） | base の text utility の GNU 拡張の差分の試験: `cases/`（awk・grep・misc・sed・sort）を GNU の実物と比べる（`plan/tools/utils/util-diff.py` が使う）、base の image の config（`config-amd64-base.mk`、他の試験の config が include する）、guest での case（`build-guest-utils.sh`・`guest-batches.sh`、full の guest image が要る） | `python3 plan/tools/utils/util-diff.py`（WS043・WS045 の手順）、`make ZEDBSD_CONFIG=plan/tools/gnu-utils/config-amd64-base.mk BUILD=… disk-image` |
| [FreeBSD 15.1 の試験の guest と backend の試験](tools/keiland-freebsd/README.md)（WS137） | 公式の 15.1 の image（CHECKSUM を q550 の記録と照合）と NoCloud の seed から作る QEMU+KVM の guest。loopback の SSH と QMP の PNG を使い、serial の log は読まない。guest の中で keiland-freebsd.mk を native で build（warning 0）し、audit と ws131 の host 試験を流す。GPU は無い | `build-guest.sh [--force] [OUT]`、`guest.sh start\|stop\|status\|ssh\|put\|get\|copy\|shot`、`backend-test.sh [OUT]`。T1・T2 は自分の build/ に作るか GUEST_DIR で読み取り専用で使う。2 つ同時は GUEST_RUN と SSH_PORT を分ける |
| [settings/](tools/settings/) | WS135 の設定の試験: compositor の store（`host-store.sh`）、libkeiland の `kl_settings_*`（`host-settings.sh`）、Wayland 無しの stand-in（`host-kl-settings.c`、Files・Terminal・Settings の host 試験が使う）、QEMU の `settings-p003.sh` と image の `config-amd64-settings.mk` | `sh plan/tools/settings/host-store.sh`、`sh plan/tools/settings/host-settings.sh` |
| `plan/tools/git-hooks/commit-msg` | git の commit-msg の hook（メッセージが `WIP` ちょうどでない commit を拒否、Co-Authored-By などの混入の防止、2026-10-03） | `cp plan/tools/git-hooks/commit-msg .git/hooks/ && chmod +x .git/hooks/commit-msg`（clone・作り直しの後に毎回） |
| `plan/tools/toolchain-lock.sh` | 共有の toolchain の tree（`build/llvm`・`llvm-source`・`llvm-build`・`NoctLang`）の directory を読み取り専用にして、許可の無い変更を防ぐ（BUG-096） | `lock`・`unlock`（main が許可した toolchain の変更の間だけ）・`status` |
| [boot-test.sh](tools/boot-test.sh)（`boot-test.py`） | 起動の確認。OVMF の USB（amd64）か BIOS の IDE（i386）で起動し、画面を QMP で撮って login prompt を読む | `plan/tools/boot-test.sh [IMAGE]`。`OUTPUT`（既定 `build/boot-test`）、`BOOT_TIMEOUT`、`BOOT_MODE=uefi-usb` か `bios-ide` |
| [Keiland の OS 境界 checker](tools/keiland-os-boundary/check.sh)（WS104） | 共通 source の OS include / ioctl、GPU layout の所有、install literal、libc に残る desktop header を C1〜C5、Linux/zedBSD moduleと実build membershipをL1〜L5で確認。evdev の 1 行だけを例外とする。2026-10-08 WS188 p003: A1〜A5（app と libkeiland の OS の操作: /dev 等の literal・AF_UNIX・getpw*/statvfs/mount・fork/exec・OS の include）と許可の表 [app-allow.tsv](tools/keiland-os-boundary/app-allow.tsv)（行ごとに理由） | `sh plan/tools/keiland-os-boundary/check.sh`。PASS は exit 0、違反は各項目の file:line と exit 1 |
| [Keiland の machine の host 試験](tools/keiland-machine/host-machine.sh)（WS188） | libkeiland-backend の machine（system の情報・file system・mount・利用者・言語）の読みを host で 118 の check（`sh plan/tools/keiland-machine/host-machine.sh`、2026-10-08 WS188 の完了で ws188/tests から移した） |
| [Keiland launcher確認](tools/keiland-launcher/README.md) | 共通shellのruntime/env/argv/customprefix/exec signal、GPU/VTを取得しない | Python3 check.py＋launcher.in。Linux/FreeBSDで実施 |
| [PDF の検索と選択の host 試験](ws177/tests/host-pdf-find-l.sh)（WS177 案 L） | libpdf の form の文字・検索の規則（行をまたぐ語・ハイフン・大小・全角半角・合字）・数・選択の 52 checks（`sh plan/ws177/tests/host-pdf-find-l.sh`、plain と ASan+UBSan、2026-10-08 登録） |
| [FreeBSD native の検証](tools/keiland-freebsd/README.md)（WS109） | actual native header/ELF/borrowedfd、properVulkanwindow、IntelGPU/VTlease/input、主要app/PTY/fileopen。専用guest限定、mockを実GPU結果としない | READMEのnativecompile/fixture手順。SSH/QMPの操作は既存承認範囲だけ |
| [Linux の試験 guest と操作の道具](tools/keiland-linux/README.md)（WS105） | Debian 13 の image / overlay・loopback SSH・QMP screenshot / 入力・install・PNG の画素。host の画面を使わない | `build-guest.sh` / `guest.sh` / `install-guest.sh` / `png-probe.py`、build/ELF/header/source の checks、Vulkan chain/interpose と `wsi-check.sh`（90 frame ×4）。README の timeout 付き command |
| [Keiland の zedBSD の検証手順](tools/keiland-linux/zedbsd-commands.md)（WS104 から移した） | build / warning・sysroot・boot・C1/C2/C9・GPU・glass / pen・Settings / 音量の既存回帰。image build は直列、BUILD と OUTPUT を個別指定 | 各節 §0〜§9 |
| Dell Latitude 5330 の実機の操作とデモの image（[tools/hw5330](tools/hw5330/README.md)、2026-10-01） | 実機の構成（5330 自身が host の passthrough、ssh `solaris10-man`）、`/tmp/i915-hw.lock`、画面・入力・結果の読み戻し、USB の単独の起動（ユーザー）、`build-demo-image.sh` と boot の行の落とし穴、実機の試験の script の一覧と PASS の印、よくある失敗。デモの優先 WS の作業の手引きは各 `plan/wsNNN/guide.md` | README.md |
| GPU の境界の試験（[tools/gpu-boundary](tools/gpu-boundary/)、WS103 から移した） | compositor が GPU を Vulkan だけで扱うことの確かめ: `v1-check.sh`（GPU の UAPI の include と ioctl が `gpu-zedbsd.c` の外に無い、GPU の UAPI の header を `#error` にして compositor が compile できる、`/dev/gpu`・`--gpu` が無い）、host の `run-dedicated-host.sh`（libvulkan の dedicated の import の照合）・`run-gpu-zedbsd-host.sh`（compositor の wire の値の確かめ）、guest の `forge-guest.sh`（偽の buffer を断る、`/bin/gpu-forge-test`）・`fence-guest.sh`（Wayland の present ごとの新しい fence、`--log-frames` の `ZWL ACQUIRE_FENCE`）。guest の image は `build-forge-image.sh`（`config-amd64-forge.mk`: 基準の image に gpu-forge-test・wltest・acquire-fence-test） | 各 script の先頭の使い方 |
| [pc98-boot.py](tools/pc98-boot.py) | pc98 の起動の確認（`boot-test.sh` に PC-98 の mode が無いため）。PC-98 fork の QEMU で起動し、text VRAM で login prompt を読み、root で login して `uname -a`。画面を text と PNG で残す。WS053 から移した | `pc98-boot.py ~/qemu-pc98/build/qemu-system-i386 IMAGE OUTPUT`（`clock/pc98-sleep.py` の `Guest` を使う） |
| [guest/guest.sh](tools/guest/guest.sh) | SSH による guest の操作（USB CDC-ECM、KVM）。コマンドの実行・file の送受・lldb・kgdb・画面 | `start IMAGE`・`wait`・`run CMD`・`put`・`get`・`lldb`・`kgdb`・`screenshot`・`stop`。image は `extra-files` の出力を eval して作る。`GUEST_RUNTIME=<dir>` で別の guest を並べて動かせる（既定 `build/guest`） |
| [guest/serial.py](tools/guest/serial.py) | シリアルの console と対話する（sshd が上がる前。`CONFIG_PCAT_SERIAL_MIRROR=y`） | `serial.py --socket S run 'CMD'`（終了状態を返す）、`login` |
| [qmp.py](tools/qmp.py) | QMP の command を送る | `qmp.py SOCKET quit` など |
| [latency/](tools/latency/) | interactive の応答の測定（起床の遅れ、端末の echo）。WS041 から移した | `run-echo-qemu.sh`、`run-wakebench-qemu.sh`、`pc98-wakebench.py`、`config-*-bench.mk` |
| [clock/](tools/clock/) | guest の時計の進み（`sleep 5` の実時間）。WS040 から移した | `clock-check.py`、`pc98-sleep.py` |
| [ufs/](tools/ufs/) | UFS の directory の試験と volume の検査。`dir-grow.sh` は mount した volume で directory を 12 block まで育て（作成・削除・rename・rmdir・上限）、`verify` で確かめる（`LONG`・`SHORT`・`MOVE`・`GONE` で数）。`check-volume.py` は guest が書いた volume を host で fsck 相当に検査する。`crash-test.sh` は journal の volume の成長の途中で QEMU を止めて replay を確かめる。WS054 から移した | `sh dir-grow.sh DIR make\|verify`（guest）、`check-volume.py IMAGE`、`crash-test.sh IMAGE SECONDS...`（host）。作業の volume は `zedimage-host ufs SIZE EMPTYDIR IMAGE --inodes=16384 [--profile=journal-snapshot]` で作り、NVMe（`-device nvme`）でつなぐ |
| UFS の journal の試験（[tools/ufs](tools/ufs/)、WS063 から移した） | `crash-test.sh`（既定は v3・NVMe の作業 volume、`PROFILE=journal-snapshot` で v2）、`journal-func.sh`＋`journal-guest.sh`（guest での journal の機能: 隠しの `.ufs-journal`、最初の mount での作成、`nojournal`・`writethru`）、`root-crash.sh`（root の強制終了と replay）、`zedimage-compare.sh`（2 つの zedimage-host の UFS の出力の byte 比較） | 各 script の先頭の使い方。`GUEST_RUNTIME`・`VOLUME` を上書きできる |
| SSH の guest image（[guest/](tools/guest/)、WS063 から） | clang の無い SSH の guest image: `config-amd64-ssh.mk`、`build-ssh-image.sh`（package が build/amd64/dynamic に link するので build/amd64 に作る） | `plan/tools/guest/build-ssh-image.sh` |
| guest の Wayland client の番号（[zwl-clients.sh](tools/guest/zwl-clients.sh)、2026-10-03 ws099-p025・BUG-146） | Venus の guest 試験で app の client の番号を compositor の log（`ZWL CLIENT`）から求める共有の helper。`zwl_app_clients`（ime=1 でない client を接続順に zc1〜zc4）、`zwl_app_client N`。keiland-ime が先に client 1 を取っても試験が外れない。`. plan/tools/guest/zwl-clients.sh` で読み込む |
| QEMU の加速と build の並列の数（[qemu-accel.sh](tools/guest/qemu-accel.sh)・[jobs.sh](tools/guest/jobs.sh)、2026-10-03 ws129-p011） | `qemu_accel_args [MODEL]`: `/dev/kvm` が使えれば `-accel kvm -cpu host`、無ければ TCG（`QEMU_NO_KVM=1` で外せる、guest.py も同じ）。user「すべてのテストで統一して、kvmを使いましょう。」。`ZEDBSD_JOBS`（既定 16、image の build の make の並列の数）。user「フルビルドは-j16にして、複数かぶってもいいようにできませんか？」 |
| 組み合わせの guest image と process の試験（WS064 から） | `guest/hybrid-image.sh BUILD OUT`（この tree の full の guest image を `guest/build-full-image.sh`・`guest/config-amd64-full.mk` で build して OUT に写す。他の tree の image を写して差し替える形は 2026-10-04 の WS136 p003 でやめた）、`guest/make-cases.sh IMAGE`（guest の make-diff）、`process/vfork-test.c`＋`guest-vfork.sh IMAGE`（fork の COW、vfork、posix_spawn、並行の fork） | 各 script の先頭 |
| NVMe と lease の試験（WS072 から） | `nvme/timeout-retry.sh`（QMP の block_set_io_throttle で 2 台目の NVMe を絞り、timeout の後の再発行を確かめる）、`ufs/format-lease-probe.c`（format の lease の下の fsync） | 各 file の先頭 |
| toolchain の試験（WS055 から） | `toolchain/link-undefined-version.sh CLANG`（version script の未定義の symbol の link）、`toolchain/zlib-shared-configure.sh CLANG SYSROOT`（zlib の configure が共有 library を作れること） | host で実行 |
| rpi4 と amd64 の serial の guest（WS036 から） | `guest/rpi4-serial.sh`（raspi4b の guest に serial で login して command を実行、`APPEND` で /chosen/bootargs）、`guest/amd64-serial.sh`（amd64 の UEFI・NVMe・KVM、image に `CONFIG_PCAT_SERIAL_MIRROR=y`）、`rpi4/bootargs-rpi4.sh`（rpi4 の boot の parameter の試験）、`rpi4/noct-rpi4.sh`（rpi4 の Noct の JIT と API） | 各 script の先頭 |
| File Manager の試験（[tools/files](tools/files/)、WS071 から移した） | lean な Venus の guest image（`build-files-image.sh`・`config-amd64-files.mk`）と guest（`files-guest.sh`、runtime `build/ws071-run`、`build/ws035-sq-venus` の renderer が要る）。`files-regress.sh [OUTDIR] [PHASE...]`（zdesktop-files の guest 試験 14 本）、`files-p011.sh`（App Home）、`files-p018.sh`（configure_bounds と置き場所）、`files-lag.sh`。host の files-render（`host-build.sh`・`host-run.sh`・`host-p009/p010/p013/p014.sh`）、`host-png.sh`（libz-compat・libpng-compat を Python と比べる）。`make-home.sh`・`qmp-input.py`。`host-build.sh` は libkeiland の gesture.c・scroll.c・motion.c も build する（ws093-p003）、`host-model.sh`（files-model を一時 folder で）、`host-default.sh`（Always Open With の利用者の一覧、WS093 から）、guest の `files-open.sh OUTDIR mouse|always|info|touch`（file の種類ごとの起動、WS093 から） | 各 script の先頭の使い方 |
| System Menu と Titlebar の試験（[tools/titlebar](tools/titlebar/)、WS070 から移した） | lean な guest image（`build-menu-image.sh`・`config-amd64-menu.mk`、probe 入り。WS035・WS071・WS074 の image の元）と guest（`menu-guest.sh`、runtime `build/ws070-run`）。`menu-p002.sh`（protocol の error）、`menu-p003.sh`（terminal の menu）、`menu-occlude.sh`、`menu-regress.sh OUTDIR TEST...`（WS035 の zdesktop の試験）、`titlebar-p008/p009/p010/p011/p013.sh`（model、glyph、CONTROLS、TABS、tab の key と wheel）、`icons-host.c`、`style-compare.sh REV FILE...`、`menu-hw.sh`（i915 実機、`flock /tmp/i915-hw.lock` の下で） | 各 script の先頭。files の guest で走らせるときは `GUEST_RUNTIME=build/ws071-run` |
| i915 の実機の試験の場面（WS075） | `plan/ws075/tests/test-hw.sh`（`flock /tmp/i915-hw.lock` の下で試験の場面（vke1・vke2・vkx・vkc ほか）を走らせ、共有の /tmp から log を写す）、`capture-hw.sh`（ZDESKTOP_APP ごとの build の directory で zdesktop の capture）、`config-test-hw.mk`（zdesktop の実機の config と serial の mirror）、`shader-survey/run.sh`（host で 122 の module を i915 の compiler の不足と照合）、`vk-calls.py`（client の Vulkan の command と実行器の対応）、`bug085-hw.sh IMAGE OUTDIR [SCENARIO]`（ws075-p015: vkloop-hw.sh の作った zdesktop の capture の image を gdbstub 付きで 1 回、capture の frame の止まり・fault を印にして QEMU を debugger のために残す。`bug085/`。watcher は起動の停止・APIC timer の較正の誤り（BUG-094）・session の終わりの halt の有無も印にする。`bug085/procs.py`・`kstack.py`・`ustack.py` は DWARF 無しの gdb で process・thread・kernel と user の stack を読む、`summary.py` は run ごとの 1 行）。注意: i915 の試験の build の kernel は 16 MiB の上限（AMD64_KERNEL_MAX_BYTES、.bss を含む）の近く。2026-09-28 に 28 KiB 超えたので vkx の場面の state（約 240 KiB）を heap へ移した | 各 script の先頭 |
| shader の網羅の調査（WS075） | `plan/ws075/tests/shader-survey/run.sh`（tree の 132 の SPIR-V module を i915 の compiler に通し、受け入れ・拒否を gaps.txt に。`GLSL_HOST=DIR` で glsl-host の出力を選ぶ、OUTDIR は fresh_out の link。2026-10-07 P1 a4 で geometry の規則と古い path を直した） |
| Venus の head の抜き差し（WS113） | `plan/tools/guest/venus-head.sh`（`VENUS_DISPLAY=dbus` で起動した zdesktop-guest の QEMU の D-Bus display の Console に SetUIInfo を送り、head を挿す（幅・高さ）・抜く（幅 0）。`list` で console の一覧）。`plan/ws035/tests/zdesktop-guest.sh` の `VENUS_DISPLAY=dbus`（runtime の私的な dbus-daemon と `-display dbus,gl=on`、既定は今まで）と `VENUS_OUTPUTS=N`（max_outputs） |
| HDMI の主出力の試験とデモの image（WS075 p011〜p013） | `plan/ws075/tests/hdmi-h1-hw.sh SCENARIO OUTDIR [FLAGS]`（i915 の試験の場面を lock の下で走らせ、5330 の host の USB を 1 秒ごとに記録、新しい device の HID の report descriptor を取る）、`hdmi-h2-hw.sh OUTDIR "BOOT LINES" [秒...]`（`ZEDBSD_BOOT_EXTRA_LINES` の zdesktop の image を実機で起動し、resident の scanout の buffer を QMP の memsave で PNG に）、`hdmi/host-output-test.sh`（`display=`・`display.mode=` と EDID・CVT・mode の選択の host 試験）、`hdmi-h4-hw.sh`（start・ctl・fetch・stop）（ws075-p016: `h4-ctl.py watch SECONDS MS` は pipe B の TRANSCONF・PLANE_CTL・PLANE_SURFLIVE を 20 ms ごとに標本化、`hdmi/h4-blank.py` はその暗・黒の区間、`hdmi/h4-cycle.sh OUTDIR COUNT` は logout・login の繰り返しと撮影。`shot` の live は PLANE_SURFLIVE から）（実機を段ごとに: lock、QEMU、std VGA の splash の連写、resident の buffer の画面、QMP の pointer・key・drag の周期負荷、guest の disk の log。`hdmi/h4-*.{sh,py}`）。デモの image: `plan/ws075/demo/build-demo-image.sh [BUILD] [passthrough]`（graphical boot + `display=hdmi` + App Home。i915 の node は sessiond が `hw.gpu.attaching` の間だけ待つ（ws035-p113、`greeter_gpu` の回避は削除）） | 各 script の先頭。`plan/ws075/tests/hdmi/h4-ctl.py latency PIPE COUNT`（pointer の移動から次の flip まで、ws084）・`rate PIPE SECONDS`（入力が続く間の flip の率、ws075-p008） |
| ls の GNU との比較（[tools/ls](tools/ls/)、WS086 から移した） | `compare-gnu.py OUR_LS [--gnu /bin/ls]`（host で GNU ls と byte 単位、端末の幅と pipe、locale、環境変数）、`guest-compare.py`（guest の ls を `ssh -tt` と pipe で GNU と比べる）、`tty-run.py COLUMNS CMD`（指定の幅の擬似端末で byte のまま取る） | `python3 plan/tools/ls/compare-gnu.py build/.../ls` |
| sh の対話の試験（[tools/sh](tools/sh/)、WS087 から移した） | host の pty で sh の行編集を試す: `history-host.py`（履歴の file と矢印）、`complete-host.py`（Tab の補完）、`prompt-host.py`（prompt の ~）、`pty-keys.py`（ssh -tt で guest の sh に key を送る）。既存の `vi-host.py`・`sh-interactive.py` と同じ場所 | `python3 plan/tools/sh/complete-host.py` |
| 画像 viewer の試験（[tools/imageview](tools/imageview/)、WS091 から移した） | `run-host.sh`（host: 復号の画素を PIL と比べる、folder の順、view の計算）、`imageview-guest.sh OUTDIR STEP...`（Venus の guest、`make-images.py` の画像）、`touch-guest.sh`（注入の touch: pinch・flick・double tap・swipe・長押し）、`style-extra.py`（style-check が見ない規則の候補の発見的な走査） | `sh plan/tools/imageview/run-host.sh` |
| 共有の file chooser の試験（[tools/keiui](tools/keiui/)、WS092 から移し ws090-p006 で libkeiui へ） | `host-chooser.sh`: libkeiui の `kui_file_chooser_*` の model と描画・`kui_ui` を通した key・click・tap の host 試験（85 件）、絵は `build/keiui-shots/` | `sh plan/tools/keiui/host-chooser.sh` |
| Text Editor の試験（[tools/textedit](tools/textedit/)、WS092 から移した） | `host-core.sh`（58 件、2026-10-07 BUG-248 の Find の panel を含む）: 文書・undo・file・表示の行・検索・編集の host 試験（34 件）。`qmp-keys.py`: QMP で文字列・key の組・pointer を guest に送る（US の配列） | `sh plan/tools/textedit/host-core.sh` |
| i915 の実機の計測（WS075） | `plan/ws075/tests/hdmi/measure-apps.sh`（lock の下で 1 回の計測の run、WS099 の C6 の試料を含む）、`c6.py OUTDIR...`（5 run 以上をまとめた C6 の判定: pointer を動かしてから cursor が行き先に出る flip まで、中央値・p90・run の幅）、`engine-gdb.sh`（session ごとの engine の時間を gdb で読む）、`h4-ctl.py`（latency・rate・freq・c6）、`stress-117.sh`（10 app の上で Model viewer の開閉を繰り返し、描画の停止と descriptor の消失を数える）。compiler の guard の host 試験 `plan/ws075/tests/guard/run.sh`（Mesa の brw_asm・brw_disasm と byte で比べる） | 各 script の先頭の使い方 |
| Mesa 25.0.7 の intel の道具（WS101・WS075・ws101 glsl の byte の往復の試験） | `sh plan/ws101/tests/host/mesa-tools.sh [DIR]`: tarball を取得・SHA-256 で検証し meson -Dtools=intel で brw_asm・brw_disasm を build（各 checkout の `build/mesa-tools`、約 2 分）。試験の既定の BRW_TOOLS・GENXML はここを指す。無ければ往復の確認だけ NOT RUN（2026-10-05、<checkout>/build/mesa-tools（2026-10-05 以前は /home/awe/p014-c、消えた） は消えた） |
| GPU の compute の compiler の host 試験（WS101） | `plan/ws101/tests/host/run.sh`: compute の module の compile、scoreboard、descriptor の応答の bit、EOT、Mesa 25.0.7 の brw_disasm・brw_asm との byte の照合、拒否すべき shader、IR の interpreter での結果の照合（add・ids・atomic・length・dynamic・noct）、実行器の compute の object の試験（executor-test）、p004 の dispatch の batch の試験と genxml の照合（Mesa の genxml を使う。既定は `<checkout>/build/mesa-tools（2026-10-05 以前は /home/awe/p014-c、消えた）/mesa/src/intel/genxml`） | `sh plan/ws101/tests/host/run.sh` |
| GPU の compute の実機の試験（WS101） | `plan/ws101/tests/hw/run-hw.sh`: 試験の組 `I915_TEST_SET`（既定 `all` は vkcs 以外、`compute` は vkcs だけ。試験の kernel の上限 16 MiB のため）で image を lock の外で build し（lock の待ちを含む上限 1 時間）、`flock /tmp/i915-hw.lock` の下で実行器の回帰（vkx・vke1・vke2・vkc）と compute の場面 `vkcs`（ONE・ADD・ID・ODD・PUSH・ATOMIC-SSBO・MIXED・MANYOPS・SPILL）を 5330 の passthrough で順に走らせる 。`plan/ws101/tests/hw/gles-hw.sh`（と `gles/` の構成）: GLES 3.1 の compute（`glescompute`）と egltest の feedback・queries を 5330 の passthrough で、lock の外で build し lock の下で走らせ、guest の disk の log を読む | 先頭の使い方 |
| GLSL ES 3.10 の compute の試験（WS101） | `plan/ws101/tests/glsl/run.sh`: libglesv2 の GLSL の compute の compile・link・spirv-val・i915 の host の compile・brw の往復、断るべき 17 shader、host の Vulkan（lavapipe）での実行と C の計算の照合（Noct の shader の形を含む） | `sh plan/ws101/tests/glsl/run.sh` |
| GLES 3.1 の compute の試験（WS101） | `plan/ws101/tests/gles/run.sh`（host の reflect）、`build-image.sh`（image を `build/ws101-p009-img` に）、`venus.sh`（自分の runtime `build/ws101-p009-run` で guest を起こし、`/bin/glescompute` の version・limits・add・noct・shared・indirect・chain・release・repeat・errors） | 各 script の先頭の使い方 |
| Noct の GPU の見本の試験（WS101） | `plan/ws101/tests/noct/g3-venus.sh`（Venus で `mix.nct` を CPU と GPU で走らせて一致と dispatch の数を見る）、`plan/ws101/tests/hw/g3-hw.sh`（同じことを 5330 の passthrough で、`NOCT` で accel の noct を指定）。libglesv2 の `KEI_GLES_COMPUTE_TRACE` で dispatch ごとの行を出す | 各 script の先頭の使い方 |
| compositor のデモの基準の試験（WS099） | `plan/ws099/tests/criteria.sh`: 基準 C1〜C10 を QEMU の Venus で一括して確かめる（閾値は先頭の変数）。基準の image は `build-criteria-image.sh`・`config-amd64-criteria.mk` | `sh plan/ws099/tests/criteria.sh` |
| libwayland の host 試験（WS035 p075） | `plan/ws035/tests/p075/run-host.sh`（host の libwayland-server と試験の protocol で、生成された protocol の event と server の作る object、client が壊した server 側の object（zombie）への event と fd、id の再利用（p089）） | host で実行 |
| xdg-shell の popup と toplevel の試験（WS035 p076） | `plan/ws035/tests/zdesktop-p076.sh`（Venus の guest、`/bin/popup-probe`（`config-amd64-menu.mk`）で menu・submenu・flip・reposition・dismiss、toplevel の move・resize・min/max size、ping の無応答の表示を QMP で操作し画面を撮る） | 先頭の使い方。PNG は `build/ws035-p076/` |
| sub-surface と seat の試験（WS035 p077・p078） | `plan/ws035/tests/zdesktop-p077.sh`（`/bin/subsurface-probe`: 位置・sync・desync・place_above/below・破棄）、`plan/ws035/tests/zdesktop-p078.sh`（`/bin/seat-probe`: XKB keymap・repeat_info・lock の modifier・wl_output v4）、`plan/ws035/tests/p078/run-host.sh`（host の libxkbcommon で zdesktop の keymap を compile し modifier と keysym を照合） | 先頭の使い方。Venus の guest |
| POSIX の console の試験（[tools/posix](tools/posix/)、WS056 から移した） | `console-posix-r2.sh IMAGE ELF [N]`（serial mirror の kernel `config-amd64-serial.mk` の guest の console で `POSIX-R2.ELF` を N 回、`AS_SH=1` で /bin/sh としても）、`guest-sigev.sh`＋`sigev-thread-mask.c`（SIGEV_THREAD と置き換えの mask の EINTR）、`guest-spawn-probe.sh`＋`spawn-probe.c`、`console-probe.sh`、`guest-pax-test.sh`・`make-pax-archives.sh`（pax・gnu・ustar の展開の比較） | 各 script の先頭の使い方 |
| [kbench/](tools/kbench/) | kernel の microbenchmark（system call、pipe の往復、fork、exec、cached の read、anonymous と file の fault）。kernel の build（LTO・最適化）の比較に使う。WS053 から移した | `kbench/build.sh BUILD OUTPUT`（amd64 の guest 用）で作って guest で `kbench [file]`。予熱の 1 回の後に数回走らせ、中央値で比べる。2026-10-03 `ffault.c`（file に裏付けられた page の fault の時間、BUG-027 の計測、`build.sh` の PROGRAM 引数で作る）を追加 |
| [driver-fragments/prepare.py](tools/driver-fragments/prepare.py) | 統合した driver の source から host 試験用の断片を切り出す（出力は `build/driver-fragments`）。WS025 から移した | WS004 の AX211・xHCI と WS001 の UFS の host 試験が呼ぶ |
| [packages/](tools/packages/) | 外部 package の試験: ライセンス監査、未解決 symbol、取得機構とクロスビルドの host 試験。WS032 から移した | `audit-licenses.sh`、`check-unresolved-symbols.py`、`run-external-host-test.sh`、`run-cross-host-test.sh` |
| [menuconfig-target-host-test.py](tools/menuconfig-target-host-test.py) | menuconfig の target の選択の host 試験。WS020 から移した | `make menuconfig-host-test` |
| [boot-parameter-image-tool.c](tools/boot-parameter-image-tool.c) | image の boot parameter の読み書きと、pc98 の text VRAM の解読（`decode-pc98-vram`）。WS003 から移した | WS005・WS013 の試験が compile して使う |
| [sync.py](tools/sync.py)（[README](tools/README.md)） | GitHub との同期（GitHub mode） | `plan/tools/README.md` |
| sh の試験（[tools/sh](tools/sh/)） | `/bin/sh` を dash と比べる（oils の spec と自前の case）。guest では 40 件ずつ。対話（serial console）と行編集（host の pty） | `build-host-sh.sh`、`sh-diff.py --shell build/ws042/host-sh`（`fetch-oils.sh` で oils を取得）。guest は `--export build/ws042/guest-export` の後 `guest-batches.sh`（中で `guest-diff.sh`）。対話は `sh-interactive.py SOCKET`、行編集は `vi-host.py build/ws042/host-sh`。WS065 から: `build-guest-sh.sh`（この tree の sh を guest の image の libc.so で build）、`guest-batches.sh` の `GUEST_SH=FILE`（guest の copy の /bin/sh を置き換える）、`guest-expat.sh SH`（guest で expat の configure・make・runtests を走らせ configure の生成物の checksum を出す） |
| utility の差分試験（[tools/utils](tools/utils/)） | base の utility を GNU（POSIX mode）と比べる（`cases/` の 484 件、guest へは `--export` と `plan/tools/sh/guest-diff.sh`）。実際の configure（expat・coreutils）を GNU の道具と我々の道具で走らせて生成物を比べる。libc の浮動小数の書式を glibc と比べる | `build-host-utils.sh`、`util-diff.py`、`configure-diff.sh`、`float-format.c`。書き直しの前後の ls の比較は `ls-compare.sh OLD NEW` |
| X11 の回帰（[tools/x11](tools/x11/)） | zdesktop-x11server の上の X11 の app（Venus、`plan/ws035/tests/zdesktop-guest.sh start` の guest）: x11-p003（zterm の rootless の窓、入力、docked）、x11-p004（glxtest の GLX、docked の大きさの変化）、x11-p005（zgears 300 frame、回る、fps）。WS069 から移した | `sh plan/tools/x11/x11-p00N.sh [OUTDIR]`（`GUEST_RUNTIME` の既定は build/ws035-sq-run）。画面を目で確かめる |
| libm の試験（[tools/libm](tools/libm/)、WS076） | libc の libm（`src/libc/math/`）を MPFR（gmpy2）の参照値と比べ、関数ごとの最大・平均の ulp 誤差、正確であるべき結果の不一致、C11 Annex F の特殊な値・errno・例外を出す。host（host の clang、libm を link しない）と guest（amd64、image の libc.so、serial で実行） | `plan/tools/libm/host-test.sh [--count N] [NAME...]`、`plan/tools/libm/guest-test.sh [--count N] [NAME...]`（`BUILD` 既定 `build/ws076-amd64`）。参照の生成は `gen-reference.py OUT.bin`。ブラウザの JS（ws074 の試験と `js/libm.js`）を guest で Chromium と比べる `browser-js.sh`（`js-reference.py --reference` で期待値） |
| 規約の検査（[style-check.py](tools/style-check.py)） | `plan/coding-style.md` のうち機械的に確かめられる規則（条件の中の呼び出し、閉じ括弧の後の空行、段落の comment、入れ子の宣言、条件演算子、goto、前方宣言、comment の形、名前、複数行の本体の括弧） | `python3 plan/tools/style-check.py FILE... [--summary] [--rule NAME]` |
| Browser component（[tools/browser-component](tools/browser-component/README.md)、WS107） | Wayland無し/public headerのみの動的第2client、2view/抽象入力/callback、allocation rollback・async history、標準Vulkan/lavapipeのdraw/record/readback/caller fence/resize/target解放、plain＋ASan/UBSan | `sh plan/tools/browser-component/run.sh [plain|asan]` |
| Keiland native deb（[release driver](../tools/release/keiland-linux-deb/README.md)、WS108） | pinned Debian13/Ubuntu26.04 QEMU native build、fresh guest導入/GUI/input/public Vulkan/upgrade/remove、manifest/buildinfo/checksum | `make keiland-linux-debian` / `make keiland-linux-ubuntu2604` |

QEMU の不具合は log を読まずに、QEMU のデバッグ機能で解析する:

- **gdbstub**: `-S -gdb tcp::<port>` で止めて起動し、host の `gdb` で `target remote :<port>`。`vmunix` は strip されていない。
- **map**: link で作る `$(BUILD)/vmunix.map` で address から関数を引く（`-g` は付けない）。
- **monitor/QMP**: `info registers`・`info mem`・`info tlb`・`x/`・`xp/`・`pmemsave`。
- **trace**: `-d int,cpu_reset,guest_errors -D <file>`（例外と reset だけ）。

pc98 は QEMU の PC-98 fork（`~/qemu-pc98/build/qemu-system-i386`、`-M pc9821,pegc=off,coregraph=on`）で起動し、
`pmemsave 0xa0000 0x2000` で取り出した text VRAM を `boot-parameter-image-tool decode-pc98-vram` で読む。
回帰試験では GPU を使わず、標準 VGA の framebuffer で login prompt だけを確かめる。

guest の memory（2026-09-24 ユーザー決定「ゲストのメモリはamd64とarm64では8GBでテストしましょう」）: amd64 は 8 GiB（`plan/tools/guest/guest.py` の既定と
`boot-test.sh` の `uefi-usb`）。arm64 の QEMU raspi4b は board の model が 2 GiB しか受け付けない（`Invalid RAM size, should be 2 GiB`）ので 2 GiB（2026-09-24 ユーザー決定「raspi4bは2GBでOKです。」）。i386 は変えない。

- WS105 継続 fixture: `plan/tools/keiland-linux/dbus-wire.c` / `dbus-wire.py`（独立 wire / fd 境界、ordinary + ASan/UBSan）、`seat-fd.c`（guest の DRM master / caller fd 所有権）。[手順](tools/keiland-linux/README.md#logind-の-fd-と-d-bus-wire-の独立-fixture)。

## プロジェクト固有の情報

エージェントの守る規則は [AGENTS.md](../AGENTS.md) の「プロジェクトの規則」節にある。ここには計画に要る事実と決定を置く。

### 試験の方針（2026-10-03 ユーザー決定）

細かい修正ごとの回帰試験はやめる。実装をレビューして確信を持ち、WS の最後にまとまった単位で試験の担当 T1 に依頼する。T1 は依頼をまとめて 1 つの QEMU で流し、長くしすぎない。負荷・耐久試験はユーザーに確かめて夜間。規則は [AGENTS.md](../AGENTS.md) の「検証」と [protocol](agents/protocol.md) の「試験の担当 T1」。

### 実機試験の進め方（2026-10-03 ユーザー決定）

2026-10-03 user「えーと、WiFi試験は、実機でやります。zedBSDの安定版を作って、実機で起動し、SSHで接続して試験を行います。1つの安定版に、複数の実機試験を詰め込みます。WiFiに限らずです。さまざまな試験をそこで行い、結果をいくつかのWSで修正して、また別な安定版を作り、そこで実機試験を詰め込みます。」
- 実機の試験は「安定版」ごとにまとめる: 安定版の image を作る → 実機で起動 → SSH で接続して、その版に詰め込んだ複数の実機試験（WiFi に限らない）を行う → 結果の不具合を各 WS で直す → 次の安定版を作り、また実機試験を詰め込む。
- 安定版ごとの実機試験は一つの WS にまとめる（最初は [WS133](ws133/ws.md)）。各 WS の「実機は未実施」の項目は、次の安定版の実機試験の候補として WS133 型の WS に集める。QEMU の仮想 WiFi（zedBSD 用の hwsim 相当）は作らない。

### 対象 platform（2026-09-24 ユーザー決定）

| platform | 位置付け | tick 周期 |
| --- | --- | --- |
| amd64 | **主対象**。デスクトップ・GPU・アプリケーション。fg010 のデモ | 1000 Hz |
| aarch64（rpi4 ほか） | **主対象** | 1000 Hz |
| i386（pcat・pc98） | デモ用のおまけ。基本のコマンドと Xzed が動けばよく、性能は考えない | 100 Hz |
| sparcv9（sun4u）、m68k（x68k） | サポート外。コードは残す | 100 Hz |

tick 周期は `include/hal/arch/<arch>.h` の `HAL_TIMER_FREQUENCY`。時間の計算は `kern_ms_to_ticks()`・`kern_ticks_to_ms()`・
`KERN_MS_TO_TICKS()` で行い、tick の数を数字で書かない（WS040）。

### 有効なユーザーの判断（全体に関わるもの）

2026-09-29 に整理した。WS に固有の判断はその ws.md に移し、記録先に書かれたもの・後の判断で置き換わったもの・完了した WS のものは削除した（git の履歴にある）。

| 項目 | 決定 | 記録先 |
| --- | --- | --- |
| Kei Operating System（2026-09-28） | ユーザー:「プロジェクトの名前は Kei Operating System とします。カーネルの内部名がzedbsdです。デスクトップの名前はKeilandで、Kei + Waylandなのですが、カーネルからデスクトップまでOSとして垂直統合しているので、デスクトップ環境とかデスクトップみたいにあえて呼ばず、内部名がKeilandです。OSの見えるところからzedBSD, zed, zの名前を徐々に外していきます。zdesktopは/bin/wayland, zdesktop-x11serverは/bin/xserver, zdesktop-browserは /bin/browser にします。以前から、シンボル名にzedbsdを含めないように実装してきましたが、カーネル、ドライバ、UAPIなどで誤って新規実装で混入してしまっているようです。これは一斉に改めましょう。ZEDBSD_ではなくKERN_が望ましいプレフィックスです。ロゴなどでKだけだとKDEの商標を侵害してしまう可能性があるので、かならず Kei と3文字にします。Keiは日本語の軽いという意味です。」→ [WS078](ws078/ws.md) |
| 画像の library の置き場（2026-09-28） | ユーザー:「JPEGライブラリは、userland/base/libjpeg-compatにして、共有にしましょう。GIFもそうするのがいいです。include/libc/jpeg/みたいな位置にヘッダがあるのがいいです。」→ libjpeg-compat は base の共有 library（desktop の分類でなく base）。GIF も `userland/base/libgif-compat`（WS074 design D5 の案を採る）。同日の訂正:「include/libc/compat/jpeglib.hの方がいいです。訂正します。」→ header は今の `include/libc/compat/` のまま（GIF も同じ所）。さらに:「PNGもbase/libpng-compatにして、include/libc/compat/png/に入れましょう。」→ libpng-compat も base の共有 library、header は `include/libc/compat/png/`。zlib（main の問い、ユーザーの回答「base・全 platform・header を compat/zlib/ へ」）→ libz-compat も base の共有 library・全 platform、header は `include/libc/compat/zlib/zlib.h`（PNG も全 platform に）。WS074 が実施 |
| retro・GOP・Kei の印（2026-09-28） | ユーザー:「Keiマークはちょうどいいです。GOPフレームバッファは1920x1080を要求して上下左右の不足部分を黒い帯にすればいいかなと思います。X11のプログラムはuserland/X11/にあると思いますので、それはuserland/retro/に入れましょう。zedinstはretro/に入れておいて、Waylandであとで作り直しますが、それはOSCでのデモでは必須ではないので、優先度を下げます。」→ `userland/X11` → `userland/retro`、`userland/base/zedinst` → `userland/retro/zedinst`（main が実施、image の build・boot test・menu の試験 PASS）。UEFI の loader は GOP の 1920x1080 を求め、足りない所は黒い帯。Wayland の installer の作り直しは低い優先度 |
| デモの利用者・KVM の検討の時期（2026-09-28） | ユーザーの回答: デモは**名前のある利用者**でログインする（指定が無いので利用者名 `kei`、表示名「Kei」。root で空の password のログインはデモの image から外す）。WS082（KVM の検討）は**空いた枠で始める** |
| 既定の image と login（2026-09-29） | ユーザー: root の password を root、kei の password を kei、見つかった外部の display → 内蔵の LCD の順、make の既定の hdd-image.img でデスクトップを試せる、既定で自動の graphical login → base の passwd・group・shadow に kei（uid 1000、network）と SHA-512 の password、i915 は display= が無い・auto・hdmi で HDMI を先に探し、edp・panel で内蔵（boot の parser も受ける）、menuconfig の既定を amd64 に、Venus・i915 を amd64 で既定 y、sessiond が `/etc/keiland/autologin`（既定 kei、root は拒む）の利用者を boot で 1 回 login（Log Out の後は greeter）。serial・pc98 の試験の login は password を送る。確認: 既定の config で image の build と boot test PASS、Venus の guest で greeter を経ずに desktop（`build/ws035-shots/default-image-20260929-autologin.png`）。既定の image は font を持たず文字が出ない、guest の harness の SSH の鍵も無い。実機は未実施 |
| デモの image の root（2026-09-28） | root を lock し su も無いと実機で管理の作業ができない件でユーザーの回答:「root に password を設定する」→ `plan/ws035/demo/demo-accounts.sh` が root に password を付ける（`DEMO_ROOT_PASSWORD`、無ければ 12 文字の乱数。`BUILD/demo-accounts/root-password`（0600、git に入れない）に書く。image には SHA-512 crypt だけ）。空の password の root は引き続き無し。kei は password 無しのまま |
| 表示の build の既定 | ユーザー:「GPUの問題は解決したとみなして、以後はロゴを出してメッセージを隠すビルドにしましょう。ふたたびGPUドライバの修正をするとき、ロゴを無効にしましょう。」→ demo・実機の image は既定（logo と `kmsg=quiet`、`plan/ws075/demo/build-demo-image.sh BUILD`）。GPU の driver を直す Phase だけ `ZEDBSD_GRAPHICAL_BOOT=n "ZEDBSD_BOOT_EXTRA_LINES=display=edp login=graphical"`（logo を消し kernel の message を画面に残す） | Guardrail、WS084 |
| LCD のみの構成 | ユーザー:「HDMIはいったんやめて、LCDのみの構成にします。」→ demo の既定 `display=edp`。HDMI の LCD は WS075 に戻す | WS084、WS075 |
| 文字の符号（2026-09-29） | ユーザー:「我々のOSはutf-8のみをサポートしており、Escapeは不要と思います。LANG=CをUtf-8と解釈するのが乱暴というなら、C.UTF-8を設定するのでもいいです。」→ ls は locale に関わらず名前を UTF-8 として扱い、表示できる UTF-8 の文字を escape しない（escape は制御文字と不正な byte だけ）。幅は UTF-8 の表示幅 | WS086 |
| デモに必須の追加（2026-09-29 夜） | ユーザーの回答: 新しい要望のうちデモ（10/17）に必須は「画像 viewer と text editor」（WS091・WS092、Files からの起動 WS093 を含む）。WS090（widget の library）・WS094（desktop の icon）・WS095（IME）はデモに必須ではない。窓の縁の resize は四隅に加えて辺も入れた（ws035-p128、設計の「枠と角＝resize」どおり） | WS091〜WS093、WS035 |
| デモまでの進め方（2026-09-29 夜） | ユーザー:「実は、すでにデモに耐えられるだけの完成度にはなっています。…いちおう、当日までOSCでのデモという目標は掲げたままにします。まだ当日まで時間があるので、新規実装をどんどん行って、デモの1週間前くらいから、バグ修正とデモ実機での調整のみの期間に入ろうかなと思っています。」→ fg010 は保つ。**2026-10-10 ごろまでは新規実装**（デモに必須でない WS090・WS094・WS095・WS080 等も進めてよい）、**2026-10-10 ごろ〜10-17 は bug の修正と実機（5330）での調整だけ**（新しい機能は入れない） | WS の優先順位 |
| 文字の編集の touch（2026-09-29 夜） | ユーザー:「スクロールは2本指にするのと、共通部品にしましょう。」（Text Editor の touch の選択について）→ 文字を編集する view（Text Editor・text field）では 1 本指の drag を選択、scroll を 2 本指にし、WS090 の共通の部品（libkeiui）で作って Text Editor へ入れる。Files・Image Viewer などの 1 本指の pan は変えない（main の解釈） | WS090、WS092 |
| compositor の速さとすりガラス（2026-09-30） | ws075-p023 の実機の計測（10 app）: すりガラスを切っても compositor の 1 run は縮まず（8.8 → 9.2・8.9 ms）、分岐の中の ALU を飛ぶと 4.9 ms の見込み。ユーザー:「分岐の中の計算を飛ばす、にしますので記録しておいてください。」→ すりガラスは残し、WS075 p023 で panel.frag の分岐の中の ALU を飛ぶ実装をする。WS035 p135（暗い壁紙の上の glass の文字）はすりガラスを残す前提で再開できる | WS075、WS035 |
| ファイルピッカー（2026-09-29 夜） | ユーザー:「テキストエディタのファイルピッカーは、KeiのUIライブラリに入れるのがいいと思いました。」→ Open・Save As の chooser を app ごとに持たず、共有の library（今は libkeiland、WS090 の最初の部品）に置く。作るのは WS092 のエージェント、WS091（画像 viewer）・WS089（Settings）・Notes・PDF Viewer が順に使う | WS090、WS092、WS091 |
| サブエージェントの運用（2026-09-29 夕） | ユーザー:「サブエージェントを使って作業します。N=6で、6エージェントを起動します。メインエージェントであるあなたは、サブエージェントに依頼して、結果を受け取ってマージする、プランナーです。サブエージェントは、5時間の利用制限に到達したときに強制終了されてしまうので、そのときに作業内容が失われます。そこで、こまめにメインエージェントに依頼して、マージを行います。また、強制終了した場合もサルベージ可能なように、作業ディレクトリを構成します。5時間制限の残り時間と使用率から、N=0からN=6の間で調整していきます。サブエージェントにはラップアップを依頼することで、キリのいいところで終了が可能です。」→ worktree は固定の path と branch（`.claude/worktrees/wsNNN-<名前>`・`wt/wsNNN`、前の枠の 2 つは元の path）、build が通るたびに WIP commit、1 回の依頼は 1 Phase、終わるたびに main が merge して同じエージェントに続きを依頼、強制終了は branch と未 commit の差分から回収 | queue.md |
| サブエージェントの effort（2026-09-29 夜） | ユーザー:「いくつかのエージェントは、Opus 5.5のMidで動かすように、エージェント設定を変更したいです。明示的にHighのままにしたいのは、i915、Keilandデスクトップ (WS035）、カーネル、バグフィックス、あたりです。そのほかは、特にブラウザは、Midにしたいです。これは試行回数が大きいですからね。」→ `.claude/agents/phase-runner-mid.md`（effort: medium）を足した。High（`phase-runner`）: i915（WS075）・kernel・bug の修正（Keiland の bug を除く）。Mid（`phase-runner-mid`）: Keiland のデスクトップ（WS035、その bug を含む。2026-09-29 ユーザー「KeilandのサブエージェントもMidにします。」）・touch（WS081、同日「WS081もMidにします。」）・ブラウザ（WS074）・アプリ（WS089・WS091・WS092）・IME（WS095）ほか。走っているエージェントは次の Phase の区切りで Mid の新しいエージェントに引き継ぐ | queue.md |
| toolchain の保護と subagent の範囲（2026-09-28） | ユーザー:「再発防止のため、ツールチェインはメインエージェントの許可がないと変更できないようにしましょう。また、サブエージェントの修正可能範囲を明示しましょう。」→ AGENTS.md の「禁止と承認」に 2 つの規則、`plan/tools/toolchain-lock.sh`（共有の toolchain の tree の directory を読み取り専用に、main だけが一時的に unlock）。経緯: 15:55〜15:57 に古い Makefile の worktree の build が共有の `build/llvm-source` に clang・libcxx の package の patch 5 つ（41 file）を当てた。main が patch -R で戻し、manifest の全 file の SHA-256 と file の一覧の一致を確認（BUG-096） |
| 試験の範囲（2026-09-27 14 時半） | ユーザー:「試験はamd64のみにしましょう。phase内ではビルドが通れば先に進み、phaseの最後にテストしましょう。」→ 試験は amd64 だけ（pcat・pc98・rpi4 は走らせない）。Phase の途中は build が通れば進み、試験（guest の試験・回帰・boot test）は Phase の最後に 1 回。main の merge の後の検証も amd64（デスクトップの image と boot test）だけ | 検証、AGENTS.md の回帰の範囲の例外 |
| 古いグラフィックの試験の driver（2026-09-28） | ユーザー:「venus-backend-testのような初期のテストドライバは、もう使わなくてOKです。グラフィック関連の古いテストドライバは捨てて、回帰テストは不要です。デスクトップ環境が起動しているからです。どうしても特定の機能をテストしたいときは、そのときにテストを書いてください。i915はまだexecutorを実装する必要があるので、テストドライバは残していいです。」→ 初期のグラフィックの試験の driver（venus-backend-test 等）は削除してよく、回帰の対象から外す。デスクトップの起動が回帰の代わり。特定の機能は必要なときに試験を書く。**i915 の executor の試験の driver（vkx・vke1・vke2・vkc 等）は残す** | 検証、WS030・WS031・WS075 |

### 主な依存関係

- WS046（make）→ guest での expat の build（WS042 の残り）。
- ws035-p051（承認）→ p052〜p055・p057（合成）→ fg010。
- WS014・WS031（GPU の土台）→ WS035 の合成とアプリ。
- WS049（AML）→ WS050（UCSI）→ WS051（DP Alt Mode、i915 の display も要る）。WS049 → WS052（S0i3）。
- WS036 p026（AArch64 の LLVM target）→ aarch64 の userland と package。

### 参照資料

- [設計方針・決定の参照資料](master-design-policy.md): 独立実装・ライセンス境界、module の設計、toolchain、個別の設計判断。
- [コーディング規約](coding-style.md)、[Guardrail](guardrail.md)、[Awesome Plan の設定](config.md)。

WS105 p005 の追加道具: [display-probe.c](tools/keiland-linux/display-probe.c)（guest 専用、seat fd / direct の3色・oldSwapchain・CRTC復元）。[使い方](tools/keiland-linux/README.md)。

WS105 KMSの再開検証: [flip-delay.c](tools/keiland-linux/flip-delay.c)（test-only、単発poll timeoutの後の実eventを検証）。

WS105 p010 の追加道具: [network-probe.c](tools/keiland-linux/network-probe.c)・[audio-probe.c](tools/keiland-linux/audio-probe.c)（production library / userkei の実WiFi・ALSA）、[wifi-setup.sh](tools/keiland-linux/wifi-setup.sh)（disposable guestのhwsim AP、192.0.2.2の試験専用IP）。
