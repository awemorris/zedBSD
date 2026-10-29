<!-- awesome-plan project=zedbsd record=ws315-p003 -->
# ws315-p003: 5330 の実機（RTL8156 の vendor mode）

Status: cleared（2026-10-30 06:15 P3 q1694-i01: 5330 の image の試験 img1 と調整 k5・k6。vendor mode send 2372.0・recv 2338.3、同時 send 2346.5・recv 1442.9（調整の前 1037.1、Linux 2345.7・2340.9）、ping・MTU・down/up・他の USB の device の attach PASS。camcap の frames=0 は WS282 の範囲（Q1 の判断）。抜き差し・相手の port の上げ下げはユーザーの拾える試験へ。前の記録: 2026-10-30 05:43 P3: 残りの camcap・送受信の同時は image を作り、5330 への SSH が security の判定で止まってユーザーの判断待ち。前の記録: 2026-10-30 05:27 P3: k4 で vendor mode の受け入れの主な項目を満たした、残りは下の未実施。前の記録: 2026-10-30 04:16 P3 q1694-i01: k1 FAIL（SSH 無し）、5330 の電源の入れ直し待ち）
Parent: [WS315](../ws.md)
Queue: q1694 / q1694-i01（P3）
依存: p002

## 試験

- `plan/ws315/tests/5330-rtl.sh KERNEL NAME [SECONDS]`（MODE=--nvme-kernel、CFG_ADD で boot の語、出力 build/ws315-hw/NAME/summary.txt）。
- 受け入れ: send・recv 30 s（相手 10.0.10.2）を NCM（`usb.rtl815x=off`）と Linux r8152（WS221 p014: 2353.4・2353.6）と並べる、ping 1472、開いたままの MTU の変更、down・up、
  USB の他の device（Broadcom の ccid・AX211 の bt・camera）の非退行、抜き差し（5330 は遠隔で抜けない: 未実施になる見込み、ユーザーの拾える試験の一覧へ）。

## 結果（5330）

| run | kernel | 結果 |
| --- | --- | --- |
| k1（2026-10-30 04:03、`5330-rtl.sh build/ws315-5330/vmunix k1 30`、MODE=--nvme-kernel） | 5534277a3 の tree | FAIL: 5 分 SSH 無し、10.0.30.3 は ping も無し、NCM の時の MAC の host も無し。5330 は電源の入れ直し待ち（Q1 がユーザーに依頼）。原因は未確定 |

- k1 の後の直し（16122571a）: attach の待ち（autoload・PHY の状態・MAC の reset）を FreeBSD と同じく時間切れでも続ける（10 s、log）、attach・open の手順ごとの失敗の log、
  link を interrupt が告げない時に worker が 1 秒ごとに PHYSTATUS を読む（carrier が立たず DHCP が始まらない、を候補として）。
- 試験の script（7712411ab）: 前の起動の /var/log/kernel.log.old の driver の行を集める（届かなかった起動の原因を、次の起動で読む）。出力は build/ws315-hw/NAME。
| k2-ncm（04:5x、16122571a、`CFG_ADD=usb.rtl815x=off`） | NCM | 5330-run PASS: token で NCM（urbs=4/2、link 2500）、send 2062.8・recv 1473.7 Mbit/s、ping 1472 3/3、MTU 9000 は NCM の上限（1504）で拒否（想定どおり）、1500 に戻して ping 3/3。終わりの zedBSD の reboot の後に Debian が 5 分戻らず（cfg の行を戻せず: NVMe の zedbsd.cfg に `usb.rtl815x=off` が残る）。5330 は再び到達不能 |

