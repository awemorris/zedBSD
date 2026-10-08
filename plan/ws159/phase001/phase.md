<!-- awesome-plan project=zedbsd record=ws159-p001 -->

# ws159-p001: native のタッチパッドの設計

Status: cleared（2026-10-08 Q1 の判定（sweep-beta2-rc2 §2）: Q1 の ACK（2026-10-05）、後続の p003・p004 が cleared）（旧: cleared の判定待ち（2026-10-05 P1 generation17 / q713-i01。設計を書き、Q1 が「このまま p002 へ」と ACK（main に統合）。Q1 の委任: D3・D6 の移動・EVIOCGPROP・D8 の caps））
Disposition: normal
Parent: [WS159](../ws.md)
Queue: q713 / q713-i01（2026-10-05 ユーザーのクリックの回答「ベータ1」、Q1 の投入）。design-reviewer は省く（2026-10-05 ユーザー）。

## 目的

5330 のタッチパッドを PS/2 の互換の mouse ではなく、I2C-HID の Precision Touchpad（PTP）として読む。指ごとの位置・押し込み・指の数を
evdev の multitouch（protocol B）で出し、compositor のタッチパッドの層で tap・tap-drag・押し込み・2 本指のスクロールを作る。
この Phase は設計だけ。実装は p002〜p004、実機の確認は p005。

## 事実（2026-10-05、5330 の Linux で採った。`plan/ws159/tests/latitude5330-linux/`）

採取の許可: 2026-10-05 ユーザー「5330を起動しました。操作はすべて許可します。」（Q1 の中継）。

| 項目 | 値 | 出典 |
| --- | --- | --- |
| device | Synaptics 06CB:CE65。ACPI `\_SB.PC00.I2C1.TPD0`、`_HID` は `VEN_06CB`（NVS の `TPHD`）、`_CID` `PNP0C50` | `firmware_node/path`・`hid`、DSDT 99758 行 |
| bus | I2C1 = PCI 00:15.1 `8086:51e9`（Alder Lake PCH Serial IO I2C #1）、BAR0 は 64-bit 0x4017001000（4 KiB）、PM capability 0x80、idle は D3 | `lspci-lpss-i2c.txt` |
| I2C | 7-bit address 0x2c、Fast mode 400 kHz。Linux の clock は 133 MHz、HCNT:LCNT = 100:200、SDA hold TX:RX = 6:1 | `lpss-i2c-timing.txt`、`rebind-dmesg.txt` |
| HID descriptor | register 0x0020。`1e 00 00 01 99 02 21 00 24 00 40 00 25 00 17 00 22 00 23 00 cb 06 65 ce 02 04 00 00 00 00`: report descriptor の長さは 665、register は 0x21、input の register は 0x24（最大 64 byte）、output は 0x25（23）、command は 0x22、data は 0x23 | `dmesg-touchpad.txt` |
| report descriptor | 665 byte（`synaptics-06cb-ce65-rdesc.bin`、sha256 `c04a0ef6…`）。report 2 = 互換の mouse、**report 3 = Touch Pad**（Finger 5 個: Confidence・Tip・Contact ID 3 bit・X 0..1336・Y 0..760（16 bit、0.01 cm の単位）、Scan Time、Contact Count、Button 1）。feature 8 = Contact Count Maximum・Pad Type、feature 13 = Latency Mode、feature 7 = PTPHQA の blob（256 byte）、feature 4 = **Device Mode**、feature 6 = **Surface Switch・Button Switch** | `rdesc.txt` |
| Linux の初期化 | SET_POWER ON、RESET、report descriptor、GET_REPORT feature 8・8・7、SET_REPORT feature 13 = 0、**feature 4 = 3（PTP mode）**、**feature 6 = 3（surface と button を有効）** | `rebind-dmesg.txt` |
| evdev（Linux） | "Touchpad": props 0x05（POINTER・BUTTONPAD）、ABS_X 0..1336・ABS_Y 0..760（resolution 12/mm）、MT の slot 0..4、TOOL_TYPE、TRACKING_ID、keys BTN_LEFT・TOOL_FINGER・TOOL_QUINTTAP・TOUCH・TOOL_DOUBLETAP・TRIPLETAP・QUADTAP。別に "Mouse"（report 2） | `evdev-absinfo.txt` |
| 割り込み | `GpioInt(Level, ActiveLow, ExclusiveAndWake)` on `\_SB.GPI0`（INTC1055）。ACPI の pin は 327 = pad 233（CPU_GP_1）。DW0 は 0x80800102（RXINV、GPIO の input、IOxAPIC への route は無し）、DW1 の INTSEL は 0x33。Linux は intel-gpio の IRQ（親は IO-APIC の 14）を使う | `/sys/kernel/debug/gpio`・`pinctrl`、DSDT の `TPDM` = 0 の経路 |
| 割り込みなしの読み | input の register を読むと、データの無い時は長さ 0 が返る（10 回とも） | `poll-probe-1.txt` |
| PS/2 との関係 | DSDT の `HIDD` は、I2C-HID の `_DSM` を評価すると `PS2D = 1` にする。その後、`\_SB.PC00.LPCB.PS2M`（PNP0F13）の `_STA` は 0 を返す。firmware は「OS が I2C-HID を使ったら PS/2 の mouse は無い」と伝えている。Linux は psmouse も残す。操作中の 60 秒の記録で、Touchpad は 20070 event、PS/2 は 0 event（D7） | DSDT 77727・99528〜99600 行 |

