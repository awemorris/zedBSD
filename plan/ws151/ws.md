<!-- awesome-plan project=zedbsd record=ws151 -->

# WS151: Settings の Accessibility の頁の検討（要らなければ削除、要るなら設計と実装）

<!-- awesome-plan-current:start -->
Status: incomplete（2026-10-08 q910 P2 の照合: ws148-p002 は 2026-10-08 Q1 が cleared（T1-271）。WS の完了を Q1 が判定）
Primary Milestone: MG006
Related Milestones: —
Parent: [Master](../master.md)
Queue: なし（担当と時期は未定）
Resume point: 2026-10-06 ユーザーの決定で Accessibility の頁を無くした（q824、[ws148-p002](../ws148/phase002/phase.md) で 3 つの頁をまとめて実装、host 試験 PASS）。T1 の QEMU の目視の後に p002 を cleared、WS を完了にする。
<!-- awesome-plan-current:end -->

## 単一目標

Settings の Accessibility の頁（今は stub）で何を設定すべきかを検討し、設定すべき項目が無ければ頁を削除し、あれば設計して実装する。

## ユーザーの指示（2026-10-04 夜）

「SettingsのAccesibilityも検討WSを作ってください。」（[WS148](../ws148/ws.md) の Privacy・[WS149](../ws149/ws.md) の Security と同じ進め方: 検討し、必要ないなら頁を削除、必要なら設計と実装）

## 検討の観点（p001）

- 他の desktop（GNOME・KDE・macOS・Windows）の Accessibility の頁の項目を調べ、Keiland で実現できる物と、実現に何が要るか（compositor・libkeiland・各 app の対応）を判定する。候補の例:
  - 見る: 文字の大きさ（全体の scale・font の倍率）、高い contrast の外観、動き（animation）を減らす、pointer の大きさ、画面の拡大（zoom・magnifier）、色の反転・色覚の補正の filter（compositor の shader）
  - 聞く: 音の通知の画面での表示（visual bell）、mono の音声
  - 操作: 固定 key（sticky keys）、遅い key（slow keys）、key のリピートの調節（Keyboard の頁と重なる）、マウスキー、double click の速さ、タッチの長押しの時間
  - 読み上げ（screen reader）: accessibility の API（AT-SPI に当たる物）と読み上げの engine が要る大きな仕事。対象にするか、別の WS にするかを判定する
- 各項目について、今の Keiland に実体があるか、他の頁（Display・Keyboard・Mouse・Touchpad・Look）で足りるかを判定する。
- 結論: (a) 設定すべき項目が無い → 頁を削除。(b) ある → 項目・保存先（desktop.conf・kl_settings_*）・compositor と app の口を設計し、実装の Phase を足す。結論はユーザーに確かめてから進む。

## Phase（案）

| Phase | 内容 | Status | 依存 |
| --- | --- | --- | --- |
| [ws151-p001](phase001/phase.md) | 検討（上の観点、他の頁との分担）と結論の案、ユーザーの判断 | cleared（2026-10-06 ユーザー: (a) 頁を無くす） | — |
| ws151-p002 | (a) 頁の削除（[ws148-p002](../ws148/phase002/phase.md) でまとめて実装） | cleared（2026-10-08 Q1、T1-271） | p001 |
| ws151-p003 | (b) の時の実装と回帰 | canceled（(a) に決まったので不要、2026-10-06） | p002 |
