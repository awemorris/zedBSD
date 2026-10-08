<!-- awesome-plan project=zedbsd record=ws149 -->

# WS149: Settings の Security の頁の検討（要らなければ削除、要るなら設計と実装）

<!-- awesome-plan-current:start -->
Status: incomplete（2026-10-08 q910 P2 の照合: ws148-p002 は 2026-10-08 Q1 が cleared（T1-271）。WS の完了を Q1 が判定）
Primary Milestone: MG006
Related Milestones: —
Parent: [Master](../master.md)
Queue: なし（担当と時期は未定）
Resume point: 2026-10-06 ユーザーの決定で Security の頁を無くした（q824、[ws148-p002](../ws148/phase002/phase.md) で 3 つの頁をまとめて実装、host 試験 PASS）。T1 の QEMU の目視の後に p002 を cleared、WS を完了にする。
<!-- awesome-plan-current:end -->

## 単一目標

Settings の Security の頁（今は stub）で何を設定すべきかを検討し、zedBSD・Keiland に設定すべき項目が無ければ頁を削除し、あれば設計して実装する。

## ユーザーの指示（2026-10-04 夜）

「SettingsのSecurityタブも同様にWSを作って検討です。」（[WS148](../ws148/ws.md) の Privacy と同じ進め方: 「何を設定するのかわかりません。WSを作って検討し、必要ないならタブを削除、必要なら設計、実装をお願いします。」）

## 検討の観点（p001）

- 他の desktop（GNOME・KDE・macOS・Windows）の Security の頁の項目を調べ、Keiland に当てはまる物を選ぶ。候補の例:
  - 画面の lock（自動の lock の時間、sleep からの復帰で password を求めるか）と login の password の方針
  - firewall（zedBSD に packet filter があるか。無ければ対象外か別の WS）
  - disk の暗号化（zedBSD に無ければ対象外）
  - 自動の更新・software の更新の方針（release の仕組み（WS129）との関係）
  - SSH の鍵・known_hosts の管理（Sharing の SSHD（[ws089-p025](../ws089/phase025/phase.md)）と重なる）
  - 保存した WiFi の鍵・秘密の store（Keiland の秘密の store）の一覧と削除
  - USB の device の許可（新しい device の接続を許すか）
- 各項目について、今の zedBSD・Keiland に実体があるか、他の頁（Users・Sharing・Network・Privacy）で足りるかを判定する。Privacy の頁（WS148）と項目を分け合う（重ねない）。
- 結論: (a) 設定すべき項目が無い → 頁を Settings から削除。(b) ある → 項目・保存先・口を設計し、実装の Phase を足す。結論はユーザーに確かめてから進む。

## Phase（案）

| Phase | 内容 | Status | 依存 |
| --- | --- | --- | --- |
| [ws149-p001](phase001/phase.md) | 検討（上の観点、WS148 との分担）と結論の案、ユーザーの判断 | cleared（2026-10-06 ユーザー: (a) 頁を無くす） | WS148 の p001 と並べて行う |
| ws149-p002 | (a) 頁の削除（[ws148-p002](../ws148/phase002/phase.md) でまとめて実装） | cleared（2026-10-08 Q1、T1-271） | p001 |
| ws149-p003 | (b) の時の実装と回帰 | canceled（(a) に決まったので不要、2026-10-06） | p002 |