- k1 の原因（k2-ncm が集めた /var/log/kernel.log.old）: `usb-rtl815x: ue0 chip=RTL8156B version=0x7410 mac=6c:1f:f7:06:14:8a mtu_max=9000 urbs=4/4 rx_block=49152`
  （attach は成功、MAC は Debian と同じ）、その後 `ue0 open failed (3)`（EINVAL）×5。原因: RX・TX の URB に付けた `drv_usb_urb_reserve_transfer` が同期の staging を作り、
  callback 付きの `drv_usb_urb_setup` を EINVAL で拒む。予約を外した（次の commit）。5330 の RTL8156 は RTL8156B（0x7410）だった。

- k2-ncm の「Debian が戻らない」の原因: 試験の script が SSH（ue0 を通る）の上で `ifconfig ue0 down` を流し、session が切れて up が走らず、5330-run の最後の reboot（SSH）も届かなかった。
  p014（xHCI の depth）ではない。直し 02e29e5cf（down・up を 5330 の上で detach して流す）。5330 は 2 回、ユーザーが電源を入れ直した。

| run | kernel | 結果 |
| --- | --- | --- |
| k3-ncm（05:2x、2d208a345 の tree、`CFG_ADD=usb.rtl815x=off`） | NCM（urbs 4/2） | 5330-run PASS、終わりの reboot から Debian に戻った（Q1 の指示: depth > 1 の teardown を伴う再起動を実機で 1 回）。send 1971.0・recv 1479.9、ping 1472 3/3、down・up（rc 0）の後の ping 3/3、MTU 9000 は NCM の上限で拒否（想定どおり）、RX errors 6・TX dropped 456639（queue の満杯の背圧） |
| **k4-rtl**（05:4x、同じ kernel、token なし） | **vendor mode**（RTL8156B 0x7410、urbs 4/4、RX block 48 KB） | 5330-run PASS、reboot から Debian に戻った。**send 2372.2・recv 2319.1 Mbit/s**（30 s、相手 10.0.10.2）、link up 2500 full-duplex、ping 1472 3/3、開いたままの MTU 9000 への変更（rc 0）と 1500 に戻した後の ping 3/3、down・up（rc 0）の後の ping 3/3、RX errors 0、TX dropped 177154（背圧） |

| driver | send Mbit/s | recv Mbit/s | 記録 |
| --- | --- | --- | --- |
| zedBSD NCM（WS221 p014） | 2097.0 | 1449.0 | WS221 p014 |
| zedBSD NCM（この Phase、k3-ncm） | 1971.0 | 1479.9 | build/ws315-hw/k3-ncm |
| **zedBSD vendor mode（k4-rtl）** | **2372.2** | **2319.1** | build/ws315-hw/k4-rtl |
| Debian r8152（WS221 p014） | 2353.4 | 2353.6 | WS221 p014 |

- 受けは NCM の 1.57 倍で Linux の 98.5 %、送りは Linux の 100.8 %。
- USB の他の device の非退行（k4 の kernel log）: webcam は uvc、Broadcom 58200 は usb-ccid、AX211 は usb-bt で k3-ncm と同じ。i915 の survey の diff は PASS（steady の register の差 0）。
- 未実施: camcap（NVMe の userland に無い、image の試験が要る）、send と recv の同時、相手の port の上げ下げ、抜き差し（5330 は遠隔で抜けない、ユーザーの拾える試験の一覧へ: Q1）、
  9000 の相手との jumbo の ping（相手が無い）、RTL8156（A、0x7020/0x7030）と 0x7400・0x7420 の実機（手元は 0x7410 だけ）。

## 残りの試験（2026-10-30 再開）

- 試験の script（c3957e2bf、未試験）: `5330-rtl.sh` が send・recv の後に send と recv を同時に流す（2 本目の `perf-peer.py` を port 5312 に、`both` の行と TCP-PERF の 2 行）。
  `config-5330-rtl.mk` に camcap を足した（`CAMERA=1` で 10 frame）。
