<!-- awesome-plan project=zedbsd record=ws155 -->

# WS155: Keiland の app: カレンダー・スケジューラ・オーガナイザ（まず簡単な物）

<!-- awesome-plan-current:start -->
Status: incomplete（2026-10-08 Q1 の判定（sweep-beta2-rc §2）: p000 cleared。 2026-10-08 q902 P1 の照合: p001〜p004 cleared（2026-10-07 Q1、T1-297）。ベータ2 の範囲の残りは p000 の扱いだけ、続きはベータ3）
Primary Milestone: MG006
Related Milestones: —
Parent: [Master](../master.md)
Queue: なし
Resume point: 2026-10-08 q902 P1 の照合: p002〜p004 cleared（T1-297、.ics の中身は未確認）。p000（mock）は p001 の設計と p003 の app に置き換わったので Q1 が cleared か canceled かを判定。p005（全文規約）はベータ3。旧: T1 の結果（p002〜p004）、その後 p005（規約の見直し、後回し）。
Target: **ベータ3**（続き）（2026-10-07 ユーザー「下記をベータ3に移動します。・左手デバイスOSK、ゲームパッドOSK, 写真の続き, カレンダーの続き, IMEの続き、POSIX, NVMe, make, RTL8822C, Sleep」）
<!-- awesome-plan-current:end -->

## 単一目標

Keiland の標準 app として、カレンダー・予定の管理（スケジューラ）・オーガナイザの app を作る。まずは簡単な物。

## ユーザーの指示（2026-10-04 夜、原文）

「Keilandアプリとしてカレンダー・スケジューラ・オーガナイザを実装します。まずはシンプルなものでいいです。ユーザの宿題として、アプリの外観の画像を提出するまでブロックします。」

## デザイン案（2026-10-05 ユーザーの提出）

ユーザー「カレンダーのデザイン案をお渡しします。このままにする必要はなく、要素で利用してほしいです。3Dを利用します。Linuxデスクトップのような格好つけだけのクールさはいらないです。Windows 11のように、実用性があるミニマルな中に、知性を感じさせるデザインがいいです。それでも、少しの動的なデザインがほしいです。そこに3Dのアニメーションを入れて、落ち着いた知性を表現したいです。モックを見て再指示するので、具体的デザインをお任せします。」

画像は会話の添付で file は無い。Q1 が要素を書き取った物（このままにする必要は無い、要素として使う）:

- 窓: 淡い青の glass の背景。左上に信号機の 3 つの button、上の帯に ‹ › の移動・「Today」・中央の segmented の「Month / Week / Day」（選ばれた物は青の塗り）・右に「Search events…」の検索の欄と「…」の menu。
- 左の sidebar: app の icon と「Calendar」の題、「Month View」（選ばれた行は淡い青の塗り）・「Today」・「Search」・「Settings」の行（線の icon）。区切りの後に「My Calendars」: Work（青）・Personal（赤）・Family（緑）・Study（黄）の色つきの checkbox、「+ Add Calendar」。下に小さな card（icon、「A more organized you」「Plan today for a brighter tomorrow.」）。
- 中央: 月の表を縦に続けて scroll（「April 2025」「May 2025」「June 2025」の大きな見出し）。曜日の行、日曜の列は淡い赤・土曜の列は淡い青の地で数字も赤・青、前後の月の日は灰色、今日（15）は青の塗りの cell。予定は cell の中の小さな pill（色の点＋題、地は calendar の色の淡い色: Team Meeting・Doctor Appointment・Study Session・Kids' Event・Project Review など）。
- 右の panel 「Add Event」: 「Drag an icon to a date on the calendar to create a new event.」。種類の card（大きな立体の icon: Work 青の鞄・Personal 赤のハート・Study 緑の本・Family 黄の人々、副題「Meeting, Task, Deadline」など）と「Custom / Create your own」（+）。下に **3D の日めくりの絵**（青い綴じ輪の付いた「Apr 15」の卓上カレンダーが斜めに立ち、後ろに重なる頁、周りを淡い青のリボンが流れる）と「Small plans make big days.」。
- 全体: 角の丸い card、柔らかい影、淡い青の諧調、立体感のある icon。

### 方針（Q1、mock の前提。ユーザーが mock を見て変える）

