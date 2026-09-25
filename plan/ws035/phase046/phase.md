<!-- awesome-plan project=zedbsd record=ws035p046 -->

# ws035-p046: USB hub driver

Phase ID: `ws035-p046`
Parent: [WS035](../ws.md)
Status: **cleared**（q365-i01、2026-09-24）
Phase disposition: normal
Queue: q365（q365-i01）
実行: メインセッション

## 経緯

2026-09-24 ユーザー指示「USB ハブドライバは作成したいので、phase を作ってください」。hub の先の device は列挙されなかった
（QEMU の UHCI で 3 つ目の device が見えない、実機の hub・dock も同じ）。設計は [design.md](design.md)。

## 変更

- USB core（`src/drivers/usb/usb.c`、`include/drivers/usb/usb.h`）
  - port の状態を bus ではなく hub の device ごとに持つ（`struct usb_hub_state`）。root hub も外の hub も同じ
    `hub_scan_locked()` で走査する。`enumerate_port`・`find_port_device` は親の hub を取る。
  - 外の hub の登録 `drv_usb_hub_attach_ports()` と変化の通知 `drv_usb_hub_changed()`（topology の lock は try。取れなければ
    `EBUSY`）。port の操作は `struct drv_usb_hub_ops`（port_status・clear_feature・port_reset）。
  - device を壊すとき子を先に（再帰で）壊す。段の上限 5（`USB_HUB_TIER_MAX`）。
  - HCD の任意の操作 `hub_configure`（hub の port の数・MTT・TT think time を知らせる）。
- xHCI（`src/drivers/pci/pci-xhci.c`）: slot context に route string・root port・TT（最も近い HS の hub の slot と port）・
  Hub/NumPorts/TTT/MTT を書く。speed は core が hub の port から決めたものを使う（root の子でなければ PORTSC を読まない）。
  `xhci_hub_configure` は slot だけの Configure Endpoint。
- hub driver（新規 `src/drivers/usb/usb-hub.c`、`include/drivers/usb/usb-hub.h`）: interface class 9 に bind。attach が
  kthread を作り、hub descriptor を読んで port に電源を入れ、core に port を登録し、interrupt endpoint の変化の報告
  （250 ms の timeout と周期の走査）で `drv_usb_hub_changed` を呼ぶ。port reset は `SET_FEATURE(PORT_RESET)` と
  `C_PORT_RESET` の待ち。detach は worker を止めて待つだけ。
- 構成: `CONFIG_DRIVER_USB_HUB`（i386・amd64、既定 y）、`config/drivers/usb.drivers`、`platform/{amd64,pcat}/vmunix.mk`、
  `src/kern/platform/pcat.c` の登録。

## 検証（QEMU）

| 試験 | 結果 |
| --- | --- |
| xHCI の hub（8 port）の下に keyboard・tablet・storage | 3 つとも列挙され driver が付く（`lsusb -t` の木） |
| hub の下の storage を 16 MiB 読む | host と同じ cksum（490333905） |
| hub の port 4 に keyboard を hot-plug | 列挙され usb-hid が付く |
| 2 段の hub（0-6-5 の hub の下に tablet） | 0-6-5-1 に列挙（route string） |
| QMP で hub を抜く | 子（8,7,6,5,4,3）が先に、最後に hub（2）が切り離される。抜いた瞬間に usb-storage の BOT の error が一時的に出るが、切り離しで終わる |
| 別の hub を差し直し、port 2 に keyboard | 0-6 に usb-hub、0-6-2 に usb-hid |
| UHCI（`piix3-usb-uhci`）の hub の下に keyboard・storage | 両方列挙、16 MiB の cksum 一致（[uhci-hub.txt](evidence/uhci-hub.txt)） |
| CI の kernel（amd64・pcat・pc98） | warning 0 |

## 範囲外（設計の D4）

- hub の下の device の reset（`drv_usb_device_reset` は非 root で `ENOTSUP`）。usb-storage の回復は root の device だけ。
- USB 3 hub（SuperSpeed の hub descriptor）、EHCI の split 転送（QEMU に HS の hub が無く試せない）、過電流の回復、選択的 suspend。
- 実機の hub・dock での確認はしていない。
