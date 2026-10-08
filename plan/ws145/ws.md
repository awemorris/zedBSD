<!-- awesome-plan project=zedbsd record=ws145 -->

# WS145: 印刷（printer の daemon・IPP/LPD で PDF を送る、libkeiland の印刷の口、Settings の Printers の頁）

<!-- awesome-plan-current:start -->
Status: incomplete（2026-10-08 q910 P2 の照合: p001〜p004・p007 cleared（2026-10-07・08）。残り: p005（Linux・FreeBSD の guest、10/13 以降）、p006（規約、ベータ3）。準正常系は WS177 の案 Q・BUG-271）
Primary Milestone: MG006
Related Milestones: MG005
Parent: [Master](../master.md)
Queue: q831（P2）
Resume point: p002〜p004・p007 を実装（host・build 済み）、T1 待ち。p005（Linux・FreeBSD の guest の確認）と p006（全文規約）は後。
<!-- awesome-plan-current:end -->

## 単一目標

app が libkeiland に PDF の場所を渡して印刷を依頼すると、network の printer（IPP か LPD）で印刷される。printer の一覧と既定の printer を libkeiland から compositor 経由で取れ、Settings の Printers の頁で printer の IP address・port・protocol を設定できる。

## ユーザーの指示（2026-10-04 夜、原文）

「SettingsのPrintersは、プリンタのIPアドレス、ポート、プロトコルだけ設定できる画面を追加してほしいです。libkeilandでプリンタ一覧やデフォルトプリンタをコンポジタ経由で取得可能にしましょう。実際のプリントは、libkeilandにPDFの場所を書いて印刷依頼を投げると、libkeiland-backendがプリンタデーモンが未起動なら起動し、デーモンがIPPやLPDのプロトコルでPDFを投げる、という形にしたいです。PDFが最初のフォーマットでOKで、そのあとPostScriptやベンダー形式への変換フィルタなども実装して広げていけばいいと思います。」

## 範囲（p001 で設計して確定）

1. **Settings の Printers の頁**（今は stub）: printer を足す・消す・既定にする。設定項目は IP address・port・protocol（IPP か LPD）だけ（名前は自動か入力は設計で決める）。
2. **libkeiland の口**: printer の一覧と既定の printer の取得（compositor の拡張 kl_system_manager_v1 を経て libkeiland-backend）、印刷の依頼（PDF の path を渡す、job の状態の通知・取り消しは設計で決める）。app は OS の口を持たない（Guardrail の「配置」「app と設定」）。
3. **libkeiland-backend**: 印刷の依頼を受けて、printer の daemon が起動していなければ起動し、job を渡す。2026-10-05 の設計（design.md D1）: daemon は**利用者の権限**の `keiland-printd` で、backend が `posix_spawn` で起動する（root の daemon・setuid・init の service（WS002）は使わない）。printer の設定は利用者ごとの `~/.config/keiland/printers.conf`（D2 は要確認）。
4. **printer の daemon**（新しい userland の daemon）: job の queue（spool）、IPP（RFC 8011 の Print-Job、HTTP の上）と LPD（RFC 1179）で PDF を送る。最初の形式は PDF だけ（printer が PDF を受ける前提。受けない printer は失敗を返す）。
5. **後の段**（別の Phase）: PostScript・vendor の形式（PCL・raster など）への変換の filter を足して対応する printer を広げる。printer の発見（mDNS/DNS-SD）は後の候補。
6. Linux・FreeBSD の Keiland: backend で CUPS などの既存の仕組みを包む（設計で決める）。
7. 外部の実装を取り込む場合の license の監査（Guardrail・設計方針）。

## Phase（案）

| Phase | 内容 | Status | 依存 |
| --- | --- | --- | --- |
| [ws145-p001](phase001/phase.md) | 調査と設計（[design.md](design.md)） | cleared（2026-10-08 Q1） | — |
| [ws145-p002](phase002/phase.md) | keiland-printd（約束の行・spool・IPP・LPD・寿命）と host の符号化・通し・寿命の試験 | cleared（2026-10-07、T1-318） | p001 |
| [ws145-p003](phase003/phase.md) | backend の print（設定の file・printd の起動と通信・状態遷移）、compositor の printers の object、protocol version 17、libkeiland の口（KL_VERSION 56 の案）、`printtest`、host の backend の試験 | cleared（2026-10-07、T1-318） | p002 |
| [ws145-p004](phase004/phase.md) | Settings の Printers の頁（IP address・port・protocol、詳しい設定の path・queue） | cleared（2026-10-07、T1-318） | p003 |
| ws145-p005 | Linux・FreeBSD の build と install と Debian の QEMU+KVM・FreeBSD 15 の guest の確認 | planning | p003 |
| ws145-p006 | 全文の規約の確認と回帰、T1 の QEMU の試験（最後） | planning | p002〜p005 |
| （別の WS の案） | 変換の filter（PDF → PWG raster・PostScript・PCL）。単一目標の外。D4 の機種が PDF を受けなければ受け入れの前に | — | p002 |
| [ws145-p007](phase007/phase.md) | PDF Viewer の File > Print（D6: 含める） | cleared（2026-10-07、T1-318） | p003 |