- 目指す物: Windows 11 のような実用的でミニマルな中に知性。格好だけの効果は入れない。
- 動き: 少しだけ。3D のアニメーションで「落ち着いた知性」を表す。例: 今日の日めくりの 3D の絵がゆっくり呼吸するように傾く、日付が変わる・月を移る時に頁がめくれる、予定を drag で落とした時に cell が軽く沈む。動きは短く（数百 ms）、常時の動きは遅く小さく、「動きを減らす」の設定で止める。
- 3D の描き方: compositor の GPU（Venus）を app から直接使う口は今無いので、mock では app の中の小さな software の 3D（三角形の rasterizer、照明は 1 つ）で日めくりの絵を描く。本実装の描き方は p001 で決める。

## 止めていた理由（解除済み）

- **ユーザーの宿題**: app の外観の画像の提出。届くまで設計・実装を始めない。

## 範囲（画像が届いた後に p001 で設計して確定）

- 月・週・日の表示、予定の追加・編集・削除、繰り返しの予定は「まず簡単な物」の範囲に入れるかを決める。
- To-do・memo（オーガナイザ）の範囲。
- 保存の形式（iCalendar（.ics）の file を利用者の領域に、など）と、他の app（Files・通知）との関係。通知（予定の時刻の知らせ）は compositor の通知の仕組みに。
- libkeiland の UI の部品で作る（他の標準 app と同じ）。Linux・FreeBSD の Keiland でも動く。
- **system bar の時計からの起動**（2026-10-04 夜 ユーザー「画面右上通知領域の日付・時刻をクリックすると、カレンダーappが表示されるようにします。」）: 右上の通知領域の日付・時刻のクリックで、この app を起動する（起動済みなら前に出す。単一の instance の扱いは ws089-p016 の activation の仕組みと合わせる）。compositor の system bar の側の変更（WS099）も、この WS の Phase に含める。
- 同期（CalDAV・クラウド）は後の候補（WS146・WS147 と関係）。

## Phase（案）

| Phase | 内容 | Status | 依存 |
| --- | --- | --- | --- |
| ws155-p000 | **UI の mock**（デザイン案の要素と上の方針で。月の表示を中心に、sidebar・Add Event の panel・3D の日めくりの animation。data は固定の試験 data、保存は作らない）。ユーザーが見て再指示する | cleared（2026-10-08 Q1 の判定（sweep-beta2-rc §2）） | | — |
| ws155-p001 | 外観の画像に基づく設計（画面・操作・保存・通知・試験） | cleared（q831、P2、[phase](phase001/phase.md)） | p000 の mock へのユーザーの再指示 |
| ws155-p002 | 予定とメモの保存（`~/Documents/Calendar`、iCalendar） | cleared（2026-10-07、T1-297） | p001 |
| ws155-p003 | app: 保存・編集・Week と Day・開始の通知 | cleared（2026-10-07、T1-297） | p002 |
| ws155-p004 | system bar の時計から Calendar を開く | cleared（2026-10-07、T1-297） | p001 |
| ws155-p005 | 全文規約の見直し | planning（ベータ3、2026-10-08 ユーザー「コーディング規約による整形はベータ3」） | p002〜p004 |

## mock への再指示（2026-10-05 ユーザー、T1-179 の画面を見て）

ユーザー「カレンダーのスクショ、いいですね。気に入りました。カレンダーはglass透過にして、ペインは分離して背景はなしの、Filesと同じスタイルにしましょう。右下のカレンダーの画像部分のスペースがちょっと無駄っぽく見えるので、メモ・ノートの領域にしましょう。日付ごととかでなくて、アプリの唯一のメモ領域でOKです。メモはドラッグして日付にドロップすると、メモ属性の予定として追加して、でもこれは予定追加ではなくて、予定追加の機能を応用した、メモの保存にしましょう。」

- 見た目: Files と同じ glass の style（pane は分離、背景（地）は無し）。
- 右下の 3D の日めくりの場所を、app に 1 つだけのメモ（ノート）の領域に替える（日付ごとではない）。
- メモを drag して日付に drop すると、その日に「メモ」の属性の項目として保存する。予定の追加ではなく、予定の追加の仕組みを応用したメモの保存（予定とは見た目・扱いを分ける）。
- mock への反映は ws155-p000 の続き（q745-i02、P2）。