## 設計

### D1. 全体の形

```
PCI 00:15.1 ─ lpss-i2c（DesignWare の I2C、p002）
                 └ i2c-hid（HID over I2C の protocol、ACPI の PNP0C50、p003）
                      └ hid（report descriptor の parser と PTP の状態機械、USB と共有、p003）
                           └ evdev の input device（MT protocol B、props POINTER・BUTTONPAD）
                                └ compositor の touchpad.c（tap・tap-drag・押し込み・スクロール、p004）
```

kernel は「指の位置と押し込みを正しく出す」だけを担い、gesture（tap を click にする等）は作らない。gesture は compositor の層で作る（Linux の libinput と同じ分け方）。

### D2. LPSS の DesignWare I2C（p002、`src/drivers/i2c/lpss-i2c.c`（新）・`include/drivers/i2c/i2c.h`（新））

- PCI の driver（`drv_pci_driver_register`）。対象は Alder Lake の 8086:51e8・51e9。他の世代の ID は表に足すだけで済む形にする。
- attach の手順: PMCSR を D0 にし、MEM と BME を立てる。BAR0 を map する（64-bit、4 GiB の上）。LPSS の private の reset（BAR の 0x204）を解き、`IC_COMP_TYPE`（0xFC）が 0x44570140 であることを確かめる。`IC_COMP_PARAM_1` から FIFO の深さを読む。
- clock: LPSS の I2C の入力は 133 MHz とする（5330 の Linux の clk_summary）。Fast mode の SCL の HCNT・LCNT と SDA hold は、ACPI の `FMCN`・`SSCN` が有ればそれを使う（5330 の I2C1 には無い）。無ければ I2C の規格の最小の時間（Fast mode: tHIGH 0.6 µs・tLOW 1.3 µs・tf 0.3 µs）から計算し、5330 では Linux と同じ 100:200 に近い値にする。
- 転送: 7-bit address、write と read の combined（restart）だけ。`IC_DATA_CMD` に FIFO の深さまで command を積み、`sched_sleep` で 1 tick（1 ms）ずつ待ちながら RX の FIFO を吸う。busy-wait はしない。controller の割り込み（PCI の INTx）は使わない（ベータ1）。TX_ABRT（NACK など）は EIO で返し、`IC_CLR_TX_ABRT` で戻す。
- 公開の口（kernel の内部）: `drv_i2c_bus_lookup_acpi(path)`（`\_SB.PC00.I2C1` などの ACPI の path から bus を引く。bus は attach の時に自分の ACPI の node（PCI の `_ADR` から）を覚える）と、`drv_i2c_transfer(bus, address, speed, write, write_length, read, read_length)`。I2C の client の driver はこれだけを使う。

### D3. ACPI の口の追加（p002、WS049 の `acpi-resource.c`・`acpi.h`）

- `drv_acpi_resources_walk` に 2 種類の resource を足す: `DRV_ACPI_RESOURCE_I2C`（I2cSerialBusV2、tag 0x8E type 1: slave address・速度・10-bit の印・resource source の path）、`DRV_ACPI_RESOURCE_GPIO_INT`（GpioInt、tag 0x8C type 0: pin・level/edge・active の向き・shared/wake・controller の path）。`struct drv_acpi_resource` に path（短い文字列）と pin を入れる field を足す。
- `_DSM` の評価は既存の `drv_acpi_evaluate` で行える（引数は UUID の buffer・整数・整数・package）。

