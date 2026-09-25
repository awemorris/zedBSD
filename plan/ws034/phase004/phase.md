<!-- awesome-plan project=zedbsd record=ws034p004 -->

# ws034-p004: USB列挙UAPI（`/dev/system`）と lsusb

Phase ID: `ws034-p004`
Parent: [WS034](../ws.md)
Status: **cleared**（q328-i01、2026-09-23）
Phase disposition: normal
Queue: q328（q328-i01）
実行: メインセッション

## 範囲

`/dev/system` に USB の列挙を足し、`userland/base/lsusb` を独自実装する。
UAPI の形は WS034 の決定（2026-09-23 ユーザー承認）に従う: index 指定の `_IOWR`、
bus/address/port path・VID/PID・class・bound driver 名、名前の表は同梱しない。

## 入れたもの

| 場所 | 内容 |
| --- | --- |
| `include/uapi/system.h` | `struct system_usb_device_info`（80 byte、固定幅のみ）、`KERN_SYSTEM_GET_USB_DEVICE`（`_IOWR('s', 14, ...)`）、速度の定数、`KERN_SYSTEM_USB_ROOT_HUB`。大きさと `vendor`・`driver` の offset を `_Static_assert` で固定 |
| `src/drivers/usb/usb.c`、`include/drivers/usb/usb.h` | `drv_usb_system_describe(index, info)`。**`usb_topology_lock` を持ったまま**探して写し、放してから返す（hot-plug で device が消えても、放した後の参照は残らない）。順序は bus ごとに root hub、続いてその device。port path は root hub から親をたどって作る（最大7段）。driver 名は interface に bind された driver を重複なしで `,` 区切り、入らない分は切る |
| `src/drivers/generic/system-device.c` | `KERN_SYSTEM_GET_USB_DEVICE` の handler。USB core の無い kernel では weak 参照が NULL になり `ENOENT`（p003 の PCI と同じ形） |
| `userland/base/lsusb/` | `lsusb [-tv]`。`Bus 000 Device 001: ID 46f4:0001` の形で bus・address 順。`-t` で port path・class・速度・driver、`-v` で descriptor の値。root hub は device 000 で `root hub` と付ける |
| 3つの config | `ZEDBSD_USER_PROGRAMS` に `lsusb` |

## 検証

| 検証 | 結果 |
| --- | --- |
| amd64 `disk-image`、pcat・pc98 `vmunix` | PASS。新しい warning なし |
| QEMU q35（KVM）、xHCI に storage・keyboard・tablet、EHCI に mouse | 4 device と 2 root hub。bus.address（0.1、0.2、0.3、1.1）と速度（5000M、480M）が QEMU の `info usb` と一致。VID:PID は QEMU の値（MSD 46f4:0001、HID 0627:0001）。driver は usb-storage・usb-hid |
| hot-unplug・replug（QMP の `device_del`・`device_add`） | 抜くと一覧から消え、挿すと同じ port に device 3 として戻り driver が bind される |
| 並行 | ゲストで `lsusb` を300回回す間に、host で同じ device を5回抜き差し。失敗0、kernel の異常なし |

証拠: `evidence/`。

## 気づいたこと

- bus 番号は kernel の番号（0 から）。Linux の `lsusb` は 1 から数えるが、kernel の log（`usb0:`）と合わせた。
- port は **controller の root port 番号**である。qemu-xhci では USB3 の port が 1〜4、USB2 の port が 5〜8 で、
  QEMU の `info usb` の port 番号（2、3）とは数え方が違う（USB2 の keyboard が 6、tablet が 7）。
- root hub は kernel が作る仮の device で、descriptor は class 09 だけが入っている（VID:PID 0000:0000、bcdUSB 0）。
- 外付け hub の下の device は、今の USB core が root port の device しか作らないので試せない（hub の driver が無い）。
- descriptor の 16 bit の値は、device から届いた little endian のままである（kernel の他の箇所と同じ）。
  big endian の platform（sun4u）はサポートから外れている。
- 実機では試していない。
