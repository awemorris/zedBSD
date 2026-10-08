<!-- awesome-plan project=zedbsd record=ws089 -->

# WS089: 設定のアプリ（Settings）

<!-- awesome-plan-current:start -->
Status: incomplete（2026-10-08 Q1 の判定（sweep-beta2-rc2 §2）: p012・p013・p019 は cleared、p014 は 5330 の Wi-Fi の driver 次第で保留のまま。2026-10-08 q910 P2 の照合: p012（T1-238 PASS）・p013（T1-278 PASS）は Q1 の判定待ち、p019 は p012 の q805 の compositor の直しで済み（Q1 の判定）、p015 は表に合わせて canceled（WS158、ベータ3）。残りは p011・p014 の実機（p014 は 5330 の Wi-Fi の driver が要る）、p018 は規約（ベータ3））
Primary Milestone: MG006
Related Milestones: MG002
Objectives: O2
Parent: [Master](../master.md)
Focused goal: fg019（ベータ1、2026-10-17）
Queue: なし
Resume point（2026-10-02 ベータ1の計画）: 2026-10-02 user「Settingsも重点的に」で、2026-09-29 の「ブラッシュアップは後回し」を置き換えて再開する。**最初は p010（現在の main での回帰・S7・棚卸しと候補、source は変えない）**、並行して p012（Settings の中だけで済む操作性）。p011 は 5330 の passthrough（i915 の lock）。p013〜p017 はユーザーの選択・他の成果待ちの planning。最後は p018。下の「ベータ1 の到達目標」。
過去の resume（2026-09-29）: p001〜p009 は cleared（QEMU）。完了の処理は main の判断。完了の後に WS090 p007（Settings の libkeiui への移行）。試験は `plan/ws089/tests/`（`settings-regress.sh`）。壁紙の生成は `userland/desktop/wallpapers/generate.py`。
<!-- awesome-plan-current:end -->
作業の手引き（2026-10-01）: [guide.md](guide.md)

## 目標（2026-09-29 ユーザー）

「これもOSCデモで使う優先事項にしたいですが、設定画面のアプリを作ってほしいです。添付がイメージです。あくまでもイメージなので、この通りでなくていいです。
左側に項目のペイン、右側に設定項目。フローティングでセパレート。」

- 見本: [mockup-network.webp](mockup-network.webp)（Network の頁: 左に項目の一覧（Wi-Fi・Ethernet・Bluetooth・VPN・Network・Appearance・Wallpaper・
  Notifications・Sound・Display・Storage・Battery・Keyboard・Mouse・Touchpad・Printers・Sharing・Users・Privacy・Security・Accessibility・
  Updates・About）、上に戻る・進む・Home・breadcrumb・検索・表示の切り替え、右に card の群（接続の状態、Wi-Fi の一覧、Ethernet、VPN、
  DNS と Proxy、通信量の graph、初期化の button））。見本の通りでなくてよい。
- 形: 左の項目の pane と右の設定の pane を、それぞれ浮いた（floating）すりガラスの pane として分ける（Files（WS071）の付箋の pane と同じ作り）。
  窓は Keiland の浮いたタイトルバー（System Menu・CONTROLS）。名前の規則: 画面の文字は「Kei」、Keiland・libkeiland は出さない。
- デモの受け入れ（案、p001 で確定）: 起動して項目を選ぶと頁が替わり、少なくとも About（OS の名前・版・機械）、Network（networkd の実際の状態:
  interface・address・Wi-Fi の一覧と接続）、Display（解像度・拡大）、Appearance・Wallpaper（見た目と壁紙の変更が desktop に反映）、Sound（音量、
  audiod）、Mouse・Touchpad（速さ、慣性の scroll の on/off）が実際に働く。それ以外の項目は頁の枠と「準備中」の表示でよい。App Home から起動できる。
