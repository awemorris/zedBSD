<!-- awesome-plan project=zedbsd record=ws089-beta1-candidates -->

# WS089 ベータ1 の改善の候補（ws089-p010、2026-10-03）

[ws089-p010](phase010/phase.md) の回帰と 23 頁の通し（QEMU の Venus、1280x800 と 1920x1080、host の描画）の気づきと、[ws.md](ws.md) の
「後回しの候補」の表をまとめた。**採否はユーザーが選ぶ**（Q1 が聞く）。この一覧は計画であり、実装の許可ではない。

- 決まっていること: 日本語の UI（p015）と accent の色・dark の外観（p017）は**ベータ1 に入れない**（[F-068](../future-work.md)、2026-10-02 user）。
  Display の変更（解像度・拡大・複数 display）は [WS113](../ws113/ws.md) p006 が持つ。
- 推奨の記号: **入** = ベータ1 に入れる推奨、**任** = 時間があれば（どちらでも）、**外** = ベータ1 に入れない推奨。
- 目安はエージェントの時間（host の試験と QEMU の Venus の確認を含む）。
- 衝突: WS089 の Phase は `userland/desktop/settings/` を変え、互いに直列。Settings は Files の `canvas.c`・`text.c`・`icons.c`・`artwork/mark.c` を
  source で共有（WS127 と同時に変えない）。WS090 p007（libkeiui への移行）とは重ねない。

## 1. 不具合（p010 の通しで見つけたもの）

重い・中は 0（下の表は全て軽い）。S-B3（重い・中が 0）は満たしている。

| # | 頁 | 内容 | 重さ | 直し方（候補の番号） |
| --- | --- | --- | --- | --- |
| D1 | Wi-Fi・Network | 一覧を出している間、Settings は 20 秒ごとに scan を出す（`network.c` の `NETWORK_SCAN_MS`）。scan の答えを待つ間に switch・Disconnect・network の行・鍵の Join を押すと「Wait for the network to answer, then try again.」で**押した操作が捨てられる**。QEMU の stand-in の scan は一瞬なので試験では出ないが、実 radio（AX211 の scan は数秒）では押しても効かないことがある。system bar の menu は同じ問題を ws005-p019 で待ちの slot で直した（BUG-138）。Settings は未対応 | 軽い（実機では中になりうる） | C1 |
| D2 | Wi-Fi・Network・Home | `network` group の外の利用者では networkd の socket が EACCES で拒むが、Settings は「The network service is not running.」と出す（libkeiland の `reachable=0` に理由が無い）。ws005-p019 の規則（WiFi の制御は network group）と食い違う案内。デモの利用者 kei は group に入っているので見えない | 軽い | C2 |
| D3 | 左の項目の pane | 既定の窓（1180x690、1920x1080 でも同じ大きさ）では 25 行の一覧が入りきらず、Users 以下（1280x800）・Updates 以下（1920x1080）は scroll しないと見えない。scroll の手がかり（bar・影）が無い | 軽い | C3 |
| D4 | Sound | audiod は動くが出力の device が無いとき、slider は 100% を出したまま触れる見た目（色だけ薄い）。Home の tile は「Sound service running」、頁は「Running, no sound output」で言い方が違う | 軽い | C4 |
| D5 | Network | Network Activity の card の説明が「Every interface's traffic while this window is open.」で、合計の下は「Since the computer started」（合計は起動からの値、graph は窓を開いてから）。一つの card で二つの期間が説明なしに混ざる | 軽い | C4 |
| D6 | 試験 | `settings-regress.sh` の 1 回目の p002 が `zdesktop: ERROR lines` で FAIL、流し直しで PASS。ERROR の行は次の試験が log を上書きして不明（Settings の失敗か compositor かも不明）。試験は今後 ERROR の行を log に残す（p010 で直した） | 軽い（試験） | 再び出たら行を見て ticket |

（参考: 1 回目の回帰の全体の FAIL（p006 ほか）は、前の世代の P1 の試験が同じ runtime の同じ guest で同時に走っていたためで、Settings の不具合ではない。
phase.md の結果に経緯。）

## 2. 候補

