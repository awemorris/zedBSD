<!-- awesome-plan project=zedbsd record=ws143p001 -->

# ws143-p001: 調査と設計（Bluetooth）

Phase ID: `ws143-p001`
Parent: [WS143](../ws.md)
Status: cleared（2026-10-08 Q1 の判定: design.md の §9 の D1〜D18 はユーザーが全部推奨どおりに決定（2026-10-05）、HID が先、A2DP・PAN は F-082 へ）
Phase disposition: normal
Queue: q752（P1、2026-10-05。Q1「Go ahead with p001 (survey and design) under q752」）、q860-i01（P2、2026-10-08、記録の締め）

## 範囲

device（5330 の AX211 の Bluetooth、USB 8087:0033）、firmware、HCI の transport と core、profile の範囲、desktop の経路
（libkeiland-backend・kl_system_manager_v1・Settings・system bar、Linux・FreeBSD の Keiland）、外部の実装と license の境界、試験の方法を
調べ、[design.md](../design.md) に設計を書く。code は書かない。設計は design-reviewer の敵対的な review を通す。

ユーザーの判断が要る点（profile の範囲、UAPI、root の daemon の口、外部の package、firmware の package）は design.md §9 に選択肢と
推奨を書き、Q1 経由でユーザーに尋ねる。答えが出るまで実装の Phase（p002〜）は planning のまま。

## 完了の条件

- design.md が §1〜§10 を持ち、各事実に出典（file と行、上流の URL と revision）がある。
- design-reviewer の指摘を全て反映するか、反映しない理由を書いた。
- §9 の判断の質問を Q1 に送った（答えは WS の記録に残す）。

## 確認

### 2026-10-05 q752-i01: design.md 第 1 版と design-reviewer（中断）

design.md 第 1 版を書き、design-reviewer（agent afdfd376fe101aed5）の review を受けた: blocker 3（F1: Intel の bootloader は
Secure Send を bulk OUT で送り返事が bulk IN に来るので `/dev/btN` の型の固定の形では load できない、F2: BR/EDR の SSP の暗号は
controller が行い host は要らない・LE の P-256 は controller の HCI_LE_Generate_DHKey で可・既存の wlan-crypto の AES・D5 の OpenSSL は
master-design-policy §2.1 の例外、F3: pairing の許可の模型と鍵の注入の防御が無い）、major 10、minor 15、加えて §9 に無い判断 D8〜D16。
全文は agent の報告（Q1 にも要約を送る）。2026-10-05 Q1 の URGENT（BUG-195）で中断。

再開の点: review の F1〜F25 を design.md の第 2 版に反映し（§3 の手順の書き直し、§5.1 の bootloader の経路・寿命・queue の
backpressure、§5.2 の usb-hid の glue の分離と LED、§6 の security の節と特権の分離・LE の再接続・legacy の pairing、§8 の D-Bus の
拡張と API の版、§9 に D8〜D16、§10 の Phase の順と UAT の Phase と見積もりの見直し）、もう一度 design-reviewer を通してから Q1 に
判断の質問を送る。

### 2026-10-05 q752-i01（続き）: 第 2 版・第 3 版

第 2 版（43273705）に F1〜F25 を反映し、design-reviewer（agent a40b549b4e89b1245）の 2 回目の review を受けた（[review-2.md](review-2.md)、
N1〜N17）。第 3 版で全て反映（§3 の手順と失敗の経路、§5.1 の reset の約束と queue、§5.3 の resume、§6.1・§6.2・§6.4 の BR/EDR の
流れと鍵の長さ、§6.5 の特権の分離と seat の人、§8.1 の版、§9 の並べ直し（D15 を先に、D16〜D18 を追加、D11 を分ける））。

第 3 版の部分の再確認（同じ agent）: N1〜N17 のうち 16 が解決、N11（resume の印）が残り、`/dev/system` に resume の class を足す案（a）で
直した（D2 の中で承認を求める）。D5・D18・情報のお願いの文の小さな直しも反映。reviewer の判断「§5.3 を直せば §9 をユーザーに送ってよい、
次の review は要らない」。

### 2026-10-08 q860-i01: 記録の締め（cleared の提案）

- ユーザーの判断: 2026-10-05 夕「WS143 Bluetooth: §9 の D1〜D18 は全部推奨どおり（音と PAN は後回し、HID が先）」と、同日の
  「HIDが先で、オーディオとPANもほしいですが、ほかの開発項目より後回しでいいです。」（[master](../../master.md) の decisions-log）。
  design.md の §9 に決定の欄を足し、§1 の受け入れを確定にした。
- 完了の条件の照合: (1) design.md の §1〜§10 と出典（第 3 版）、(2) review-1 の F1〜F25 と review-2 の N1〜N17 を全て反映（reviewer の判断
  「次の review は要らない」）、(3) §9 の質問を Q1 に送り答えを得た（上）。3 つとも満たすので cleared を提案する（判定は Q1）。
- 次: [ws.md](../ws.md) の Phase の表に p002〜p009 を置いた（HID の経路は p002〜p005、desktop は p006）。A2DP と PAN は D1 のとおり別の WS
  （Future Work への登録を Q1 に依頼）。