- image: `yes | make ... firmware-licenses`（2026-10-30 ユーザー「試験に関しては私はすべてのライセンスに同意しておりますので、自動でいいです」）の後に
  `make ZEDBSD_CONFIG=plan/ws315/tests/config-5330-rtl.mk BUILD=build/ws315-img disk-image` → `build/ws315-img/hdd-image.img`（rc 0、check OK。warning は外部 package の openssh・perl などだけ）。
  worktree に `build/amd64/sysroot` が無く、main の生成物（5 MB）を複写した（AGENTS §7 の複写）。
- 5330 の Debian の uname の確かめ（`ssh solaris10-man uname -a`）が auto mode の security の判定（Production Reads）で止まった。回避せず Q1 へ返し、ユーザーの判断待ち。5330 には触れていない。
  承認の後: `CAMERA=1 MODE=--nvme-image sh plan/ws315/tests/5330-rtl.sh build/ws315-img/hdd-image.img img1 30`（p004 を含めるなら image を作り直す）。
- 相手の port の上げ下げ: centris（10.0.10.2）の enp94s0f0 は switch の先（同じ segment に 10 の neighbor）で、centris を down しても 5330 の RTL8156 の link は落ちない。
  実施しない。switch 側の cable の抜き差しとしてユーザーの拾える試験の一覧へ（Q1 が master.md に入れた）。

## image の試験と同時の送受信の調整（2026-10-30 再開、5330 への SSH の承認の後）

- 承認: 2026-10-30 ユーザー「5330へのSSHは許可します。」（Q1 経由）。試験の前に `ssh solaris10-man uname -a` → `Linux chaos 6.19.13+deb13-amd64 … Debian`（各 run の前にも確かめた）。
- image の作り直し（main を merge した f03d745e3 の tree、p004・p005 入り）: `flock /tmp/zedbsd-image-build.lock make -j16 ZEDBSD_CONFIG=plan/ws315/tests/config-5330-rtl.mk BUILD=build/ws315-img disk-image`
  → rc 0、`check-amd64-native-image … OK`、warning 0（build/ws315-img3.log）。

| run | kernel・userland | 結果 |
| --- | --- | --- |
| img1（`CAMERA=1 MODE=--nvme-image sh plan/ws315/tests/5330-rtl.sh build/ws315-img/hdd-image.img img1 30`） | image（f03d745e3） | 5330-run PASS（zedBSD up 21 s、survey の diff PASS、Debian に戻った）。RTL8156B 0x7410 attach、link up 2500 full-duplex。send 2371.4・recv 2350.4、**同時 send 2351.1・recv 1037.1**。ping 1472 3/3、MTU 9000（rc 0）→ 1500（rc 0）の後 ping 3/3、down・up（rc 0、5330 の上で切り離し）の後 ping 3/3。RX errors 0、TX dropped 405540。camcap frames=0 |
| 基準: Linux の同時（`sh plan/ws315/tests/5330-linux-both.sh both1 30`、新しい script） | Debian 6.19.13、r8152、2500 | 同時 send 2345.7・recv 2340.9（build/ws315-linux/both1） |
| k5（`CAMERA=1 MODE=--nvme-kernel … build/ws315-k5-vmunix k5 30`） | 調整 1: 小さい frame（128 byte 以下）に TX queue の余地 +32 | PASS。send 2372.6・recv 2354.8、同時 send 2351.6・recv 1072.7（効果は小さい）。TX dropped 319333。camcap rc 3 |
| **k6**（同じ形、`build/ws315-k6-vmunix k6 30`） | 調整 2: 純粋な ACK を TX queue の data の前に出す（k5 の余地も残す） | **PASS。send 2372.0・recv 2338.3、同時 send 2346.5・recv 1442.9**。ping 1472・MTU 9000 と戻し・down/up の後の ping すべて 3/3、RX errors 0、TX dropped 342666。Debian に戻った |