### D4. I2C-HID（p003、`src/drivers/i2c/i2c-hid.c`（新））

- 発見: ACPI の namespace を歩き、`_CID` か `_HID` が `PNP0C50` で、`_STA` が present の device を拾う（`drv_acpi_walk`）。`_CRS` から I2C の address・速度・bus の path と GpioInt を取る。HID descriptor の register は `_DSM`（UUID `3cdff6f7-4267-4555-ad05-b30a3d8938de`、rev 1、function 1）で取る。この評価が firmware の `PS2D = 1` を起こす（D7）。
- 手順（HID over I2C 1.0）: HID descriptor（30 byte）を読む → SET_POWER ON → RESET（command 0x0100）→ reset の完了（input の長さ 0 の report）を最長 1 秒待つ → report descriptor を読む → hid の層で layout を作る → PTP なら feature を設定する: Device Mode（usage 0x0D/0x52）= 3、Surface Switch・Button Switch（0x57・0x58）= 1、Latency Mode（0x60）= 0。GET_REPORT・SET_REPORT は command の register と data の register を使う（Linux と同じ byte 列: 例 `22 00 34 03 23 00 04 00 04 03`）。
- input の読み（ベータ1 は割り込みを使わない sampling。D5）: input の register から wMaxInputLength byte を 1 回の read で読む。長さが 0 なら何も無い。長さが 2 より大きければ report（先頭が report ID）を hid の層へ渡す。
- 1 kernel thread（`kthread_create`）が 1 つの device を受け持つ。attach の失敗（NACK・descriptor の不正）は log に理由を出し、device を使わない（PS/2 の経路が残る）。

### D5. 割り込みの代わり（ベータ1 の決定）と後の割り込み

- ベータ1: **pad の RX の状態を MMIO で見る sampling**。GpioInt の controller が Intel の GPIO（`INTC1055` など）で、DSDT に Intel の reference の ASL の helper（`\_SB.GADR`・`\_SB.GGRP`・`\_SB.GNMB`）が有り、`TPD0` の pad（NVS の `GPDI` = group<<16 | pad）が分かる時に使う。attach の時に AML で DW0 の物理 address（`GADR(group, 2) + pad * 16`）を 1 回だけ求め、map する。thread は 4 ms ごとに DW0 の bit 1（GPIORXSTATE、pad の生の入力）を読み、0（active low で assert）の間だけ I2C の read をする。触っていない間の I2C の bus の負荷は 0 で、遅れは最大 4 ms＋1 回の転送（64 byte で約 1.6 ms）。
- 上の helper が無い platform: 8 ms ごとに input の register を読む（長さ 0 なら捨てる、5330 で確かめた振る舞い）。触っていない間も bus を使うので、指が離れて 1 秒経ったら 25 ms ごとに落とす。
- 後（ベータ1 の後、別の Phase）: Intel の GPIO の driver（`INTC1055` の `_CRS` の 4 つの community と IRQ 14、`GPI_IS`・`GPI_IE`。group ごとの offset も同じ ASL の `GPCL` の表にある）で、GpioInt を本当の割り込みにする。
- **HAL の API は変えない**。MMIO の map は既存の device mapping を使う。

### D6. hid の層の共有と PTP（p003）