- **受け入れ（2026-09-29 確定）**: ユーザーの回答「設定項目は、ネットワークを中心にしてください。ディスプレイはまだスタブでいいです。」と
  main の判断による。
  1. 起動して項目を選ぶと頁が替わる（左の項目の pane と右の頁の pane、戻る・進む・breadcrumb）。App Home から起動できる。
  2. **Network が中心**: 接続の状態、Wi-Fi の一覧と接続・切断（**新しい network への鍵の入力を含む**）、Wi-Fi の入り切り、Ethernet、
     address・netmask・MAC・DNS、通信量（見本の Network の頁に近い内容）。networkd の protocol に最小の追加が要るなら
     [proposed/](proposed/) に案を置いてから実装する。
  3. About（OS の名前・版・機械）。
  4. Display は stub（読むだけの情報、または準備中）。accent の色と Touchpad は出さない（準備中）。
  5. Appearance・Wallpaper・Sound・Mouse・Keyboard は Network の後の優先度（p004・p005・p007）。デモの受け入れの必須は 1〜4。
  6. その他の項目は頁の枠と「準備中」。

## Phase

設計は [design.md](design.md)。他の WS の source への変更の案は [proposed/](proposed/)（main の許可が要る）。

| Phase | 目的 | Status | 依存 |
| --- | --- | --- | --- |
| [ws089-p001](phase001/phase.md) | 設計: 項目の一覧とデモで働かせる範囲、画面の構成（Files の pane・card の部品の再利用）、各項目の backend（networkd・audiod・sessiond・/dev/system・compositor の設定）との接続の方法、設定の保存先 | cleared（2026-09-29） | — |
| [ws089-p002](phase002/phase.md) | アプリの骨格: 窓、present、glass の 2 枚の card、titlebar（Back・Forward・Home・Breadcrumb・Sidebar）と履歴、System Menu、左の項目の pane、右の頁の scroll、Home（tile）・About・準備中の頁、App Home の行 | cleared（2026-09-29、Venus の guest） | p001、link の規則（main の許可済み） |
| [ws089-p008](phase008/phase.md) | 検索（titlebar の欄と結果の頁）と Home の tile の今の状態。App Home の歯車の絵（許可済み D4）も同時に | cleared（2026-09-29、Venus の guest。実機は未実施） | p002 |
| [ws089-p007](phase007/phase.md) | desktop の設定の仕組み: libkeiland の `keiland_preferences_*`（KEILAND_VERSION 13）と zdesktop の反映（[案](proposed/desktop-preferences.md)、許可済み D4） | cleared（2026-09-29、Venus の guest。実機は未実施） | p001、main の許可 |
| [ws089-p003](phase003/phase.md) | Network・Wi-Fi・Ethernet の頁（networkd の状態、Wi-Fi の一覧・接続・切断・新しい network の鍵、address・DNS・通信量。[案](proposed/libkeiland-network-link.md)、networkd の protocol は変えない） | cleared（2026-09-29、Venus の guest。実機は未実施） | p002 |
| [ws089-p004](phase004/phase.md) | Appearance・Wallpaper・Display・Storage の頁 | cleared（2026-09-29、Venus の guest。実機は未実施） | p002, p007 |
| [ws089-p005](phase005/phase.md) | Mouse・Keyboard の頁と Sound（audiod の有無の表示だけ、main の依頼）。Touchpad は準備中のまま | cleared（2026-09-29、Venus の guest。実機は未実施） | p002, p007 |
| [ws089-p009](phase009/phase.md) | 生成の壁紙（5 枚、1920x1080、`userland/desktop/wallpapers/generate.py`）を build の時に作り、デモの image に同梱（2026-09-29 ユーザーの D8 の判断、main の許可） | cleared（2026-09-29、Venus の guest。デモの image は `make -n`。実機は未実施） | p004 |
| [ws089-p006](phase006/phase.md) | 規約の全文との照合、回帰、デモの通し（App Home の絵は p008、デモの image の壁紙は p009） | cleared（2026-09-29、Venus の guest。実機は未実施） | p003〜p005, p008 |
| [ws089-p010](phase010/phase.md) | 現在の main（KEILAND_VERSION 20 以後）での回帰の取り直し・S7（QEMU）・頁ごとの通しと不具合の表・ブラッシュアップの候補の一覧（ユーザーが選ぶ） | cleared（2026-10-03、q617-i01、QEMU の Venus。regress 8 本・volume-p005・host・boot test PASS、重い・中の不具合 0、候補は [beta1-candidates.md](beta1-candidates.md)） | — |
| [ws089-p011](phase011/phase.md) | 5330 の passthrough で S7（壁紙・透明度・検索）、透明度 85% の frame の率 | planned（2h、`/tmp/i915-hw.lock` が空くこと。2026-10-06 q805 は実機の Phase なので skip） | p010 |
| [ws089-p012](phase012/phase.md) | Settings の中だけで済む操作性: 検索の結果の上下の key と Enter、頁の pane の touch の drag の scroll、左の pane の key の移動の見直し（q619 で C1・C4 を追加） | cleared（2026-10-08 Q1 の判定（sweep-beta2-rc2 §2）: T1-238 PASS。旧: test-wait（2026-10-06 q805 P2: 欄の Down を compositor で直した、T1-238。以前: unc…） | —（p010 と並列可、source は p012 だけが変える） |
| [ws089-p013](phase013/phase.md) | About の memory と Storage の使用量（[proposed/libkeiland-system.md](proposed/libkeiland-system.md)、libkeiland の追加） | cleared（2026-10-08 Q1 の判定（sweep-beta2-rc2 §2）: T1-278 (1) PASS、About に Memory の行。旧: in-progress（2026-10-06 q821 P2: 既存の kl_system_monitor で About の Memory…） | p010 |
| [ws089-p014](phase014/phase.md) | Network の頁を実機の Wi-Fi（5330）で: 一覧・接続・鍵・切断 | planning（WiFi の driver の WS（BUG-134 ほか）の成果が要る） | ネットワークの WS、実機 |
| [ws089-p015](phase015/phase.md) | 日本語の UI（WS127 p005 と共通の仕組み） | canceled（2026-10-05 夜、WS158 に吸収） | p010、WS127 p005 と仕組みを共有 |
| [ws089-p016](phase016/phase.md) | 単一の instance（二つ目の起動で既存の窓を前に） | cleared（2026-10-05 Q1、T1-185 PASS） | p010、compositor の Phase（WS099 と直列） |
| [ws089-p017](phase017/phase.md) | accent の色・dark の外観（D2） | cleared（2026-10-05 Q1、p017a T1-195・p017b T1-198b PASS） | p010 |
| [ws089-p018](phase018/phase.md) | 全文規約と回帰（WS の最後）、完了の処理の準備 | planning（最後） | 選んだ実装の Phase |
| [ws089-p020](phase020/phase.md) | BUG-152: Wallpaper の頁の縮小表示を別の thread で読み、頁を先に出す | cleared（2026-10-03 Q1、T1-011） | — |
| [ws089-p021](phase021/phase.md) | Wi-Fi の画面の自動の scan（Scan のボタンを無くす、compositor が要求を数えて libkeiland-backend 経由で networkd に on・off）と Disconnect の icon（2026-10-04 ユーザーの要望）、BUG-184 | cleared（2026-10-05、T1-109・T1-098 PASS） | WS131・WS005、q700 の Wi-Fi の Bug と同じ担当 |
| [ws089-p022](phase022/phase.md) | Ethernet の頁で設定の取得・変更（IPv4 DHCP/Static・Address・Mask・Router、IPv6 Auto/Static・Address・Router、MTU、DNS 1/2、MAC などは読むだけ）。compositor 経由、libkeiland-backend が net の command で設定（2026-10-04 ユーザーの要望） | cleared（2026-10-05 Q1） | WS131・WS005・WS033、IPv6 は WS130 |
| [ws089-p023](phase023/phase.md) | Storage の頁: 解析の button で folder の階層ごとの使用量を multi-thread で解析し逐次に表示、Stop で止める。Trash を空にする（2026-10-04 ユーザーの要望） | cleared（2026-10-05 Q1、T1-159） | WS127（Trash） |
| [ws089-p024](phase024/phase.md) | Mouse の頁を device ごと（マウス・タッチパッド）の設定に、pointer の加速、既定 base 150%・加速 強め、自然な方向のスクロールはタッチパッド ON・マウス OFF（2026-10-04 ユーザーの要望） | cleared（2026-10-05 Q1） | compositor の入力、WS135 |
| [ws089-p025](phase025/phase.md) | Sharing の頁に SSHD の ON/OFF（後でクラウドストレージの設定もここに、WS146）（2026-10-04 ユーザーの要望） | cleared（2026-10-05 Q1） | WS002（service） |
| [ws089-p026](phase026/phase.md) | Users の頁の実装（一覧・自分の password・管理者の利用者の追加・削除・group）（2026-10-04 ユーザー） | cleared（2026-10-05 Q1、T1-183b PASS。Manage users の UI は UAT） | WS131、account の userland |
| [ws089-p027](phase027/phase.md) | About に版の名前（`/etc/os-release` の PRETTY_NAME）を出す | cleared（2026-10-05、T1-108 PASS） | ws129-p003 |


