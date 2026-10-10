<!-- awesome-plan project=zedbsd record=ws199-p005 -->
# ws199-p005: 5330 の UAT（YubiKey 5 NFC、USB と ACR1552）

Status: cleared（2026-10-10、実装・p004検証とユーザーのWS199完了受け入れ）
Parent: [WS199](../ws.md)

## ゴール
- ユーザーが 5330 で確かめる: Add（USB・NFC、PIN 有り・無し）、Change PIN、Reset（Cancel で消えない）、Remove、Software Security Key、login（USB・NFC、PIN 有り・無し、タッチ）、unlock の 3 通り（PIN＋タッチ・タッチだけ・どちらも不要で 0.5 秒）、鍵を挿すと自動で鍵のモードと user、lock 中に lid を閉じると取り消して眠る、NFC の置きっ放し。
- 失敗は Bug にして P1 が直す。

## やり方
- image はユーザーが top の config.mk で作り直す（Vulkan Video の試験の設定も入っている）。急ぐ時は Q1 が SSH で passkey・passkey-fido2・sessiond だけを入れ替えられる（ユーザーの許可を取る）。
- 手順と期待は plan/beta2.md の「次の UAT」の表。

## 2026-10-10 終了

ユーザーのWS199 complete・close 指示で受け入れ。詳細は [WS完了記録](../ws.md#完了close2026-10-10ユーザー受け入れ)。今回追加実施した全UATとは扱わない。remote issue無し、local cleared。Q1資材整理待ち。