- report descriptor の parser（今は `src/drivers/usb/usb-hid.c` の中の `drv_hid_report_layout_*`・`drv_hid_report_decode`）と、触る指の状態機械（`src/drivers/usb/hid-touch.c`）を、transport に依らない `src/drivers/hid/` へ移す（内容は変えない移動と、header の path の変更）。USB の HID の driver と I2C-HID の driver の両方がこれを使う。config の依存は `CONFIG_HID`（USB の HID か I2C-HID のどちらかが有効なら build する）。
- parser の追加: application collection の **Touch Pad**（0x0D/0x05）を Touch Screen と同じく指の collection として読む。同じ application の Button page の usage 1〜3 を pad の button（BTN_LEFT〜）として読む。feature report の Device Mode・Surface/Button Switch・Latency Mode・Contact Count Maximum の位置（report ID・bit offset・size）を layout に記録し、I2C-HID の driver が feature report の byte 列を作れるようにする。
- 状態機械の追加（hid-touch.c）: touchpad の形では、frame ごとに触っている指の数から `BTN_TOOL_FINGER`・`DOUBLETAP`・`TRIPLETAP`・`QUADTAP`・`QUINTTAP` のどれか 1 つを立て、button を `BTN_LEFT` として出す。Confidence が 0 の指（手のひら）は、Linux と同じく `ABS_MT_TOOL_TYPE = MT_TOOL_PALM` で出す。kernel は gesture を作らない。
- evdev の device: props に `INPUT_PROP_POINTER` と、Pad Type が clickpad（0）なら `INPUT_PROP_BUTTONPAD`。axis の resolution は HID の unit から計算する（5330 は 12 単位/mm、Linux と同じ）。名前は "<vendor>:<product> Touchpad"。
- kernel の input の層に **`EVIOCGPROP`** を実装する（UAPI には既に定義がある。`drv_input_device_register` の info に props を足す）。USB の touch screen には `INPUT_PROP_DIRECT` を付ける。

### D7. PS/2 との二重の入力

- firmware の契約（`HIDD` の `PS2D = 1` → `PS2M._STA` = 0）に従う。I2C-HID の touchpad の attach が成功した後、i8042 の driver に「ACPI の PNP0F13 の `_STA` をもう一度評価し、0 なら aux の stream を止める（`PS2_DISABLE_STREAM`）」を頼む口を足す（`drv_ps2_aux_recheck()`）。外付けの PS/2 の mouse は 5330 には無い。
- 事実（2026-10-05 01:17:30、5330 の Linux、ユーザーが操作中に 60 秒の記録、`tests/latitude5330-linux/touch-*`）: I2C-HID の Touchpad の evdev に 20070 event、**PS/2 の mouse には 0 event**。I2C-HID の driver が device を PTP mode にしている間、touchpad の PS/2 の互換の packet は出ない。したがって二重の入力は起きない見込みで、上の aux の停止は firmware の契約に合わせる保険（外付けの PS/2 の mouse の無い機械で aux を止めても失う物が無い）。zedBSD で I2C-HID の attach の前後に PS/2 側の packet が止まるかは p003 の実機で確かめる。

### D8. compositor のタッチパッドの層（p004、`userland/desktop/wayland/touchpad.c`（新）・`touchpad.h`）

- 分類（`input.c` の `zwl_input_probe`）: MT protocol B の device のうち、props に `INPUT_PROP_DIRECT` が無く（`INPUT_PROP_POINTER` が有るか `BTN_TOOL_FINGER` が有る）ものを touchpad とする。他の MT は今どおり touch screen（`touch.c`）。props を compositor に渡すため、`libkeiland-backend` の `struct kl_backend_input_caps` に `properties` の bitmap を足す。zedBSD の backend だけ `EVIOCGPROP` で埋める。Linux・FreeBSD は 0 のままの stub にする（2026-10-05 ユーザー「libkeiland-backendのLinux、FreeBSDの実装は、ベータ1までにはやらなくていいです。」）。
- touchpad.c は report（SYN_REPORT）ごとに指の状態を更新し、pointer の操作を作る。出力は既存の `zwl_seat_motion`・`zwl_seat_button`・`zwl_seat_axis`・`zwl_seat_frame` だけで、Wayland の client と zdesktop の UI はマウスと同じ経路で受ける（BUG-190 の「タップダウン = 押下」もこの経路で揃う）。
- 規則（BUG-166 の仕様 2026-10-04、ユーザー）:
  - **移動**: 1 本の指の移動を相対の motion にする。device の単位（resolution）から mm に直し、ws089-p024 の加速の曲線（無ければ線形の既定）で px にする。
  - **tap**: 1 本の指が 180 ms 以内に 3 mm 未満の動きで離れたら、左の press・release（click）。2 本なら右の click。tap の press は指が離れた時に出す。
  - **tap-drag**: tap の直後の約 300 ms の間に指が触れて動いたら、tap の release を出さずに press のまま drag にし、指が離れたら release。
  - **押し込み（clickpad）**: `BTN_LEFT` は、その時に触れている指の数で左（1 本）・右（2 本）の press にし、押したまま動かせば drag。押し込みの指の移動は、押した瞬間の小さな揺れ（1 mm 未満）を捨てる。
  - **2 本指のスクロール**: 2 本の指が同じ向きに動いたら `zwl_seat_axis` で scroll。自然な向きは Settings の Touchpad の設定（既定 ON、ws089-p024）。
  - **手のひら**: `MT_TOOL_PALM` の指と、pad の端の 5% から入って動かない指は数えない。