- 同時の受けの原因の解析: zedBSD の TCP の受けの window は 128 KB（`src/kern/net/tcp.c` の `TCP_RECEIVE_BUFFER`）。1037 Mbit/s は 128 KB / 約 1.0 ms の往復に当たる。
  送りの data が TX queue（128 frame ≈ 190 KB ≈ 0.65 ms）と 4 本の 16 KB の block を満たし、受けの ACK がその後ろで待って往復が延びていた。
  k5 の「断られる ACK」の仮説（ENOBUFS → TCP の再試行が 10 ms 待つ）は効果が小さく主因ではなかった。k6 で ACK を data の前に出し 1.39 倍（1037 → 1443）。
  Linux は受けの buffer を自動で数 MB に広げるので往復の延びが速さに出ない。残りの差（1443 対 2341）は TCP の受けの window（stack の側、WS315 の範囲の外）が主と見る。
  候補（Q1 へ、記録だけ）: TCP の受けの buffer の自動調整か既定の拡大、tcp-perf の SO_RCVBUF（1 MB まで）での確かめ。
- 調整の内容（`src/drivers/usb/usb-rtl815x.c`）: `rtl_tx_is_pure_ack`（IPv4・IPv6（拡張 header なし）の TCP、payload 0、SYN・FIN・RST なし、IPv4 の fragment は除く）、
  純粋な ACK は queue の頭の ACK の列の最後に入れ、他の frame は末尾（同じ connection の data・FIN の順は保つ。ACK が自分の data を追い越しても相手は古い ACK・window として扱う、RFC 9293 3.10.7.4）。
  128 byte 以下の frame は queue の上限 128 を 32 超えてよい（32-bit の板は 16 + 4）。build warning 0、`plan/ws315/tests/run-rtl815x-frame-test.sh` PASS（frame の組み立ては変えていない）。
  ACK の判定の host 試験は無い（static の関数、実機の k6 で同時の値が上がった事で確かめた）。
- USB の他の device（k6 の kernel log）: uvc（0c45:6d1d、interface 0・1）、usb-ccid（Broadcom 58200）、usb-bt（AX211、8087:0033）、usb-rtl815x が attach。img1・k5 も同じ。
- camcap: frames=0、resubmit_failures=1、`camcap: CAM_DEQUEUE_BUFFER: Input/output error`（rc 3、k5・k6）。uvc の attach と format（MJPEG 640x480、payload 3072、buffer 4）は正常。
  5330 の webcam の stream は前例が無く WS282（camera の枠組み、実装済み・試験未実施）の範囲（2026-10-30 Q1 の判断: WS315 の範囲の外、他の USB の device が attach すれば p003 は可）。出力と kernel log の path は Q1 に送った（WS282 の handover へ）。
- 試験の script の直し: camcap の全出力と camcap 自身の rc を残す、`/var/log/kernel.log.old` の節の区切り（sudo の prompt が `---` と同じ行になった）。
- 未実施: 抜き差し・相手の port の上げ下げ（5330 は遠隔で抜けない、switch の先: ユーザーの拾える試験）、9000 の相手との jumbo の ping（相手が無い）、
  RTL8156（A）・0x7400・0x7420 の実機（手元は 0x7410 だけ）、RTL8157（5G、未購入）。同時の送受信の Linux との残りの差（上の候補）。
- 5330 の終わりの状態（06:15）: Debian で起動（`uptime` 0 min、k6 の run の最後の reboot から戻った）。NVMe の zedBSD: root・userland は img1 の image（f03d745e3 の tree、camcap 入り）、
  kernel は k6（f03d745e3 + 調整 2、この Phase の commit）、vmunix.old は k5 の kernel。zedbsd.cfg に CFG_ADD の行は無い（img1・k5・k6 は CFG_ADD なし）。
- 証拠: build/ws315-hw/{img1,k5,k6}/summary.txt・extra.txt、build/ws315-linux/both1/summary.txt、build/hw5330/logs/ws315-{img1,k5,k6}-*（kernel log は -dmesg.txt）。
  5330-run が作った plan/tools/hw5330/evidence/ws315-{img1,k5,k6}.md は commit していない（Q1 へ）。