| ID | 項目 | 価値（デモ・ベータ1 の利用者） | 目安 | 危険 | 依存・判断 | 触る file（衝突） | 推奨 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| C1 | Wi-Fi の操作の待ちの slot（D1）: scan の答えの待ちの間に押した操作を一つ預かり、答えの後に送る（system bar の ws005-p019 と同じ形）。scan は預からない | 実機（5330）で on/off・接続が「押しても効かない」を防ぐ。BUG-138 と同じ種類の不満を Settings で再発させない | 1h | 低 | なし（Settings の中だけ） | `settings/network.c`（p012 と直列） | **入** |
| C2 | network group の外の案内（D2）: networkd の socket が拒んだ（EACCES）ことを Settings が自分で見分け（`getgroups`・`getgrnam("network")` か接続の errno）、「This account may not control the network. Ask an administrator to add it to the network group.」を出す | 権限の規則（2026-10-02）と画面の案内が合う | 1h | 低 | libkeiland の API を変えずに済む形（Settings だけ）。libkeiland に理由を足す形なら main の許可（KEILAND_VERSION） | `settings/network.c`・`page-network.c` | 任 |
| C3 | 窓の既定の大きさを output に合わせる（D3）: 高さを output の 85% まで（一覧が入る大きさ）、左の pane に scroll の手がかり | 1920x1080・5330 の 1920x1200 で全項目が一度に見える | 1h | 低 | なし。窓の初期の大きさは compositor の bounds の範囲 | `settings/main.c`・`window.c`・`ui.c` | 任 |
| C4 | 文言と無効の見た目の整え（D4・D5）: device の無い Sound の slider を無効の見た目に、Home の tile と頁の語を揃える、Network Activity の期間の説明 | 細部の仕上げ（デモで目に入る） | 0.5h | 低 | なし | `settings/page-input.c`・`page-home.c`・`page-network.c` | 入 |
| C5 | 検索の結果の key 操作・頁の touch の drag の scroll・左の pane の key（= [p012](phase012/phase.md)） | key だけ・touch だけで使える（S-B4） | 2h | 低 | なし（planned） | `settings/search.c`・`ui.c`・`pages.c`・`page-*.c` | **入** |
| C6 | About の memory（= [p013](phase013/phase.md)）。Storage の volume ごとの使用量は既に出ている（2026-10-08 ws188-p002 から desktop が読む: `kl_system_machine_filesystems`）（p010 で確認: `/` の使用量の bar）ので、p013 は memory の行だけに縮められる | About の情報が揃う（見本の項目） | 1.5h | 低〜中（libkeiland の API の追加と KEILAND_VERSION） | main の許可（[proposed/libkeiland-system.md](proposed/libkeiland-system.md)）、WS113 p005 と KEILAND_VERSION で直列。一般の利用者が `/dev/system` の vm の統計を読めるかは未確認 | `libkeiland/`（新 `system.c`）・`keiland/keiland.h`・`settings/machine.c`（旧 about.c、ws188-p002）・`page-about.c` | 任 |
| C7 | 単一の instance（= [p016](phase016/phase.md)）: 二つ目の起動で既存の窓を前に、指定の頁を開く | App Home を二度押しても窓が増えない | 2h（Settings の UNIX socket の形）／3h 以上（xdg-activation） | 中（前に出すには compositor の activation が要る） | compositor（P2 が変更中、WS099 と直列）。Settings だけの socket の形でも「前に出す」は compositor 次第 | `settings/main.c`、`wayland/`（衝突: P2） | 外 |
| C8 | 保存した Wi-Fi の「忘れる」（Forget）: 保存済みの network の一覧と削除 | 誤った鍵・不要な AP の整理（今は鍵の上書きだけ） | 2h | 中（libkeiland に削除の API、networkd への PROFILES の通知） | main の許可（libkeiland の API、KEILAND_VERSION）。ws005 の store の形式（`~/.wifi.conf`）の所有は WS005 | `libkeiland/`・`keiland.h`・`settings/network.c`・`page-network.c` | 任 |
| C9 | 準備中の頁の扱い: 12 頁（Bluetooth・VPN・Notifications・Battery・Touchpad・Printers・Sharing・Users・Privacy・Security・Accessibility・Updates）を左の一覧から隠すか、一覧の下の「Coming later」の節にまとめる | デモで「押すと準備中」が続く印象を減らす | 0.5〜1h | 低（検索・Home の tile・試験 p006 の 23 頁の期待も直す） | ユーザーの判断（見本の項目の並びを保つか） | `settings/pages.c`・`page-home.c`・`search.c`、`plan/ws089/tests/settings-p006.sh` | 任（ユーザーの好み） |
| C10 | Battery の頁と system bar の電池の表示（今は system bar も絵だけ、[shell.c](../../userland/desktop/wayland/shell.c) の注記「a mock-up」） | 5330 で残量・充電中が見える | 6h 以上（複数の WS） | 高 | backend が無い: kernel の ACPI に `_BST`・`_BIF` の読み出しはあるが（`/dev/acpi` は診断用の text の interface）、利用者が読める電池の interface・daemon が無い。kernel・driver の WS が要る | kernel（`src/drivers/acpi/`）、libkeiland、compositor、settings | 外（別の WS） |
| C11 | 日付と時刻の頁（time zone・NTP）。今の clock は UTC（guest の画面は JST の 03:xx に「Oct 2 18:xx」）。libc の `tzset` は `TZ` の環境変数（POSIX の文字列）だけで `/etc/localtime` を読まない | 日本のデモで時刻が合う | 4h 以上 | 中（system の設定の保存・session の環境・libc） | libc・sessiond・compositor の clock の所有の WS。Settings の頁は最後 | libc、`sessiond/`、`wayland/shell.c`、settings | 外（別の WS。clock の時刻のずれだけなら Q1 に別件で） |
| C12 | Users の頁（読むだけ）: 利用者の一覧、自分が network group に入っているか | D2 の案内の補い | 1.5h | 低 | なし（passwd・group の読み出し） | `settings/` に新しい頁 | 外（ベータ1 の価値は小） |
| C13 | Settings の libkeiui への移行（WS090 p007） | 部品の共通化（見た目は同じ） | 4h 以上 | 中（全頁の描画を移す） | ユーザーの判断（ws.md の未決 (4)）。計画エージェントの案はベータ1 の後 | settings の全体（WS089 の全 Phase と衝突） | 外 |
| C14 | Touchpad の頁 | 5330 の touchpad | — | 高 | kernel の touchpad の driver（D9）が無い | — | 外 |
| C15 | 通信量の 24 時間の graph | 見本の Network の頁に近づく | 4h 以上 | 中 | 記録の daemon | networkd か新しい daemon | 外 |
| C16 | 暗い壁紙（Aurora・Twilight）の上の文字の contrast | 見た目 | — | — | compositor の glass の tint（main が WS035 に回した） | `wayland/glass.c`（衝突: P2） | 外（WS035） |
| C17 | 日本語の UI（p015）・accent と dark（p017） | — | — | — | **ベータ1 に入れないと決定済み**（F-068） | — | 外（決定済み） |

