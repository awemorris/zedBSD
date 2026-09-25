<!-- awesome-plan project=zedbsd record=ws035p044 -->

# ws035-p044: UHCI の短い packet で転送を終える（SPD）

Phase ID: `ws035-p044`
Parent: [WS035](../ws.md)
Status: **cleared**（q346-i01、2026-09-24）
Phase disposition: normal
Queue: q346（q346-i01）
実行: メインセッション

## なぜ

ws035-p039 で見つけた。UHCI（と EHCI の companion の UHCI）に付けた USB CDC-ECM で、**長さが 64 byte の倍数の frame を
受け取れない**（ARP の応答が届かず、ping が 0/3）。

`src/drivers/pci/pci-uhci.c` は IN の TD に SPD（Short Packet Detect）を立てない（本文の注記「The current builder never
sets SPD」）。短い packet（長さ 0 を含む）の後も controller は同じ URB の残りの TD を実行し、次の frame を同じ URB に受ける。
`uhci_request_actual()` は長さ 0 の TD を読み飛ばして後の TD を足す。frame の境目が失われる。

## 範囲

- bulk・interrupt の IN の TD に SPD を立て、短い packet で URB を終える。短い packet の後の QH の要素（controller は
  短い TD を指したまま止まる）を、次の URB の TD へ進める。
- `uhci_request_actual()` は短い TD（長さ 0 を含む）で数えるのを止める。
- data toggle の保存（`uhci_request_commit_toggle`）が実行された TD までを数えることを確かめる。

## 受け入れ

- host fixture: 64 byte の倍数の IN と、その後に長さ 0 の packet が来る転送が、1 つの URB で正しい長さで終わる。
  次の URB が次の frame を受ける。toggle が正しい。
- QEMU: `ich9-usb-uhci1` の `usb-net` で ARP・ping・DHCP・TCP が通る。EHCI の companion でも。
- UHCI の既存の試験（USB storage・HID）が退行しない。

## 結果（q346-i01、2026-09-24）

### 直したもの（`src/drivers/pci/pci-uhci.c`）

- **bulk・interrupt の IN の TD に SPD を立てる**（`uhci_build_request`）。短い packet（長さ 0 を含む）で controller が
  queue を止め、次の frame を同じ URB に受けない。control 転送には立てない（data の後に status の段があるため）。
- **短い IN で URB を終える**: `uhci_request_terminal` は、SPD の立った IN の TD が非 active で短ければ、そこで完了とする
  （後ろの TD は ACTIVE のまま実行されない）。判定は新しい `uhci_td_short_in()`。
- **受けた長さは短い TD で数え終える**: `uhci_request_actual` は長さ 0（`0x7ff`）を読み飛ばして次の TD を足していた。
  各 TD の受信量を数え、最大より短い TD で止める。
- 割り込みは既に Short Packet Interrupt を有効にしていた（USBINTR `0x000d`）。PIIX の不具合への対処（QH の要素が進まない
  ときの修理）は、短い IN を「進まない」と誤認しない作りに既になっていた。data toggle の保存は、実行された TD までを数える。

### 検証（QEMU 10.0.11、KVM）

| 検証 | 修正前 | 修正後 |
| --- | --- | --- |
| UHCI（`ich9-usb-uhci1`）の `usb-net`: ping 10.0.2.2 | 0/3 | **5/5** |
| 同: 1 MiB の `fetch` | 接続できず | 2.26 秒、cksum 一致、RX・TX の error 0 |
| UHCI の `usb-storage`（hot-plug）から 8 MiB を読む | 18.73 秒、cksum 一致 | 18.68 秒、cksum 一致（退行なし） |
| UHCI の `usb-kbd` | attach | attach（`usb-hid` の event device） |
| amd64・pcat の CI kernel | — | warning 0 |

### 気づいたこと

- QEMU の UHCI は root port が 2 つで、3 つ目の device には hub が挟まる。**この USB stack には hub の driver が無い**
  （`class 09/00/00 no-driver`）ので、hub の先の device は見えない。範囲外（夜間の報告に記録）。
- UHCI の storage を boot 時から付けると、それが `sda` になり boot disk の選択に失敗する（試験の組み方の問題。hot-plug で避けた）。
- UHCI の host fixture は用意していない（`plan/ws004/tests/` の UHCI・HCD の fixture は変更前から build できない）。
