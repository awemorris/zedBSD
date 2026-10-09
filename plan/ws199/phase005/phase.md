<!-- awesome-plan project=zedbsd record=ws199-p005 -->
# ws199-p005: 5330 の UAT（YubiKey 5 NFC、USB と ACR1552）

Status: planning（p004 の後、ユーザーが image を作り直してから）
Parent: [WS199](../ws.md)

## ゴール
- ユーザーが 5330 で確かめる: Add（USB・NFC、PIN 有り・無し）、Change PIN、Reset（Cancel で消えない）、Remove、Software Security Key、login（USB・NFC、PIN 有り・無し、タッチ）、unlock の 3 通り（PIN＋タッチ・タッチだけ・どちらも不要で 0.5 秒）、鍵を挿すと自動で鍵のモードと user、lock 中に lid を閉じると取り消して眠る、NFC の置きっ放し。
- 失敗は Bug にして P1 が直す。

## やり方
- image はユーザーが top の config.mk で作り直す（Vulkan Video の試験の設定も入っている）。急ぐ時は Q1 が SSH で passkey・passkey-fido2・sessiond だけを入れ替えられる（ユーザーの許可を取る）。
- 手順と期待は plan/beta2.md の「次の UAT」の表。
