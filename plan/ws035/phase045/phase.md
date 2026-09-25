<!-- awesome-plan project=zedbsd record=ws035p045 -->

# ws035-p045: USB CDC-ECM の送信の列と受信の取りこぼし

Phase ID: `ws035-p045`
Parent: [WS035](../ws.md)
Status: **cleared**（q345-i01、2026-09-24。NCM も実機で直した）
Phase disposition: normal
Queue: q345（q345-i01）
実行: メインセッション

## なぜ

ws035-p039・ws034-p046 で見つけた。

- **送信**: `src/drivers/usb/usb-cdc-ecm.c` は送信の URB が 1 本で、使用中の間の `ecm_transmit()` は packet を捨てて
  `ENOBUFS` を返す。1 MiB を受け取る間に数百回（TCP の ACK の多く）が捨てられる。TCP は再送で耐えるが、FIN の最初の送信が
  捨てられて close が 1 秒遅れる、などが起きる。
- **受信**: 10 回中 1 回、相手の 1440 byte の segment がゲストに受け取られず、TCP の再送（約 1.5 秒）を 45 回待って
  1 MiB に 69 秒かかった。どこで落ちるか（受信の URB の再投入の間、packet の pool、network worker）は調べていない。

## 範囲

- 送信: 数個（packet の pool は全体で 32 なので 4〜8）の packet を持てる送信の列を置き、完了ごとに次を出す。
  あふれたときだけ捨てる。detach・close で列を空にする。
- 受信: 取りこぼしの場所を特定する（計数を足して pcap と突き合わせる）。受信の URB を複数にするか、再投入を早める。
- NCM（`usb-cdc-ncm.c`）が同じ作りなら同じ扱いにする（範囲に入れるかは調べてから決める）。

## 受け入れ

- ECM の上の 1 MiB の `fetch` を 20 回続け、全部が数秒以内で終わり、`ue0` の TX の drop が 0（または列があふれた回数だけ）。
- 既存の ECM の QEMU 試験（DHCP、ping、detach・reconnect）が退行しない。

## 結果（q345-i01、2026-09-24）

### 取りこぼしの場所

計数で切り分けた。10 回の 1 MiB の `fetch` の後、`ue0` の **RX errors は 1、TX dropped は 3697**（送信 約 9280 回のうち）。
受信は落ちておらず、**送信（大半は TCP の ACK）が URB 使用中に捨てられていた**。ACK が届かない相手（slirp）は再送の timeout
（約 1.5 秒）まで待つので、「受け取られない segment」に見えていたのは実は ACK の欠落だった。ws034-p046 の「受信の取りこぼし」の
見立ては誤りだった。

### 直したもの（`src/drivers/usb/usb-cdc-ecm.c`）

- **送信の列**: 送信の URB が使用中の間、最大 8 個（`ECM_TX_QUEUE_MAX`。packet の pool は全体で 32）の frame を adapter が持ち、
  送信の完了ごとに先頭を出す。あふれたときだけ `ENOBUFS` で捨てる。submit が失敗した frame は捨てて次へ進む（列が止まらない）。
- 送信の本体を `ecm_tx_submit()` に切り出し、`ecm_transmit()` と完了の処理が共有する。
- stop（close・detach）で、URB を止めた後に列を空にする（`ecm_tx_queue_free()`）。

### 検証（QEMU 10.0.11、KVM、`usb-net` を boot disk と同じ xHCI）

| 検証 | 修正前 | 修正後 |
| --- | --- | --- |
| 1 MiB の `fetch` 10 回 | 2.6〜71 秒、TX dropped 3697 | 0.15〜0.24 秒（初回 1.2 秒）、TX dropped 0 |
| 続けて 20 回 | — | 20 回とも cksum 一致、最遅 0.252 秒、TX dropped 0 |
| detach（QMP `device_del`）→ attach | — | `ue0` が消えて戻り、DHCP・1 MiB の取得が通る |
| 転送中の detach → attach | — | panic なし、新しい `ue0` で DHCP・取得が通る |
| amd64・pcat の CI kernel | — | warning 0 |

### 残り

- NCM は下の「追記」で、実機を使って直した。
- RX errors が接続（device の世代）ごとに 1 つ数えられる。最初の受信か何かの状態と見られるが調べていない。
- ECM の host fixture（`run-usb-cdc-ecm-driver-test.sh`）は変更前から link できない（`kern_malloc` 未定義）。

## 追記: NCM（2026-09-24、ユーザーが実機を host に挿した）

ユーザーが RTL8156（`0bda:8156`、SuperSpeed）を host に付け、QEMU の USB passthrough で使ってよいと指示した。
`/dev/bus/usb/002/002` を一時的に書込み可にし（`sudo chmod 666`）、`-device usb-host,bus=xhci.0,hostbus=2,hostaddr=2`
でゲストへ渡した。host では r8152 が外れる（host の主経路は `enp94s0f0` なので影響なし）。
ゲストは実 LAN の DHCP（10.0.0.1）から address を取り、host（10.0.10.2）の HTTP から取得する。

`src/drivers/usb/usb-cdc-ncm-net.c` に ECM と同じ送信の列（`NCM_TX_QUEUE_MAX` 8、`ncm_tx_submit()`、完了で次を出す、
stop で `ncm_tx_queue_free()`）を入れた。NTB の sequence は submit が成功したときだけ進める（従来どおり）。

| 検証（実機 RTL8156、QEMU passthrough、KVM） | 修正前 | 修正後 |
| --- | --- | --- |
| attach、DHCP、ping host | 通る | 通る |
| 1 MiB の `fetch`（10 回 / 20 回） | 10/10 一致、0.10〜1.29 秒、**TX dropped 6429（約 55%）** | 20/20 一致、0.14〜0.33 秒、**TX dropped 0** |
| 8 MiB の `fetch` | — | 1.43 秒（約 47 Mbit/s）、cksum 一致、drop 0 |
| detach（QMP `device_del`）→ attach | — | `ue0` が消えて戻り、DHCP・取得が通る |
| amd64・pcat の CI kernel | — | warning 0 |

### 気づいたこと（範囲外）

- **`net dhcp ue0` の直後にもう一度 DHCP が走る**（networkd が link up の通知で再設定すると見られる）。その間の `connect()` は
  すぐ失敗する（`fetch: cannot connect`）。数秒待つと通る。networkd の振る舞いで、driver の問題ではない。夜間の報告に記録。
