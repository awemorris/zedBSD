<!-- awesome-plan project=zedbsd record=ws035-usb-hub-design -->

# USB hub の設計（ws035-p046）

2026-09-24。ユーザー指示「USB ハブドライバは作成したいので、phase を作ってください」。

## 今の USB core（`src/drivers/usb/usb.c`）

- 列挙は **root port だけ**: `drv_usb_hcd_root_hub_changed()` が root port の状態を HCD の `root_hub_control`（hub class の要求）で読み、
  `enumerate_port(bus, port, status)` が `parent = bus->root_hub` の device を作る。port ごとの状態（observed・connected・enabled・
  接続の世代）は `bus->ports[]`。`find_port_device()` は root の子だけを探す。
- 子を持つ device は無いので、device を壊すとき子を先に壊す仕組みも無い。
- xHCI の `device_enable` は device の port を root port とみなし、speed を root port の PORTSC から読む。slot context の
  route string・TT・Hub の欄は 0。

## 決定

### D1. core に「hub の port の集まり」を一般化する

- hub の port を読む・変化を消す・reset する 3 つの操作を `struct drv_usb_hub_ops` として公開する。root hub は今の
  `root_hub_control` をこの形に包み、外の hub は hub driver が control 転送で実装する。
- port ごとの状態を、bus ではなく **hub の device ごと**に持つ（`struct usb_hub_state`: ops、context、port の数、port の状態の配列）。
  root hub の device も同じ形を持つ。
- 今の root の走査（`drv_usb_hcd_root_hub_changed` の中身）を `hub_scan_locked(bus, hub)` にし、root と外の hub で共用する。
  `enumerate_port` は `(bus, parent hub, port, status)`、`find_port_device` は `(bus, parent, port)`。

### D2. 外の hub の登録と走査

- class driver の `attach` は core が topology の lock を持ったまま呼ぶので、attach の中で走査はできない。hub driver は attach で
  worker thread を作り、worker が:
  1. hub descriptor を読み、各 port に電源を入れ（`SET_FEATURE(PORT_POWER)`、`bPwrOn2PwrGood` を待つ）、
  2. `drv_usb_hub_attach_ports(device, ops, context, port_count, ...)` で core に port を登録し、
  3. interrupt endpoint の status change の報告を待ち（無ければ周期で）、`drv_usb_hub_changed(device)` で走査させる。
- `drv_usb_hub_attach_ports`・`drv_usb_hub_changed` は topology の lock を **try** で取り、取れなければ `EBUSY`（worker は少し待って
  やり直す）。core が hub を壊している最中に worker が lock を待ち続けると、detach が worker の終わりを待って deadlock するため。
- hub の device を壊すとき、core は**子を先に（再帰で）壊し**、hub の状態を外してから hub の interface の driver を外す。
  hub driver の detach は worker を止めて待つだけで、core の hub の状態には触れない。

### D3. HCD へ知らせること

- device の speed は core が hub の port の状態から決める（root でも同じ）。HCD は root の子でない device の speed を PORTSC から読まない。
- xHCI の slot context:
  - **Root Hub Port Number**: 祖先を辿り、root hub の子の port。
  - **Route String**: root の子の hub の port から下へ、段ごとに 4 bit（15 を超える port は 15）。
  - **TT**（LS/FS の device が HS の hub の下にいるとき）: 最も近い HS の hub の slot と、その hub のどの port から入ったか。MTT の hub なら MTT。
  - hub の device には、hub driver が descriptor を読んだ後に **Hub=1・port の数・TT Think Time・MTT** を設定する。
    新しい任意の HCD 操作 `hub_configure(hcd, device, ports, multi_tt, think_time)`（xHCI は Evaluate Context）。
- UHCI（QEMU の FS の usb-hub）は address で届くので追加は要らない見込み。EHCI の split 転送（HS の hub の下の FS/LS）は後回し
  （QEMU に HS の hub が無く、試せない）。USB 3 の hub（SuperSpeed の hub descriptor と port の状態）も後回し。

### D4. 範囲外にする（最初の実装）

- hub の下の device の reset（`drv_usb_device_reset` は非 root で `ENOTSUP` のまま）。usb-storage の回復は root の device だけ。
- USB 3 hub、EHCI の TT、hub の過電流の回復、選択的 suspend。

## 試験

- QEMU: `-device usb-hub,bus=xhci.0,port=4` の下に `usb-kbd`・`usb-storage`・`usb-net`（`port=4.1` 等）。`lsusb -t` の木、
  keyboard の入力、storage の読み、network の DHCP。hub ごとの hot-plug・unplug（子が先に消える）。
- 2 段の hub（`port=4.2` に hub、その下に device）で route string。
- UHCI（`piix3-usb-uhci`）の下の hub。
