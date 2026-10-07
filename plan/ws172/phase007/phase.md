<!-- awesome-plan project=zedbsd record=ws172-p007 -->
# ws172-p007: greeter・lock の画面で認証の方式を選ぶ

Status: planned
Disposition: normal
Parent: [WS172](../ws.md)
Queue: なし（p002 の T1 の結果の後に投入）

## 由来（2026-10-08 ユーザー）

「PINログインの仕組みができたら、PIN, Password, Hardware Keyから選べるようにしてほしいです。」

## 範囲

- greeter と lock の画面に、認証の方式の選択（PIN・Password・Hardware Key）を出す。登録の無い方式は出さない（例: PIN が未登録なら PIN を出さない）。既定は前回使った方式か、登録のある最初の方式（設計で決める）。
- 選んだ方式で sessiond の `AUTH name style`・`UNLOCK style` を呼ぶ（p001・p002 の口）。Hardware Key は p003（FIDO2）の経路。p003 が未完了の間は Hardware Key を出さないか、無効の表示にする。
- lock の画面の配置は [WS187](../../ws187/ws.md)（大きな時計）と重ならないこと。
- 規約: plan/coding-style.md の全文。

## 確認

- build warning 0、選択の host 試験。QEMU の PNG は T1。

## 記録
