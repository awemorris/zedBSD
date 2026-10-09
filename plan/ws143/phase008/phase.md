<!-- awesome-plan project=zedbsd record=ws143-p008 -->
# ws143-p008: UAT（5330 の素の機械、ユーザーの BR/EDR と LE のキーボード・マウス）と Wi-Fi との共存

Status: cleared（2026-10-10 Q1 判定: ユーザーの 5330 の UAT「Bluetoothはキーボード、マウスについて接続確認、利用可能であることを確認できました。」。BUG-275（USB の zero-bandwidth の endpoint）の直しの後）
Disposition: normal
Parent: [WS143](../ws.md)
Queue: none
依存: p006（cleared）、p005 の i02・i03（T1-464・T1-467 PASS）、T1-502（p002〜p005 の回帰の再試験、今の tree）、ユーザーの機器

## 目的

[design.md](../design.md) §1 の受け入れ 1〜4 を 5330 の実機で確かめる（電波は実機だけ）。p003 の i02（intelbt の firmware の load）と p005 の i04（実機の門、Q20）も同じ場で見る。
結果で、ベータ2 に Bluetooth を入れるか（2026-10-09 ユーザー「入れる、動かなければ既知」、10/16 に OFF）を決める材料にする。

## 用意

- image: release の config（`config/release/config-amd64-beta2.mk`、bluetoothd・bt・`_bluetooth` の account・`CONFIG_DRIVER_USB_BT`・`intelbt-firmware` が入る）か、`config/current-uat.mk` の UAT の image。T1-502 の PASS の後の tree で作る（Q1）。
- 5330 を USB の image から素で起動（passthrough ではない）、AC 電源。Wi-Fi に接続しておく（共存を見るため）。
- 機器（design §9 の「情報のお願い」、受け入れ 3 は両方が要る）: BR/EDR（従来型）のキーボードかマウス 1 台、LE のキーボードかマウス 1 台。機種名を Q1 に知らせる（記録のため）。
- 何かが ✘ の時の材料（Q1 が SSH で取る、ユーザーの手は要らない）: `bt show`・`bt devices`・`bt status`・`bt bonds` の出力、画面の写真か PNG。詳しい記録が要る時は Q1 が `sudo service stop bluetoothd` → `sudo /sbin/bluetoothd -s /tmp/bt.snoop >/tmp/btd.log 2>&1 &` で daemon を記録つきで起こし直し、同じ操作をもう一度してもらう。`/tmp/bt.snoop` を host に写して `tshark -r` で読む（p005 の「外部の判定 S11」と同じ命令）。

## 確認の項目（ユーザー、5330）

✔ は動けば OK、✘ はその場の様子（何をしたら何が起きたか）を一言。

| # | 項目 | 手順 | 期待（受け入れの番号） |
| --- | --- | --- | --- |
| B1 | controller が上がる | 起動して login。Settings → Bluetooth を開く。（Q1 が SSH で `bt show`） | 頁に controller の名前と switch が出る。「Bluetooth is not available」や「firmware が要る」が出ない。`bt show` が `state=on` と BD_ADDR（1）。firmware が要ると出たら、出た file の名前を Q1 へ（5330 の CNVi・CNVR の id が intelbt の Solar の 3 つと違う、p003 i02） |
| B2 | 電源の switch | Settings の switch を off → on。system bar の Bluetooth の印も見る | off で一覧が消え bar の印が淡く、on で戻る（2） |
| B3 | 周りの機器 | 機器を pairing の mode にする（機器の説明書の手順） | Other Devices に機器の名前が出る（2） |
| B4 | BR/EDR の pairing | BR/EDR の機器の Pair を押す。キーボードなら画面の数字をキーボードで打って Enter、マウスなら確認の窓で Pair | My Devices に移り Connected。キーボード: Text Editor で文字が打てる。マウス: pointer が動き click・scroll が効く（2・3） |
| B5 | LE の pairing | LE の機器で B4 と同じ | 同じ（2・3） |
| B6 | 切断と接続 | My Devices の各機器で Disconnect → Connect | 切れて入力が止まり、繋ぐと戻る（2） |
| B7 | 押したままの key が残らない | Text Editor でキーボードの key を押したまま、機器の電源を切る | 文字の繰り返しがすぐ止まる（3、N14） |
| B8 | 機器の電源の入れ直し | 各機器の電源を切って 10 秒後に入れる | 自分で繋がり直る。繋がるまでの秒数を教えてください（LE の auto-connect の間隔は未確認、p005 i04） |
| B9 | 再起動 | 5330 を再起動して login | 機器を動かすと繋がる（pairing はやり直さない）（3） |
| B10 | 蓋と suspend | 蓋を閉じて 30 秒後に開け、解除 | 機器を動かすと繋がる（3） |
| B11 | 届かない所から戻る | 機器を持って部屋の外へ（10 m 以上）、1 分後に戻る | 繋がり直る（3） |
| B12 | Wi-Fi との共存 | 機器で操作しながら、Browser で頁を開くか大きい file を download | Wi-Fi が切れない。入力が目立って遅れない（体感）（design §2 F23） |
| B13 | 忘れる | My Devices の機器で Remove | 一覧から消える。もう一度 B4・B5 の pairing ができる（2） |
| B14 | CLI（任意） | Terminal で `bt devices`・`bt status` | Settings と同じ機器と接続の状態（4） |

## 判定と記録

- Q1 が結果を項目ごとに記録する（実機の証拠、QEMU の証拠と分ける）。B1〜B5 が ✘ なら、10/16 の判断で Bluetooth を OFF にし（release notes の review の comment の差し替えの文）、既知の問題に行を足す。B6〜B12 の一部が ✘ なら、既知の問題に書いて入れるかをユーザーが決める。
- p005 の i04（実機の門）は B4〜B8 の結果と、✘ の時の btsnoop の tshark の照合で代える（Q20 の推し）。p003 の i02 の 5330 の load と scan は B1・B3 で代える。
- この Phase の cleared の条件: B1〜B13 が ✔、または ✘ の項目がユーザーの決定で既知の問題に移されたこと。

## 未実施

全部（ユーザーの機器と 5330 の時間、Q1 の手配）。
