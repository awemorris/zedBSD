<!-- awesome-plan project=zedbsd record=ws172-p007 -->
# ws172-p007: greeter（login の画面）で認証の方式を選ぶ

Status: test-wait（2026-10-09 P1: 実装、build warning 0。QEMU の PNG と fido2-p003-guest は T1 の再依頼）
Disposition: normal
Parent: [WS172](../ws.md)
Queue: なし（p002 の T1 の結果の後に投入）

## 由来（2026-10-08 ユーザー）

「PINログインの仕組みができたら、PIN, Password, Hardware Keyから選べるようにしてほしいです。」

2026-10-08 ユーザー（続き）: lock の画面の分は [WS187](../../ws187/ws.md) p003 へ移した。この Phase は greeter の分だけで、WS172 とともにベータ3。

## 範囲

- greeter と lock の画面に、認証の方式の選択（PIN・Password・Hardware Key）を出す。登録の無い方式は出さない（例: PIN が未登録なら PIN を出さない）。既定は前回使った方式か、登録のある最初の方式（設計で決める）。
- 選んだ方式で sessiond の `AUTH name style`・`UNLOCK style` を呼ぶ（p001・p002 の口）。Hardware Key は p003（FIDO2）の経路。p003 が未完了の間は Hardware Key を出さないか、無効の表示にする。
- lock の画面の配置は [WS187](../../ws187/ws.md)（大きな時計）と重ならないこと。
- 規約: plan/coding-style.md の全文。

## 確認

- build warning 0、選択の host 試験。QEMU の PNG は T1。

## 記録

- 2026-10-09 P1（ベータ3 の合間の仕事、Q1 に一言の後）:
  - 今の形: lock の画面は WS187 p003 で方式を横に並べていた（Password・PIN・Security Key、sessiond が今使えると言う物だけ）。login の画面は field の下の link で次の方式に回すだけだった。
  - 実装（`userland/desktop/wayland/greeter.c`）: `greeter_layout_styles()` の「login の画面では出さない」をやめ、login の画面にも同じ横並びの方式を出す。password だけの時は出さない（今の画面のまま）。press の判定は、方式の pill を先に見る。login の画面の Restart・Shut Down は残し、lock の画面だけが他を受けない。横並びがある時は link を出さず、link の判定もしない。log の `KWL GREETER style-at style=N x= y= width= height=` は login の画面でも出る（試験の pointer 用）。file だけの `UNUSED_PARAMETER` を足した。
  - 登録の無い方式は出さない: sessiond の STYLES の答え（`greeter_styles`）だけを並べる。Hardware Key は p003（cleared）の経路で、sessiond が KEY を出す時だけ出る。
  - 既定の方式（設計で決めること）: 今の `greeter_styles_take()` のままにした。PIN が出ていれば、user が選んでいない限り PIN。無ければ password。「前回使った方式」の記憶は入れていない（greeter は何も保存しない。要るならユーザーの判断）。
  - Tab: login の画面では今どおり次の user を選ぶ（方式は press で選ぶ）。lock の画面では次の方式（WS187 p003 のまま）。
  - 試験の追従: `plan/ws172/tests/fido2-p003-guest.sh` は link の真ん中を押していた（横並びでは pill の間に当たる）。`style_at N`（`KWL GREETER style-at`）で key（4）と password（1）の pill の真ん中を押す形にし、`KWL GREETER style=N via=choice` も受ける。`passkey-p002-guest.sh` は link を押さない（PIN が既定、keyboard だけ）ので変えていない。
  - 確認: `make ZEDBSD_CONFIG=plan/ws035/tests/config-amd64-zdesktop.mk BUILD=build/p1-ws177 build/p1-ws177/bin/wayland`（-Werror、成功、warning 0）、style-check の新しい指摘 0、`sh -n` ok。QEMU は未実施（T1）。

## 2026-10-09 P1: T1-490 の passkey の FAIL の解析と試験の直し（q916 の 3 (a)）

- T1-490 (2) の `no-pin-after-restart: FAIL (1)` は p007 の変更ではなく試験の側の不具合。BUG-274 の直し（ps の 4520a9847・8f3edd5e0・65e3cb9ba、image の tree a9fb20a0a に入っている）で `ps -o args` が command の行の全体を出すようになり、試験の `stop_all`（`ps -A -o pid,args | grep -E "[s]essiond|[w]ayland( |$)"` の pid を kill）が、それを走らせている guest の shell 自身（行に `/sbin/sessiond --graphical` や `sessiond.log` を含む）を kill していた。その後の `rm -f /var/log/greeter.log` と sessiond の起動が走らず、手順 6 は手順 1・5 の古い greeter.log（手順 5 の `styles=3` を含む）を数えていた。
  - 証拠: T1 の build/t1-490/passkey2/ の最後の greeter.log は greeter が 2 つ（pid 66 の `styles=1`・password、pid 270 の `styles=3`・PIN）だけで、手順 6 の 3 つ目の greeter が無い。sessiond.log は手順 6 の `SESSIOND STOP` で終わり、新しい sessiond の起動の行が無い（手順 6 の `SESSIOND AUTH ok … style=password` の ok は手順 1 の行に当たっていた）。
- 直し: `plan/ws172/tests/passkey-p002-guest.sh` と `fido2-p003-guest.sh` の `stop_all` を、`ps -A -o pid,comm`（argv[0]、例 `/bin/wayland`・`/sbin/sessiond`）の basename で選ぶ形（awk `$2 ~ /(^|\/)(sessiond|wayland)$/`）に変えた。host で偽の ps の出力に当てて、`/sbin/sessiond`・`/bin/wayland`・`wayland` を選び `sh`・`sessiondx` を選ばないこと、quoting が guest の shell まで保たれることを確かめた。`bash -n` ok。
- 再試験は T1（plan/agents/T1/requests.md に依頼の行）。p007 は test-wait のまま。