計画エージェントの推奨のまとめ: **C1・C5・C4 を入れる**（合計 3.5h、Settings の中だけ、p012 と同じ Queue の並びで直列に）。C2・C3・C6・C8・C9 は
ユーザーの好みと時間次第（C6・C8 は libkeiland の API の許可が要る）。C7 以下は依存が大きいかベータ1 の価値が小さい。

## 3. 準備中の頁のうち 5330 で意味のある物の backend（p010 の調べ）

| 頁 | 5330 での意味 | backend の有無 |
| --- | --- | --- |
| Battery | 残量・充電中・電源の設定 | **無い**: ACPI の EC と AML の評価はある（`src/drivers/acpi/`、`/dev/acpi` は namespace の診断用の text）。`_BST` を利用者に定期的に渡す interface・daemon は無い。system bar の電池は絵だけ |
| Touchpad | 速さ・tap・慣性 | 無い（touchpad の driver、D9） |
| Bluetooth | AX211 の Bluetooth | 無い（Bluetooth の stack が無い） |
| Display（変更） | 内蔵 LCD の拡大 | WS113 p006 が持つ |
| Sound | 音量・mute | **ある**（audiod、WS100。`volume-p005.sh` PASS） |
| Wi-Fi | AX211 の一覧・接続 | ある（networkd。実 radio での確認は p014、BUG-145 の後） |
| Notifications・Printers・Sharing・Users・Privacy・Security・Accessibility・Updates・VPN | — | どれも backend が無い（Users は passwd・group の読み出しだけなら可能、C12） |
