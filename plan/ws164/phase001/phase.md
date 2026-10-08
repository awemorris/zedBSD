<!-- awesome-plan project=zedbsd record=ws164-p001 -->

# ws164-p001: 起動時の Welcome の画面（要件と設計）

Phase ID: `ws164-p001`
Parent: [WS164](../ws.md)
Status: cleared（2026-10-08 Q1 の判定（sweep-beta2-rc2 §2）: H1〜H4 決定済み、p002 が cleared）（旧: planning（2026-10-05 P1 generation17、q732。設計の第 1 版。code は §6 の判断の後。2026-10-05 夜: H1〜H3 決定、H4 は Q1 が調整として承認済み））
Phase disposition: normal
Queue: q732（ベータ2 の P1 の列の 4 番目）

## 範囲

- ユーザー（2026-10-05、原文）:「OS起動時のWelcome画面（すでにあるStartでもいいかも。要検討）」
- 決めること: 既存の物（Files の Today）で足りるか、何を見せるか、いつ出すか（最初だけか毎回か）、誰が起動するか、また見る道、試験。
- 範囲外: 実装（p002〜）。言語と入力の選択の中身（WS154 の Languages の頁、P2）。アカウントの作成（今の image は kei の自動 login、Users の頁は WS089）。

## 1. 今の形（2026-10-05 の main を読んだ）

| 項目 | 今 | 場所 |
| --- | --- | --- |
| 起動から desktop まで | sessiond が `/etc/keiland/autologin` の account（image は kei）を起動の時に 1 回 login させる。無ければ greeter | `userland/desktop/sessiond/main.c` 73・161・543 |
| session | `session.sh` が usual folder を作り、compositor を `--session --glass --wallpaper=…` で起動。compositor が desktop の program（`files --desktop`）を起動し、落ちたら起こし直す | `sessiond/session.sh`、`wayland/desktop.c` 29〜32・264〜 |
| 「Start」に当たる物 | Files の **Today**（ws127-p011 で Home から改名、Files が開く所）: wallpaper の hero に「Good morning, NAME」と今日の file の数・空きの容量、folder の card、最近の file。desktop（壁紙の上の icon）とは別で、Files の window を開くと出る | `userland/desktop/files/ui-home.c` 8〜26 |
| 最初の login の印 | 無い（home の folder を作るだけ、`sessiond/session.c` 144・481） | — |
| 設定の保存 | compositor の設定の store（`~/.config/keiland/desktop.conf`、key の表は `settings-keys.c`、Settings は libkeiland の `kl_settings_*` で読み書き） | `wayland/settings-store.c` 37、`userland/desktop/settings-keys/settings-keys.c` |
| Settings の頁 | Wi-Fi・Ethernet・Appearance・Wallpaper・Sound・Keyboard・Mouse・Touchpad・Users・About など。`settings PAGE` で頁を指して開ける | `settings/pages.c` 24〜48、`settings/main.c` 123・291 |

## 2. Files の Today で足りるか

Today は「今日の作業を始める所」で、最初の login の人に要る物（Wi-Fi に繋ぐ、壁紙と見た目を選ぶ、Kei の操作の要点を知る）を持たない。Today に足すと
毎日開く頁が重くなる。**Today とは別の短い Welcome の流れを作り、最後に Today を開く**（ユーザーの「すでにあるStartでもいいかも」は、最後の行き先として
生かす）。

## 3. 設計

### 3.1 いつ出すか

- **各 account の最初の login**（最初の起動は kei の自動 login なので、OS を初めて起動した時に出る）。毎回の起動では出さない（§6 の H1）。
- 印は設定の store の新しい key `welcome.done`（0・1、既定 0）。Settings の Welcome が終わった時か「Skip」で 1 にする。store にあるので、他の設定と同じく
  `~/.config/keiland/desktop.conf` に残り、account ごと。
- また見る道: Settings の About の頁に「Show Welcome again」の button、App Home の検索で「welcome」。

### 3.2 誰が起動するか

- compositor が session の始めに、desktop の program を起動した後で `welcome.done` が 0 なら `settings --welcome` を 1 回起動する（`desktop.c` の
  `desktop_start` の隣の小さな関数、`zwl_spawn` を使う。落ちても起こし直さない）。greeter・`--testing` では起動しない。
- Linux・FreeBSD の Keiland も同じ（compositor の中の仕組みなので OS に依らない）。

### 3.3 中身（Settings の welcome の mode）

新しい app を作らず、**Settings の既存の頁を順にたどる mode** にする（Wi-Fi・Wallpaper・Appearance の code をそのまま使い、作る物を最小にする）。
window は Settings と同じ glass の window、左の sidebar の代わりに段の点（5 つ）、下に「Back」「Next」（最後は「Start using Kei」）、右上に「Skip」。

