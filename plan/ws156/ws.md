<!-- awesome-plan project=zedbsd record=ws156 -->

# WS156: app の通知（画面の下の中央を流れる headline の popup と、ring の形の通知の log）

<!-- awesome-plan-current:start -->
Status: incomplete
Primary Milestone: MG006
Related Milestones: —
Parent: [Master](../master.md)
Queue: なし（担当と時期は未定）
Resume point: 2026-10-08 Q1 の判定（sweep-beta2-rc §2）: p001 cleared。 2026-10-08 q902 P1 の照合: p002〜p004 cleared。p001（設計、H1〜H7 は 2026-10-05 夜に決定、p002〜p004 で実装）は Q1 の判定待ち。残りは p005 の 5330 の UAT（popup の動き・× ・hotkey の log）。p005 の全文規約はベータ3（2026-10-08 ユーザー）、p006（Linux の D-Bus）は 10/13 以降。旧: 2026-10-08 Q1: p003・p004 cleared（T1-375b PASS）。残りは p005 の全文の規約と実機の UAT。以前: p002 cleared（T1-269）。
<!-- awesome-plan-current:end -->

## 単一目標

app から通知を出せる仕組みを作り、画面の下の中央を headline の news の板のように流れる popup で見せ、消えた後も hotkey で ring の形の log を左右にたどれるようにする。

## ユーザーの要望（2026-10-04 夜、原文）

「既存の右上の通知領域ではなく、アプリ通知の実装についてです。WindowsやMacだと、通知領域近辺にポップアップが出て、そのあとは画面右側に通知が一覧になって出ると思います。ポップアップは我々も出そうと思いますが、画面中央の下部に、画面サイズの20%程度のポップアップにしたいです。また、画面右下に、画面右端から出てきて、フェードインしながら画面中央にしゅっと素早く移動して、3秒とどまったら、画面左端にしゅっと素早く移動してアルファ値もフェードアウトして消えていくのがいいです。ヘッドラインニュースのボードみたいなものをイメージしています。通知は消えたあとも、ホットキーでリング上に左右に移動してログを見られるといいですね。通知は表示中はバツボタンで消せるといいです。ログの端、最新まで来ると、通知をすべて消去、というボタンがあるといいです。個別に消した通知はログに残さない方がいいです。」

## 仕様（ユーザーの要望から、Q1 の整理。細部は p001 で決める）

1. **popup の位置と大きさ**: 画面の下の中央、画面の大きさの約 20%。
2. **動き**（headline の news の板）: 画面の右下、右端の外から現れ、fade-in しながら画面の中央へ素早く（しゅっと）移る → 中央で 3 秒とどまる → 画面の左端へ素早く移りながら alpha を fade-out して消える。続けて通知が来た時の並び（順に流す・前の物を早送りする）は p001 で決める。
3. **表示中の × ボタン**: 表示中の通知を × で消せる。**個別に消した通知は log に残さない**。
4. **log**: 消えた（流れ去った）通知は log に残る。**hotkey** で log を開き、**ring の形に左右へ**たどれる。log の端（最新）まで来ると「**通知をすべて消去**」の button がある。
5. 既存の右上の通知領域（system bar の icon）とは別の仕組み。

## 範囲（p001 で設計して確定）

- app から通知を出す口: 標準の freedesktop の Desktop Notifications（D-Bus）は zedBSD に D-Bus が無いので、Keiland の拡張の Wayland protocol か libkeiland の口（`kl_notify_*` など）。Linux・FreeBSD の Keiland で D-Bus の通知を受けるかも検討。
- 通知の内容（app の名前・icon・題・本文・動作の button の有無・緊急度）、同じ app の通知の置き換え、全画面の時・lock の時の扱い（Privacy の頁（WS148）の「lock の画面で通知を出すか」と関係）。
- compositor の描画（Vulkan、animation の曲線、fade、20% の大きさの文字の配置）、hotkey の割り当て、log の保存（session の中だけか、再起動を越えて残すか）。
- 使う側: カレンダー（WS155）の予定の時刻、system の事象（電池・PnP・WiFi の失敗（BUG-187）など、WS132）。

## Phase（案）

| Phase | 内容 | Status | 依存 |
| --- | --- | --- | --- |
| [ws156-p001](phase001/phase.md) | 設計（通知の口・内容・popup の動き・log と hotkey・保存・試験の方法） | cleared（2026-10-08 Q1 の判定（sweep-beta2-rc §2）） | | — |
| [ws156-p002](phase002/phase.md) | 通知の口（protocol・libkeiland）と compositor の受け取り | cleared（2026-10-06 Q1、T1-269。実機は UAT） | p001 |
| [ws156-p003](phase003/phase.md) | popup の描画と動き（右から中央、3 秒、左へ fade-out）、× で消す、全画面・lock、system の通知 | cleared（2026-10-08 Q1、T1-375b PASS 20 項目） | p002 |
| [ws156-p004](phase004/phase.md) | log（hotkey、ring の左右、すべて消去、個別に消した物は残さない） | cleared（2026-10-08 Q1、T1-375b PASS） | p003 |
| ws156-p005 | 全文の規約と QEMU の回帰（T1）、実機の UAT | uncleared（2026-10-08 QEMU の回帰 T1-375b PASS、20 項目 ok、PNG 5 枚。残り: 5330 の実機の UAT。全文の規約の見直しはベータ3、2026-10-08 ユーザー） | p002〜p004 |
| ws156-p006 | Linux: libkeiland-backend の D-Bus の `org.freedesktop.Notifications`（設計 p001 §11、2026-10-05 ユーザー「実装はあと回し」） | planning（後回し。Linux の作業は 10/13 以降、2026-10-08 ユーザー） | p002 |
