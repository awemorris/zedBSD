<!-- awesome-plan project=zedbsd record=ws148 -->

# WS148: Settings の Privacy の頁の検討（要らなければ削除、要るなら設計と実装）

<!-- awesome-plan-current:start -->
Status: incomplete（2026-10-08 q910 P2 の照合: p002 は 2026-10-08 Q1 が cleared（T1-271）。WS の完了を Q1 が判定（Resume point の「T1 の後に完了」の条件は満たした））
Primary Milestone: MG006
Related Milestones: —
Parent: [Master](../master.md)
Queue: なし（担当と時期は未定）
Resume point: 2026-10-06 ユーザーの決定で Privacy の頁を無くした（q824、[ws148-p002](../ws148/phase002/phase.md) で 3 つの頁をまとめて実装、host 試験 PASS）。T1 の QEMU の目視の後に p002 を cleared、WS を完了にする。
<!-- awesome-plan-current:end -->

## 単一目標

Settings の Privacy の頁（今は stub）で何を設定すべきかを検討し、zedBSD・Keiland に設定すべき項目が無ければ頁を削除し、あれば設計して実装する。

## ユーザーの指示（2026-10-04 夜、原文）

「SettingsのPrivacyタブは、何を設定するのかわかりません。WSを作って検討し、必要ないならタブを削除、必要なら設計、実装をお願いします。」

## 検討の観点（p001）

- 他の desktop（GNOME・KDE・macOS・Windows）の Privacy の頁の項目を調べ、Keiland に当てはまる物を選ぶ。候補の例:
  - 画面の lock（時間・自動の lock）と lock の画面での通知の表示（今の lock の機能（WS035）との関係、別の頁（Display・Power）に置く方が自然か）
  - 最近使った file の履歴（Files・app の recent の記録）の on・off と消去
  - Trash の自動の削除（[ws089-p023](../ws089/phase023/phase.md) の Storage の Trash と重なる）
  - location・camera・microphone の app ごとの許可（今の zedBSD に該当の device・仕組みがあるか。無ければ対象外）
  - 画面の共有・録画の許可（screencopy などの protocol の許可）
  - 診断・crash の報告（zedBSD は外部へ送らない方針なら項目は不要）
- 各項目について、今の zedBSD・Keiland に実体があるか、他の頁で足りるかを判定する。
- 結論: (a) 設定すべき項目が無い → 頁を Settings から削除（pages.c の表、search の語、glyph）。(b) ある → 項目・保存先（desktop.conf・kl_settings_*）・compositor と app の口を設計し、実装の Phase を足す。
- Security の頁（[WS149](../ws149/ws.md)）と項目を分け合う（重ねない）。p001 は WS149 の p001 と並べて行う。
- 結論はユーザーに確かめてから (a) か (b) に進む（頁を消すのは利用者に見える変更）。

## Phase（案）

| Phase | 内容 | Status | 依存 |
| --- | --- | --- | --- |
| [ws148-p001](phase001/phase.md) | 検討（上の観点、他の desktop の調べ、今の実体の有無）と結論の案、ユーザーの判断 | cleared（2026-10-06 ユーザー: (a) 頁を無くす） | — |
| [ws148-p002](phase002/phase.md) | (a) 頁の削除と、最近の履歴の口（Files の Clear Recents、Storage の Keep recent items） | cleared（2026-10-08 Q1、T1-271） | p001 |
| ws148-p003 | (b) の時の実装と回帰 | canceled（(a) に決まったので不要、2026-10-06） | p002 |