| 段 | 内容 | 使う物 |
| --- | --- | --- |
| 1. Welcome | Kei の mark と「Welcome to Kei, NAME」、一行の説明 | 新しい頁（`welcome.c`） |
| 2. Network | Wi-Fi の radio がある時は Wi-Fi の頁（network の一覧と接続）、無ければ Ethernet の状態（繋がっていれば「You are online」） | `se_wifi_page_draw`・`se_ethernet_page_draw` |
| 3. Look | Wallpaper の tile と、Appearance の窓の不透明度 | `se_wallpaper_draw`・`se_appearance_draw` |
| 4. Keys | Kei の操作の要点の表: Super を軽く押す = App Home、Super+Tab = Wiseview、Alt+Tab = 切り替え、Super+L = lock、Ctrl+Alt+←/→ = desktop の切り替え、Alt+Space = 入力の言語（WS156 の後は Super+N = 通知の log） | 新しい頁（静的な表） |
| 5. Done | 「You are all set」。「Start using Kei」で `welcome.done` を 1 にし、Settings を閉じ、Files を Today で開く | 新しい頁 |

- 言語と入力（WS154 の Languages の頁）は、その頁ができたら段 3 と 4 の間に足す（§6 の H3）。
- 「Skip」はどの段でも `welcome.done` を 1 にして閉じる。window を閉じる（×）は印を付けない（次の login でまた出る）。

### 3.4 変える file（案）

| 場所 | 変更 | 持ち主 |
| --- | --- | --- |
| `userland/desktop/settings-keys/settings-keys.c` | `welcome.done` の行 | 共有の表（Q1） |
| `userland/desktop/wayland/desktop.c`（か新しい `welcome.c`） | 最初の login の起動 | compositor（WS164） |
| `userland/desktop/settings/welcome.c`（新）・`main.c`（`--welcome`）・`pages.c`（About の button） | welcome の mode | Settings は WS089（P2）。WS164 の新しい file と、`main.c`・`pages.c` の小さな hook は Q1 の調整 |
| 試験 | host: Settings の host の描画（`plan/ws089/tests/host-render.c` の形）で 5 段の画面、compositor の起動の規則の host 試験。QEMU（T1）: 新しい account（`welcome.done` が無い）の最初の login で Welcome が出て、Next で 5 段をたどり、Today が開き、次の login では出ないこと | WS164 |

## 4. 試験

- host: Settings の host の描画で 5 段の PNG（目で見る）、`welcome.done` の読み書き、Skip と × の違い。
- QEMU（T1）: zdesktop の login の image で `welcome.done` の無い account を login させ、`ZSETTINGS WELCOME step=…` の log と screenshot、Done の後に Files の
  Today の window、logout と login で 2 回目は出ないこと。
- 実機（UAT）: 最初の起動の見た目。

## 5. 見積もり

1 LW（ws.md）に収まる: 新しい頁 3 つ（Welcome・Keys・Done）と、既存の頁を段に並べる枠、compositor の起動の小さな関数、設定の key 1 つ。

## 6. 人間の判断が要る点

| ID | 問い | 案 |
| --- | --- | --- |
| H1 | いつ出すか | 各 account の最初の login だけ（印は設定の `welcome.done`）。Settings の About から再び見られる |
| H2 | 形 | 新しい app ではなく Settings の welcome の mode（Network・Look の既存の頁を使う）。最後に Files の Today を開く |
| H3 | 段 | Welcome・Network・Look・Keys・Done の 5 段。言語と入力は WS154 の頁ができたら足す |
| H4 | Settings の file（WS089、P2 の領域）への hook | WS164 の新しい `settings/welcome.c` と、`main.c`・`pages.c`・`settings-keys.c` の小さな変更を、Q1 の調整で WS164 の担当が書く |

### 決定（2026-10-05 夜、ユーザー、Q1 経由）

H1〜H3 を案のとおり承認。H4（Settings の file への hook）は技術の調整として Q1 が承認済み（2026-10-05、master の判断の記録: 「H4（Settings の hook を WS164 が書く）は Q1 が調整として承認（P2 の WS089 の file に触れる時は Q1 が順を決める）」）。

## 7. 段（案）

| Phase | 内容 | 依存 |
| --- | --- | --- |
| p002 | 設定の key、compositor の起動、Settings の welcome の mode（5 段）、About の button、host 試験。T1 で QEMU | H1〜H4 |
| p003 | 全文規約の見直し | p002 |

## 結果

（設計の第 1 版。判断 H1〜H4 待ち）