## ベータ1 の到達目標（2026-10-02 計画、fg019）

| # | 受け入れ | 測り方 | Phase |
| --- | --- | --- | --- |
| S-B1 | 最終の image で `settings-regress.sh`（8 本）・`volume-p005.sh`・host の試験が全て PASS | QEMU の Venus | p010（基準）、p018 |
| S-B2 | S7（壁紙の差し替え・透明度・検索）が 5330 の passthrough で通り（画面）、透明度 85% の frame の率を 100% と比べて記録 | `settings-s7-hw.sh` | p011 |
| S-B3 | p010 の通しで見つけた不具合の重い・中が 0（直したか、ユーザーが非阻害を決めた） | p010 の不具合の表 | p012 か新しい Phase |
| S-B4 | 検索の結果を key だけで選んで開ける、頁の pane を touch の drag で scroll できる | guest の手順 | p012 |
| S-B5 | p010 でユーザーが選んだ追加の項目（p013〜p017 の中）が全て実装・試験 PASS | 各 Phase | p013〜p017 |
| S-B6 | 変えた source の全文規約、boot test | — | p018 |

Display の頁（複数 display）は WS113 p006 が持つ（WS089 では作らない）。

source の衝突: WS089 の Phase は `userland/desktop/settings/` を変える（互いに直列）。Settings は Files の `canvas.c`・`text.c`・`icons.c`・`artwork/mark.c` を source で共有 → WS127 の Phase と同時にこれらを変えない。
WS113 p006（Display の頁）・WS099 p019（壁紙の一覧）と `settings/page-look.c`・`look.c` が重なりうる。p013 は libkeiland（KEILAND_VERSION）を上げる → WS113 p005 と直列。p016 は compositor → WS099 と直列。
**WS090 p007（Settings の libkeiui への移行）と WS089 の Phase は重ねない**（ベータ1 の前に移行するかはユーザーの判断）。