- 時刻は evdev の時刻（MSC_TIMESTAMP が有ればそれ）で測る。tap の判定は timer（`zwl_glass_tick` の流れに `zwl_touchpad_tick` を足す）で、指の離れから 300 ms 経ったら tap の release を出す。

### D9. 試験

- host（p003・p004）: (a) 5330 の report descriptor（`synaptics-06cb-ce65-rdesc.bin`）を parser に通し、touch pad の layout（指 5・button・feature の位置）を確かめる。(b) 手で作った report 3 の byte 列から、MT の evdev の列（slot・tracking・TOOL_*・BTN_LEFT）を確かめる。(c) I2C-HID の手順を偽の bus で確かめる（descriptor・reset・feature の byte 列が Linux の採取と同じ）。(d) touchpad.c の状態機械を evdev の台本で確かめる（tap・tap-drag の 300 ms の境目・押し込みの drag・2 本指のスクロール・手のひら）。
- QEMU（T1）: QEMU に I2C-HID は無い。`/dev/input-inject` に `INPUT_INJECT_KIND_TOUCHPAD`（props POINTER・BUTTONPAD、指 5、button）を足し、kernel の PTP の状態機械と compositor の touchpad.c を通して、title bar の drag・slider の tap・2 本指のスクロールを流す（`touchinject` の台本を拡張）。
- 実機（p005）: 5330 の UAT。dmesg で I2C-HID の attach と PTP mode、evdev の記録、BUG-166・167・156・190 の観点、PS/2 の二重の入力が無いこと。

## Phase の分け方（WS159 の表の改訂の案）

| Phase | 内容 | 依存 |
| --- | --- | --- |
| p002 | D2・D3: DesignWare の I2C の driver と ACPI の I2cSerialBus・GpioInt の解析。host の試験（resource の解析）、vmunix の build。実機で I2C1 の上の 0x2c から HID descriptor を読めること（p003 の実機と一緒でよい） | p001 |
| p003 | D4〜D7: hid の層の移動と PTP、`EVIOCGPROP`、I2C-HID の driver と sampling、PS/2 の停止。host の試験 (a)〜(c)、`input-inject` の touchpad | p002 |
| p004 | D8: compositor の touchpad.c と backend の props（zedBSD のみ）。host の試験 (d)、QEMU（T1、inject の touchpad） | p003 の `EVIOCGPROP` と inject（host の試験は先に） |
| p005 | 実機の UAT と全文の規約 | p004 |

## 残り（後の Phase へ確実に移す）

- **D5 の 4 ms の MMIO の sampling は、電池の消費の点でベータ1 だけの仮の形**（2026-10-05 Q1）。Intel の GPIO の割り込み（`INTC1055` の `GPI_IS`・`GPI_IE` と IRQ 14、または pad の IOxAPIC への route）へ移す Phase を WS159 に立てる（p006 の案）。それまでは、指の無い間の sampling の間隔を延ばす（D5）。

## 未決（人間の判断が要る点）

- なし（ベータ1 は割り込みを使わない sampling で進める、は技術の判断として P1 が選んだ。Intel の GPIO の割り込みは後の Phase として Future Work に置く案）。

## 範囲の外

- Linux・FreeBSD の backend の touchpad（2026-10-05 ユーザー、ベータ1 の後）。
- 3・4 本指の gesture（WS142）、Settings の Touchpad の頁（ws089-p024）。この WS では既定値で動かし、設定の口だけを用意する。
- I2C の他の device（touchscreen・sensor）。

## 記録

- host の変更（5330 の Linux）: `i2c-dev` の module を load した（残す）。`i2c_hid_acpi` と `i2c_designware.1` を unbind→bind した（元に戻した）。dynamic debug を一時的に on にした（off に戻した）。`/tmp` に試験の script を置いた。
- 採取の道具: `plan/ws159/tests/i2c-hid-poll-probe.py`、`evdev-absinfo.py`、`touch-record.sh`。