未決の判断（ユーザー）: (1) p010 の候補からベータ1 に入れる項目、(2) 日本語の UI（p015、WS127 p005 と同じ判断）、(3) accent・dark（p017）、(4) WS090 p007 の移行をベータ1 の前か後か（計画エージェントの案: 後）。

## ユーザーの判断（2026-09-29 に master から移した）

| 項目 | 決定 | 記録先 |
| --- | --- | --- |
| 設定のアプリ（2026-09-29） | ユーザー:「これもOSCデモで使う優先事項にしたいですが、設定画面のアプリを作ってほしいです。添付がイメージです。あくまでもイメージなので、この通りでなくていいです。左側に項目のペイン、右側に設定項目。フローティングでセパレート。」 | [WS089](ws089/ws.md) |
| 設定のアプリの範囲（2026-09-29 夜） | ws089-p001 の問い（Display の拡大は表示だけ・accent の色は出さない・Touchpad は準備中）にユーザー:「設定項目は、ネットワークを中心にしてください。ディスプレイはまだスタブでいいです。」→ Network（Wi-Fi・Ethernet・状態・新しい Wi-Fi への鍵の入力を含む）を中心に作り込む。Display はスタブ、accent と Touchpad は出さない（準備中） | WS089 |

## 後回しの候補（2026-09-29 ユーザーの指示「Settingsはある程度動いたらブラッシュアップは後回しにします。」）

新しい WS か、この WS の再開で扱う。どれも未着手。

| 候補 | 内容 | 要るもの |
| --- | --- | --- |
| Touchpad | 頁（速さ・tap・慣性の scroll） | kernel の touchpad の driver（D9） |
| 音量 | Sound の頁の出力の音量・mute（今は audiod の有無の表示だけ） | [proposed/libkeiland-audio.md](proposed/libkeiland-audio.md)（libkeiland の追加）、HDA の driver、デモの image の audiod |
| Display の変更 | 解像度・拡大の変更（今は読むだけ） | zdesktop の出力の scale（D1） |
| accent の色・dark | Appearance の色の選択 | zdesktop と各 app の固定の色を preferences から読む（D2） |
| 暗い壁紙の上の文字 | Aurora・Twilight の上ですりガラスの上の文字の contrast が下がる | zdesktop の glass の tint（main が WS035 に回した） |
| About の memory | memory の行 | [proposed/libkeiland-system.md](proposed/libkeiland-system.md) |
| 準備中の頁 | Bluetooth・VPN・Notifications・Battery・Printers・Sharing・Users・Privacy・Security・Accessibility・Updates | 各機能の backend |
| 通信量の履歴 | 24 時間の graph（今は窓を開いてから） | daemon の記録 |
| 検索の key 操作 | 結果の上下の選択 | settings の中だけ |
| touch の drag の scroll | 頁の pane の drag（`keiland_scroller`） | settings の中だけ |
| 単一の instance | 二つ目の起動で既存の窓を前に | settings の中だけ |
| 日本語の UI | files の F-041 と一緒に | text の翻訳の仕組み |

## WS の完了の処理に要ること（main の判断）

- 受け入れ（上の「受け入れ（2026-09-29 確定）」1〜6）は QEMU で満たした。実機の確認は未実施（main の判断で実機の確認に回す）。
- 試験の移し先の候補（`plan/tools/settings/` として Tools 節に登録）: `settings-regress.sh`・`settings-wait.sh`・`settings-guest.sh`・
  `build-settings-image.sh`・`config-amd64-settings.mk`・`settings-p002.sh`〜`p009.sh`（名前を役割の名前に変える）・`host-build.sh`・
  `host-render.c`・`host-network.c`・`host-preferences.c`・`host-preferences.sh`。network-probe（WS035 の試験 program）の stand-in の振る舞いは
  `settings-p003.sh` が使う。
- proposed の整理: desktop-preferences・libkeiland-network-link・vmunix-link・app-home-icon は適用済み、libkeiland-audio・libkeiland-system は
  未適用（後回しの候補）。
- 完了の後、Settings を libkeiui に移す作業（WS090 の p007）が始められる。settings は files の canvas・text・icons を source で共有して
  いる（`userland/desktop/files/canvas.c`・`text.c`・`icons.c` と `artwork/mark.c`、Makefile と host-build.sh）。

## 後続のDisplay機能 / 2026-10-02

Event ws113-multidisplay-plan-20261002-ws089-followup: userは読み取り専用のDisplay stubを、外部display/全拡張・全mirror/drag配置ができる[WS113](../ws113/ws.md)で後日実装するよう指定。Settingsはlibkeilandの公開APIだけからcompositor拡張へ接続する。WS089の過去のNetwork中心/stub受け入れとp001〜p009の結果はそのまま保持。新WSはplanned、WS089の未実行Phase/Queueを自動開始しない。GitHub comment/body反映保留。

2026-10-02 / ws089-beta1-plan: user「Settingsも重点的に」（2026-10-02）で 2026-09-29 の後回しを置き換え、fg019 の計画エージェントが到達目標 S-B1〜S-B6 と p010〜p018 を追加（guide の提案 p010・p011 を正式化、p012 の完了処理の案は p018 の後に Q1）。p001〜p009 の結果は不変。Queue は未投入。

- 2026-10-05 WS138 の結果（Q1）: 背景の頁は PNG と JPEG を一覧（png→jpg→jpeg の規則）、生成の背景は `.png`。Settings は wallpaper.c と compat の library を link する（WS168 の後は preview の command へ移る）。ws089 の host build はそれらを compile する。

## 2026-10-06 UAT のフィードバック

- BUG-214 透明度の slider の初期値（100% と表示、実際は frosted。BUG-171 の決定 B との食い違い、ユーザーに確かめ中）、BUG-213 Network の表示
- WS158 p004 の Settings の Languages の頁は途中（account-admin の system-language は merge 済み）
