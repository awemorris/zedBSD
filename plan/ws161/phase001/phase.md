<!-- awesome-plan project=zedbsd record=ws161-p001 -->

# ws161-p001: YubiKey（USB の FIDO2）の要件と設計

Phase ID: `ws161-p001`
Parent: [WS161](../ws.md)
Status: cleared（2026-10-08 Q1 の判定（sweep-beta2-rc §2））（旧: planning → Q1 の判定待ち（2026-10-08 q902 P1 の照合: 第 2 版 §9、U1〜U5 は 2026-10-05 承認、p002〜p005 がこの設計で実装され cleared）（旧: planning（2026-10-05 P1 generation17 q734 で第 1 版。2026-10-05 夕のユーザーの決定で第 2 版（§9）に改めた、P1 generation19 q770。U1〜U5 は 2026-10-05 承認、node は `/dev/smartcardN`）））
Phase disposition: normal
Queue: q734（第 1 版）、q770（第 2 版）

## 範囲

- ユーザー（2026-10-05、原文）:「YubiKeyサポート（USBのFIDO2が最初。NFCのCTAP2 が目標）」
- 入る（この WS の v1）: USB の HID の FIDO2（CTAPHID）で security key と話す。kernel の driver と device の node、userland の library と道具、鍵の情報・
  登録・認証の試験。WS162（FIDO2 の login）が使う口。
- 目標（v2、別の段）: NFC の CTAP2（NFC の reader の driver が要る、§6）。
- 範囲外: login への組み込み（WS162）。YubiKey の OTP（キーボードとして働く interface は今の `usb-hid` がそのまま扱う）・PIV・OpenPGP（CCID）。

**§1〜§7 は第 1 版（`/dev/fidoN`・外部の libfido2）。§9 の第 2 版が置き換える。§1 の「今の形」はそのまま有効。**

## 1. 今の形（2026-10-05 の main を読んだ）

| 項目 | 今 | 場所 |
| --- | --- | --- |
| USB の host | xHCI・EHCI・UHCI。class の driver は `struct drv_usb_driver`（ids・`match`・`attach`…）、interface ごとに `match` の点数が一番高い driver が取る（同点は後に登録した物） | `include/drivers/usb/usb.h` 230・267、`src/drivers/usb/usb.c` 4129・6740 |
| 転送 | URB（`drv_usb_urb_alloc`・`_setup`・`_submit`…）と同期の `drv_usb_interrupt`。endpoint ごとに URB は 1 つ（IN を張ったまま OUT を送れる）。interrupt OUT は controller では扱えるが、使う class の driver が無く試されていない | `usb.c` 1865・2158・2329・2866、`pci-xhci.c` 2040 |
| HID | 汎用の `usb-hid`（class 0x03 を点数 100 で取る）。interrupt IN を 1 つだけ使い、OUT は見ない。report の出力（OUTPUT の item・SET_REPORT）は扱わない。evdev の node だけを作り、input の能力が無い interface（FIDO の usage page 0xF1D0）は `ENODEV` で使われないまま | `src/drivers/usb/usb-hid.c` 144・266・609・1430、`src/drivers/generic/hid-report.c` 1465 |
| userland の node | evdev（`/dev/input/eventN`）だけ。raw の HID・汎用の USB の node は無い。USB の一覧は `/dev/system` の `KERN_SYSTEM_GET_USB_DEVICE`、hotplug は `KERN_SYSTEM_EVENT_USB` | `include/uapi/input.h`・`system.h` 203〜350 |
| 権限 | devfs の既定は 0666 root:wheel（event は 0640）。sessiond が seat の利用者に `/dev/gpu*`・`/dev/input/event*`・`/dev/backlight*` を 0600 で渡す（毎秒あて直す） | `src/kern/devfs.c` 426〜470、`userland/desktop/sessiond/seat.c` |
| crypto | base に SHA-2（libc）と SHA-512 crypt。P-256 の ECDSA・ECDH、HMAC-SHA-256、HKDF、AES-256-CBC は base に無く、OpenSSL 3.5.8 の package（既定の image に入っている）にある。CBOR は無い | `include/libc/sha2.h`、`userland/packages/security/openssl/` |
| FIDO・CCID・NFC | 何も無い | — |
| QEMU | host の QEMU 10.0.11 に `u2f-emulated`・`canokey` が無い（`u2f-passthru` は U2F だけで実物の鍵が要る）。実物の USB の転送は `usb-host`（RTL8822BU の前例） | `plan/ws005/phase020/rtl-guest.sh` 39 |

## 2. kernel: `usb-fido` と `/dev/fidoN`

- **driver**: `src/drivers/usb/usb-fido.c`（新）。HID の interface（class 0x03）の report の記述を読み、usage page 0xF1D0・usage 0x01 の collection があれば
  点数 200 を返す（`usb-hid` の 100 より高いので取れる）。interrupt IN と interrupt OUT の 2 つの endpoint を使う（CTAPHID の 64 byte の report）。
  YubiKey の他の interface（OTP のキーボード、CCID）は今のまま `usb-hid` などが扱う。
- **node**: `/dev/fidoN`（N は 0 から）。1 つの device を同時に 1 つの open だけ（2 つ目は `EBUSY`。CTAPHID の channel の取り合いを避ける）。
  - `write`: ちょうど 64 byte（1 つの report）。interrupt OUT で送る。それ以外の長さは `EINVAL`。
  - `read`: 64 byte の report を 1 つ。IN の URB を張ったままにし、来た report を小さな ring（32 個）に貯める。無ければ待つ（`O_NONBLOCK` は `EAGAIN`）。
  - `poll`: 読める report がある時に読める。
  - `ioctl`: `FIDO_GET_INFO`（vendor・product・release・製品の名前・serial の文字列、report の大きさ 64）だけ。
  - 抜いた時: `read`・`write` は `ENODEV`、node は消える。
- **UAPI**: `include/uapi/fido.h`（新）に `struct fido_info` と `FIDO_GET_INFO`、report の大きさの定数（§7 の H1）。
- **hotplug**: 今の `KERN_SYSTEM_EVENT_USB` と、node の追加の事象（`KERN_SYSTEM_EVENT_INPUT` と同じ形の `KERN_SYSTEM_EVENT_FIDO` か、既存の事象の subject
  `fidoN` で足りるかは p002 で決める）。
- **権限**: 既定は 0600 root。sessiond の `seat.c` が seat の利用者に渡す一覧に `/dev/fido*` を足す（Linux の systemd の uaccess と同じ考え: 前に座っている人が使う）。
  login の画面（WS162）は root の sessiond が直接開く。
- HAL は変えない。USB の core の変更は無い見込み（interrupt OUT の URB は今の API で張れる）。

## 3. userland: library と道具

案は 2 つ（§7 の H2）。

| | (A) libfido2 を package にする（推奨） | (B) 自前の小さな library |
| --- | --- | --- |
| 中身 | Yubico の libfido2（BSD-2-Clause）と libcbor（MIT）を外部の package（tarball を取得・検証して patch）にし、zedBSD の HID の backend（`/dev/fidoN` の open・read・write・poll、libfido2 の `fido_dev_io_t`）を足す。crypto は OpenSSL の package | CTAPHID の枠（INIT・CBOR・PING・CANCEL・KEEPALIVE・WINK・ERROR）、CTAP2 の GetInfo・MakeCredential・GetAssertion・ClientPIN（PIN protocol 2）、CBOR を自前で書く。crypto は OpenSSL か自前の P-256 |
| 道具 | `fido2-token`（一覧・情報・PIN の設定・reset）・`fido2-cred`・`fido2-assert` がそのまま付く | 自前の小さな `fido` の道具 |
| 他との関係 | OpenSSH を libfido2 つきで build し直せば `ssh-keygen -t ed25519-sk` の security key も使える（別の段）。Linux・FreeBSD の Keiland は各 OS の libfido2（hidraw・uhid）をそのまま使える | 3 つの OS の HID の口を自前で書く |
| 工数 | package 2 つ（CMake の前例は zlib・libjpeg-turbo）と backend 1 つ | CBOR と CTAP2 と PIN protocol の暗号の扱いを全部書く。誤りが安全に効く |
| ライセンス | BSD-2・MIT（監査して記録） | Zlib |

- 推奨は (A): 4 LW の見積もりの中で、成熟した実装（YubiKey の癖の扱い、CTAP2.1 の細部）を使える。暗号の細部を自前で書く危険を避ける。
- WS162 の login は root の sessiond（base の daemon）が assertion の検証をする。(A) なら sessiond が libfido2（package）を link するか、検証だけを
  小さな helper（package の側の program）に出すかを WS162 の設計で決める（§7 の H3）。

## 4. 試験

- host: (A) の backend の部分（`/dev/fidoN` の代わりに pipe を使う）と libfido2 の自前の試験。(B) なら CTAPHID の枠と CBOR の host 試験。
- kernel: QEMU には CTAP2 の emulator が無いので、試験の kernel の build に「loopback の FIDO の device」（試験用の小さな CTAPHID の応答器、`usb-hid-checkpoint` の
  前例のような試験だけの driver）を足し、`/dev/fidoN` の read・write・poll・EBUSY・抜いた時を T1 で確かめる（§7 の H4）。
- 実機（UAT）: 実物の YubiKey（5 系、USB-A か USB-C）を 5330 に挿し、`fido2-token -L`・`-I`、PIN の設定、`fido2-cred -M`（登録）・`fido2-assert -G`（認証）、
  抜き差し。QEMU の `usb-host` で実物を渡す試験も可能（host に鍵が要る）。

## 5. 段（案）

| Phase | 内容 | 依存 |
| --- | --- | --- |
| p002 | kernel: `usb-fido`・`/dev/fidoN`・UAPI・試験の loopback の device・sessiond の seat の一覧。T1 | H1・H4 |
| p003 | userland: (A) libcbor・libfido2 の package と zedBSD の backend、道具。host 試験 | p002、H2 |
| p004 | 実機の UAT（YubiKey）、Linux・FreeBSD の Keiland での libfido2 の確かめ | p003 |
| p005 | 全文規約の見直し | p004 |
| （v2） | NFC の CTAP2（§6） | p004 |

## 6. NFC（目標、v2）

- NFC の CTAP2 は ISO 7816-4 の APDU を NFC（ISO 14443-4）で運ぶ。必要な物: NFC の reader の driver と、APDU を送る口。
- 道筋の候補: (1) USB の CCID の class の reader（例 ACR122U、ACR1252U）: CCID の class の driver と PC/SC に当たる口（`/dev/smartcardN` か pcsc-lite の package）、
  libfido2 の PC/SC の backend（libfido2 は pcsc に対応している）。(2) 機種に内蔵の NFC の controller（5330 にあれば、I2C の NXP の controller）: 専用の driver。
- reader の機種と 5330 の内蔵の有無を確かめてから、v2 の WS（か段）を立てる（§7 の H5）。

## 7. 人間の判断が要る点

| ID | 問い | 案 |
| --- | --- | --- |
| H1 | **UAPI**: `include/uapi/fido.h`（`/dev/fidoN` の read・write 64 byte、`FIDO_GET_INFO`）を足してよいか | 足す。FIDO の interface だけの raw の node（キーボードなどの raw の HID は出さない） |
| H2 | **外部 package**: libfido2（BSD-2）と libcbor（MIT）を package にし、OpenSSL の package に依る形でよいか（推奨 (A)）。自前で書くか（(B)） | (A) |
| H3 | WS162 で、root の sessiond が package の libfido2 を使ってよいか（使わないなら検証の helper を分ける） | WS162 の設計で決める（この WS では口だけ） |
| H4 | 試験だけの loopback の FIDO の device を試験の kernel に足す（QEMU に CTAP2 の emulator が無いため） | 足す（`CONFIG_` の試験の build だけ） |
| H5 | NFC の reader の機種（CCID の USB の reader を買うか、5330 の内蔵を使うか） | 5330 の内蔵の有無を UAT で確かめてから |
| H6 | `/dev/fidoN` を seat の利用者に渡す（前に座っている人が鍵を使える） | 渡す |

## 9. 第 2 版（2026-10-05 夕のユーザーの決定、P1 generation19 q770）

### 9.0 決まったこと（plan/master.md 76・80〜87 行の記録から）

| 項目 | 決定（ユーザーの原文は master） | 第 1 版から変わる所 |
| --- | --- | --- |
| USB の FIDO | 標準の `usb-hid` に生の report の口を足す。入力の装置にしない HID の interface、まず FIDO の用途 page 0xF1D0。interrupt OUT と出力の report。node は **`/dev/input/hidrawX`** | `usb-fido` と `/dev/fidoN`・`include/uapi/fido.h` は無くす（H1 は消える）。UAPI は hidraw の物に替え、形はユーザーの承認が要る |
| OTP | YubiKey の OTP（keyboard の interface）は当面非対応 | — |
| CCID | PIV・OpenPGP（YubiKey の CCID の interface）と NFC の reader は**共通の CCID の driver** | 第 1 版の範囲外だった CCID が入る |
| NFC | 対象は **ACR1252U**（USB の CCID の NFC の reader、Windows の標準の CCID の driver で動く）。kernel は汎用の NFC の driver = USB の CCID の reader から ISO-DEP の APDU の交換を出す | §6 の (1) に決まった。5330 の内蔵の NFC（(2)）はやめる |
| library | 外部の libfido2・libcbor は使わない。独自の **libpasskey**: transport（hidraw の CTAPHID、NFC の APDU）と共通の FIDO2（CTAP2・CBOR の部分集合・PIN/UV の protocol） | H2 は (B) に決まった |
| 暗号 | まず OpenSSL の libcrypto（package、3.5.8）を呼ぶ。自前の暗号は後の候補 | — |
| 置き場所 | libpasskey は Keiland の側（package の境界）。base の全自前の方針（master-design-policy §2.1）とは衝突しない | — |
| 検証の場所 | **未定**（2026-10-05 Q1: ユーザーは BSD Auth の形 = `login_<style>` の helper、sandbox の CTAP の helper と libcrypto の小さな検証器を検討中）。WS161 の device の側はどこで検証するかに依らない形にする | libpasskey の検証は「bytes と公開鍵から合否」の純粋な関数に分け、どの process からでも呼べるようにする（§9.4） |

### 9.1 全体の形

```
kernel                                   userland（Keiland の側）
---------------------------------------  ------------------------------------------------------------
usb-hid ─ hidraw ─ /dev/input/hidrawN ── libpasskey: transport-hid（CTAPHID の枠）──┐
                                                                                 ├─ ctap2（CTAP2 の command）── 道具 passkey
usb-ccid ─ /dev/smartcardN（APDU の交換）──── libpasskey: transport-nfc（ISO 7816 の APDU）┘   cbor・pin（PIN/UV protocol）・verify
  ├ ACR1252U の非接触の slot（ISO 14443-4 の card = YubiKey の NFC）                      └ OpenSSL の libcrypto
  └ YubiKey の USB の CCID の interface（PIV・OpenPGP、この WS では node まで）
```

### 9.2 kernel: `usb-hid` の hidraw

- **対象**: `usb-hid` が取る HID の interface のうち、report の記述の top の collection が **用途 page 0xF1D0（FIDO Alliance）・usage 0x01（CTAPHID）**
  の物。今は「input の能力が無い」として `ENODEV` で捨てている interface（usb-hid.c 1440）を、捨てずに hidraw の node にする。他の用途 page の
  vendor の interface は当面出さない（一覧の表を 1 つ持ち、後で足せる形）。keyboard・mouse などの input の interface は今のまま evdev だけ（hidraw は出さない）。
- **転送**: interrupt IN は今の URB の張り方のまま（来た report を evdev の代わりに hidraw の queue へ）。interrupt OUT の endpoint があれば出力の report を
  interrupt OUT で送り、無ければ control の SET_REPORT（Output）で送る。CTAPHID の report は 64 byte、report ID は無い。
- **node**: `/dev/input/hidrawN`（N は 0 から、抜いたら消える）。同時に何人でも open でき、**open ごとに入力の report の queue（64 個）**を持つ（Linux の hidraw と
  同じ。CTAPHID は channel ID で混ざらない）。queue が溢れたら古い物を捨てる。
  - `read`: 入力の report を 1 つ（report ID がある device は先頭の 1 byte が ID）。無ければ待つ（`O_NONBLOCK` は `EAGAIN`）。buffer が短ければ切って返す。
  - `write`: 出力の report を 1 つ。先頭の 1 byte は report ID（ID の無い device は 0）、残りが report の中身（Linux と同じ）。長さが記述の出力の report の
    大きさ + 1 を超えたら `EINVAL`。interrupt OUT（か SET_REPORT）が終わるまで待つ。
  - `poll`: 読める report がある時に POLLIN。書くのは常に可（POLLOUT）。抜いた後は POLLHUP。
  - `ioctl`（§9.9 の U1 で形を承認してもらう）: `HIDRAW_GET_INFO`（bus = USB、vendor・product・release、interface の番号、top の collection の用途 page と usage、
    入力と出力の report の大きさ）、`HIDRAW_GET_DESCRIPTOR`（report の記述の byte、最大 4096）、`HIDRAW_GET_NAME`（製品の文字列）、`HIDRAW_GET_PHYS`（USB の
    位置の文字列、evdev の physical_path と同じ形）。feature の report（GET・SET_FEATURE）は CTAPHID に要らないので v1 には入れない。
  - 抜いた時: 待っている `read` は `ENODEV`、以後の `read`・`write` も `ENODEV`、node は消える。
- **UAPI**: `include/uapi/hidraw.h`（新）に `struct hidraw_info`、ioctl の番号、`HIDRAW_DESCRIPTOR_MAX` 4096。**ユーザーの承認が要る（U1）**。
- **hotplug**: devfs の node の追加・削除の今の事象（`/dev/input/eventN` と同じ道）で足りるか、`KERN_SYSTEM_EVENT_INPUT` の subject に hidraw を足すかは
  p002 で今の code を読んで決める（UAPI の追加が要るなら U1 に含めて承認を得る）。
- **権限**: devfs の既定（0600 root）。sessiond の seat の一覧（`seat.c`、今は `/dev/input/event*` などを seat の利用者に 0600 で渡す）に `/dev/input/hidraw*` を
  足す（H6 の決定どおり、前に座っている人が鍵を使える）。sessiond の変更は一覧の 1 行で、検証の制御は入れない。

### 9.3 kernel: 共通の CCID の driver `usb-ccid`

- **driver**: `src/drivers/usb/usb-ccid.c`（新）。USB の interface class 0x0B（Smart Card、CCID rev 1.1）を取る。CCID の class の記述子（`dwFeatures`・
  `dwMaxCCIDMessageLength`・`bMaxSlotIndex`・`dwProtocols`）を読み、**APDU の水準の交換**（`dwFeatures` の短い APDU 0x00020000 か拡張の APDU 0x00040000）の
  reader だけを扱う（TPDU・文字の水準の reader は `ENODEV`、記録だけ。ACR1252U と YubiKey の CCID は APDU の水準のはず = p002 で記述子を実物か資料で確かめる）。
- **転送**: bulk OUT に `PC_to_RDR_*`、bulk IN で `RDR_to_PC_*` を受ける（`bSeq` を合わせる。時間の延長の `bStatus` の time extension は待ち続ける）。
  interrupt IN の `RDR_to_PC_NotifySlotChange` で card の出し入れを知る（無い reader は `PC_to_RDR_GetSlotStatus` を一定の間隔で問う）。使う command:
  `IccPowerOn`（ATR を受ける）・`IccPowerOff`・`GetSlotStatus`・`XfrBlock`（APDU）・`Abort`（control の ABORT と組で）。`Escape`（reader の独自の command）は v1 に入れない
  （ACR1252U の LED・ブザーの制御が要るなら後で）。
- **ISO-DEP**: ACR1252U は非接触の card（ISO 14443-4 = ISO-DEP、YubiKey の NFC）を PC/SC の第 3 部の形で「ATR を合成した ICC」として見せ、APDU を ISO-DEP の
  frame に包むのは reader がする。kernel は APDU を `XfrBlock` で運ぶだけで、ISO 14443 の低い層（anticollision・RATS・I-block）は扱わない。
  拡張の APDU を持たない reader には、kernel は分けない（分割は ISO 7816-4 の command chaining として userland の libpasskey がする）。
- **node**: `/dev/smartcardN`（**reader の slot ごとに 1 つ**。ACR1252U は非接触の slot と SAM の slot の 2 つを見せるはずで、2 つの node になる。名前は U2 で確かめる）。
  **一度に 1 つの open だけ**（2 つ目は `EBUSY`。PC/SC の排他の接続に当たり、APDU の列が他の process と混ざらない）。
  - `ioctl`（U2 で形を承認してもらう）: `CCID_GET_INFO`（vendor・product・slot の番号と数・製品の文字列・`dwFeatures`・最長の message・拡張の APDU の可否）、
    `CCID_GET_STATUS`（card が有るか・電源が入っているか・ATR（最大 33 byte））、`CCID_POWER_ON`（電源を入れて ATR を返す）、`CCID_POWER_OFF`、
    `CCID_TRANSMIT`（command の APDU の pointer と長さ、応答の buffer の pointer と大きさ、待つ時間の上限の ms → 応答の長さ（SW1 SW2 を含む））。
    APDU の最長は短い APDU の reader で 261 byte、拡張で 65544 byte（`dwMaxCCIDMessageLength` で更に絞る）。
  - `read`・`poll`: card の出し入れの事象（`struct ccid_event`: 入った・抜けた・番号の通し）。`poll` は事象がある時に POLLIN。
  - 抜いた時（reader ごと）: `ioctl`・`read` は `ENODEV`、node は消える。card だけ抜けた時は事象を出し、次の `CCID_TRANSMIT` は `ENXIO`（電源から入れ直す）。
- **UAPI**: `include/uapi/ccid.h`（新）。**ユーザーの承認が要る（U2）**。
- **権限**: `/dev/smartcard*` も seat の一覧に足す（U3）。

#### 9.3.1 U2 の形の詰め（2026-10-05、見直しの M7〜M10・m9・m10 から。Q1: 承認の範囲の中、ユーザーには要約で示す）

- 外の仕様は `docs/reference/security-keys.md`、header は `include/uapi/ccid.h`。
- slot の node は**共有の open**（状態と card の出し入れの事象は誰でも読める）。`CCID_POWER_ON` がその open に slot の claim を取り、`CCID_POWER_OFF` か
  最後の close まで、他の open の POWER_ON・TRANSMIT は `EBUSY`。claim した file の最後の close で card の電源を切る（PIN の確かめ・選んだ applet が
  次の program に渡らない）。
- `struct ccid_transmit` は 64 bit の pointer と大きさ（32 bit と 64 bit の program で同じ形）。待ちの既定 30 秒、最大 120 秒。card が時間の延長を求めても
  期限で止め、reader に ABORT（control の ABORT と `PC_to_RDR_Abort`）を送る（`EINTR`・`ETIMEDOUT`）。答えが buffer より長ければ `EMSGSIZE`、card が
  抜けたら `ENXIO`。
- reader ごとに command は 1 つずつ（`bSeq` を合わせ、古い答えは捨てる）。answer の chaining（`bChainParameter`）は kernel が集める。command は
  `dwMaxCCIDMessageLength - 10` まで（越える物は userland が ISO 7816-4 の chaining で分ける）。
- 事象は `read()` の `struct ccid_event`（open ごとに 16 個）と `/dev/system` の `KERN_SYSTEM_EVENT_USB`（subject `smartcardN`）。ioctl の group は 'S'。

### 9.4 userland: libpasskey（Keiland の側）

- 置き場所: `userland/desktop/libpasskey/`（Keiland の library と同じ並び。Linux・FreeBSD の Keiland でも build する）。**2026-10-05 改: WS172 の設計（判断 P1）で base の `userland/base/libpasskey/`。暗号は `crypto.h` の口の後ろ（`crypto-openssl.c` だけが OpenSSL を呼ぶ）。**依存は OpenSSL の libcrypto だけ
  （zedBSD は package の `openssl`、Linux・FreeBSD は OS の物）。ライセンスは Zlib。
- **層**:

| file（案） | 中身 |
| --- | --- |
| `transport-hid.c` | CTAPHID: INIT（8 byte の nonce、channel の割り当て）、CBOR（0x10）・MSG・PING・CANCEL・WINK・KEEPALIVE（0x3B、待つ）・ERROR、64 byte の report への分割（初めの packet と続きの packet の通し番号）、時間の上限 |
| `transport-nfc.c` | NFC の CTAP: SELECT（AID `A0000006472F0001`、答えの "FIDO_2_0"・"U2F_V2" で版を知る）、NFCCTAP_MSG（CLA 0x80・INS 0x10）、短い APDU の reader では ISO 7816-4 の command chaining（CLA の 0x10）、応答の 61xx の GET RESPONSE、keepalive の 0x9100 の時の NFCCTAP_GETRESPONSE（INS 0x11） |
| `os-zedbsd.c`・`os-linux.c`・`os-freebsd.c` | device の列挙と開閉: zedBSD は `/dev/input/hidraw*`（`HIDRAW_GET_INFO` の用途 page 0xF1D0）と `/dev/smartcard*`。Linux は `/dev/hidraw*`（report の記述を読んで 0xF1D0）、NFC は v1 では無し（pcsc-lite を使うかは後）。FreeBSD は `/dev/hidraw*`（14 以降）か `uhid`、NFC は v1 では無し |
| `cbor.c` | CTAP2 の CBOR の部分集合: 符号なし・負の整数、byte 列、text 列、配列、map、true・false・null。CTAP2 の正規の符号化（最短の長さ、map の key の並び）で書き、読む時は深さ・長さ・残りの byte を必ず確かめる（不定長・浮動小数・tag は拒む） |
| `ctap2.c` | authenticatorGetInfo（0x04）・MakeCredential（0x01）・GetAssertion（0x02）・GetNextAssertion（0x08）・ClientPIN（0x06: getPinRetries・getKeyAgreement・setPIN・changePIN・getPinUvAuthTokenUsingPinWithPermissions）・Selection（0x0B）・Reset（0x07、道具だけ） |
| `pin.c` | PIN/UV の protocol 1 と 2: P-256 の ECDH、protocol 2 は HKDF-SHA-256 で HMAC と AES の鍵を分ける、AES-256-CBC、HMAC-SHA-256。PIN は UTF-8 で 4〜63 byte、使った後に消す。OpenSSL の EVP |
| `verify.c` | **検証だけの純粋な関数**: authData の分解（rpIdHash・flags の UP・UV・AT・ED・signCount・attestedCredentialData の COSE の鍵）、rpId の hash の一致、flags の要求、signCount、ES256（COSE alg -7、P-256）の署名（authData ‖ clientDataHash）の検証。入力は byte 列と保存してある公開鍵（COSE の bytes）だけで、device にも file にも触れない。どの process（compositor の mock、BSD Auth の検証器、Linux の helper）からも呼べる |
| `passkey.h` | 公開の口: device の列挙・open・close、GetInfo、credential の作成、assertion の取得、PIN の設定・変更・残りの回数、`passkey_verify_assertion`（上の純粋な関数）、wink・cancel |

- **道具**: `fidoctl`（2026-10-05 改名: `/sbin/passkey` は WS172 の認証の program。WS172 の見直しの m12）: `list`・`info`・`set-pin`・`change-pin`・`register`（rpId・user を与えて credential を作り、credential ID と公開鍵を
  出す）・`assert`（challenge を与えて assertion を取る）・`verify`（`assert` の出力と公開鍵で検証）。WS162 の mock の下ごしらえと UAT の道具。
- 検証の場所に依らない: WS162 の mock（compositor の greeter が直接）でも、BSD Auth の形（sandbox の CTAP の helper が `passkey_get_assertion` だけ、
  小さな検証器が `passkey_verify_assertion` だけ）でも、同じ library を分けて link できる（検証器は `verify.c`・`cbor.c` と libcrypto だけで足りるように file を分ける）。

### 9.5 試験

- **host（実装の担当が流す）**:
  - CBOR: RFC 8949 の付録 A の例（整数・byte 列・text・配列・map）の符号化と読み、正規の形でない入力の拒否（不定長・長すぎる長さ・深すぎる入れ子・途中で切れた入力）。
  - CTAPHID と NFC の枠: 試験だけの**ソフトウェアの authenticator**（host の試験の中だけ。OpenSSL で P-256 の鍵を作り、GetInfo・MakeCredential・
    GetAssertion・ClientPIN を答える）を fake の transport（pipe）の向こうに置き、分割・続きの packet・keepalive・cancel・command chaining・61xx を通す。
  - PIN/UV protocol 1・2: ソフトウェアの authenticator と両側で鍵を合わせ、setPIN・changePIN・token の取得が往復する。
  - verify: ソフトウェアの authenticator の assertion が通る、1 bit 変えた authData・署名・clientDataHash・rpIdHash・UP の無い flags が落ちる。
    ES256 の署名の扱い（DER の分解・低い S など）を Wycheproof の ECDSA P-256 SHA-256 の vector の一部で確かめる（暗号は OpenSSL でも、包みの誤りを見る）。
- **QEMU（T1）**: kernel の hidraw と CCID は QEMU に CTAP2 の device が無い。(a) 試験の kernel の build だけの loopback の device（第 1 版の H4 と同じ形: 試験だけの
  小さな CTAPHID の応答器を hidraw に繋ぐ）で `/dev/input/hidrawN` の read・write・poll・複数の open・抜いた時を確かめる。(b) CCID は QEMU の `usb-ccid` の
  device（emulated の card）で `CCID_POWER_ON`・ATR・`CCID_TRANSMIT`（SELECT の往復）を確かめられるかを T1 に一度だけ調べてもらう（host の QEMU の build に
  入っているか、APDU の水準か）。無ければ (a) と同じく試験だけの loopback。
- **実機（UAT、5330）**: YubiKey 5（USB）で `passkey list`・`info`・`set-pin`・`register`・`assert`・`verify`、抜き差し。ACR1252U に YubiKey 5 NFC をかざして
  同じ一連。QEMU の `usb-host` で実物を渡す試験もできる（鍵と reader が host に要る）。

### 9.6 段（第 2 版）

| Phase | 内容 | 依存 |
| --- | --- | --- |
| p002 | kernel: `usb-hid` の hidraw（FIDO の interface）、`include/uapi/hidraw.h`、seat の一覧、試験だけの loopback の device。T1 | U1・U3 |
| p003 | kernel: `usb-ccid`（APDU の水準、slot の node、card の出し入れ）、`include/uapi/ccid.h`、seat の一覧。T1（QEMU の usb-ccid か loopback） | U2・U3 |
| p004 | libpasskey: cbor・transport-hid・ctap2・pin・verify と zedBSD・Linux の os 層、道具 `passkey`。host 試験（ソフトウェアの authenticator、RFC 8949、Wycheproof の一部） | p002 |
| p005 | libpasskey: transport-nfc（SELECT・NFCCTAP_MSG・chaining・GET RESPONSE・keepalive）と zedBSD の `/dev/smartcard*`。host 試験 | p003・p004 |
| p006 | 実機の UAT（YubiKey 5 の USB、ACR1252U と YubiKey 5 NFC）、Linux・FreeBSD の build | p005 |
| p007 | 全文規約の見直し | p006 |

見積もり: 第 1 版の 4 LW から増える（CCID の driver と自前の CTAP2・CBOR・PIN protocol）。概算 6〜7 LW（kernel 2、libpasskey 3〜4、UAT と規約 1）。

### 9.7 WS162 との境

- WS162（FIDO2 の login の mock）は `passkey.h` の口を使う。ユーザーの指示（2026-10-05）は「greeter が直接叩く mock、設定は ~/.config」。
  検証の場所は BSD Auth の形を検討中（Q1）なので、WS162 の設計は libpasskey の `passkey_get_assertion`（device と話す側）と `passkey_verify_assertion`
  （検証だけ）を別の process に置ける前提で書く。
- WS163 の G1 と同じ問題（greeter は `_greeter` で、利用者の ~/.config を読めない）は FIDO2 にもある。登録した公開鍵の置き場所は WS162 の設計で扱う。

### 9.8 範囲外（この WS では作らない）

- YubiKey の OTP、PIV・OpenPGP の application（CCID の node までは作るが、PIV の道具・PKCS#11 は作らない）、U2F（CTAP1）だけの古い鍵（GetInfo が無い鍵は
  `passkey list` に「CTAP1 only」と出すだけ）、Bluetooth の FIDO、platform の authenticator（TPM）、pcsc-lite の互換の口。

### 9.9 人間の判断が要る点（第 2 版）

| ID | 問い | 案 |
| --- | --- | --- |
| U1（**承認 2026-10-05**） | **UAPI `include/uapi/hidraw.h`** の形: node `/dev/input/hidrawN`、open ごとの queue、`read` = 入力の report 1 つ（ID がある device は先頭の 1 byte が ID）、`write` = 先頭の 1 byte が report ID（無ければ 0）、ioctl `HIDRAW_GET_INFO`・`HIDRAW_GET_DESCRIPTOR`・`HIDRAW_GET_NAME`・`HIDRAW_GET_PHYS`。出す interface は用途 page 0xF1D0 だけから始める | この形で足す（Linux の hidraw の read・write の意味に合わせ、libpasskey の zedBSD と Linux の os 層をほぼ同じにする。ioctl は zedBSD の形） |
| U2（**承認 2026-10-05**） | **UAPI `include/uapi/ccid.h`** と node の名前: `/dev/smartcardN`（slot ごと、排他の open）、ioctl `CCID_GET_INFO`・`CCID_GET_STATUS`・`CCID_POWER_ON`・`CCID_POWER_OFF`・`CCID_TRANSMIT`、`read` で card の出し入れ。名前は `/dev/ccidN` か `/dev/smartcardN` か | **承認（2026-10-05 ユーザー）: `/dev/smartcardN`**、この形 |
| U3（**承認 2026-10-05**、smartcard の node で） | sessiond の seat の一覧に `/dev/input/hidraw*` と `/dev/smartcard*` を足す（前に座っている人が鍵と reader を使える。検証の制御は sessiond に入れない） | 足す |
| U4（**承認 2026-10-05**） | QEMU の試験: 試験の kernel の build だけの loopback の device（CTAPHID の応答器、要るなら CCID の応答器も）を足してよいか | 足す（第 1 版の H4 と同じ） |
| U5（**承認 2026-10-05**: ベータ2 に 6〜7 LW） | 見積もりの増加（4 LW → 6〜7 LW）とベータ2 の範囲 | ユーザーの判断 |

U1〜U3 の承認まで、kernel と UAPI の code は書かない。p004（libpasskey の cbor・ctap2・pin・verify と host 試験）は U1 の形に依る所が os 層だけなので、
os 層を除いて先に進められる（Q1 が Queue に入れれば）。

### 9.10 敵対的な設計の見直し（2026-10-05、design-reviewer）と扱い

見直しの指摘（B = 止める、M = 大きい、m = 小さい）と、この版での扱い。p002 の実装（7a1d339c）に入れた物と、後の段・判断に回す物を分ける。

| 指摘 | 中身（要点） | 扱い |
| --- | --- | --- |
| B1 | devfs の既定は 0666 で、`hidraw*` は `/dev/input` に入らない | **p002 で直した**: `devfs.c` が `hidraw*` を `/dev/input` に置き、`hidraw*`・`smartcard*` を 0600 root。rdev の major は hidraw 0x000f、smartcard は p003 で別に取る |
| M1 | 今の HID の parser は Output を記録せず、top の usage を出さない | **p002 で別の道**: 純粋な `hidraw-describe.c`（usage・番号付き・入力と出力の大きさ）を足し、parser は変えない。host 試験（YubiKey の FIDO の記述・keyboard・番号付き・拡張 usage・切れた記述） |
| M2 | 送りの門・writer の順・lifetime・待ちの上限 | **p002 で直した**: OUT は IN の URB の門を使わず `drv_usb_interrupt`（OUT の endpoint、無ければ SET_REPORT）、class の output の mutex で 1 つずつ（`mutex_lock_interruptible`）、上限 5 秒。node の record は cdev の finalizer で消える（usb-hid の detach は unregister の後に free） |
| M3 | 複数の open と revoke 無し: 別の process が CTAPHID を占め、触れた指で他人の要求に署名させられる（touch hijack） | **判断 V1 へ**（下）。UAPI の追加（排他の grab）と、WS172 で認証が root の passkey になることとの関係 |
| M4 | 検証の関数は challenge・rpId・許す credential・signCount を検証側が持ち、credential ID で鍵を引くこと。base に置くなら OpenSSL に依らない形 | WS172（passkey を base に、OpenSSL は Guardrail の例外、release 前に自前の暗号へ）の設計に渡す。libpasskey の `verify.c` は小さな crypto の口（SHA-256・P-256 の ECDSA の検証）の後ろに置く |
| M5 | NFC の SELECT の答え "U2F_V2" でも CTAP2 は在り得る（GetInfo で決める）。NFCCTAP_MSG は P1 = 0x80 | p005 の設計に入れる |
| M6 | CTAP 2.0 の鍵は getPinToken（0x05）と protocol 1 | p004 に入れる（GetInfo の `options`・`pinUvAuthProtocols` で選ぶ） |
| M7・M8・M9 | CCID: 一度に 1 つの command と bSeq、時間の延長にも上限、abort の手順、出し入れの見張りと排他の分離、close で電源を切る、NFC は短い APDU と chaining | p003・p005 の設計に入れる（slot の node の open は共有、`CCID_POWER_ON` から close までが排他、close で power off） |
| M10 | QEMU の `usb-ccid` は TPDU の水準のはずで役に立たない。loopback は xHCI の interrupt OUT を通らない | p003 の T1 は loopback か QEMU の `canokey`（host に libcanokey-qemu が要る）を調べる。interrupt OUT の実物の確かめは UAT（YubiKey）か `u2f-passthru`。pure な部分（describe・queue・CCID の枠）は host 試験 |
| M11 | node の名前が古い | 直した（`/dev/smartcardN`、U1〜U5 承認済み） |
| M12 | hmac-secret が無い（WS162 K1 (b)） | WS162・WS163 の mock は WS172 に置き換わる（2026-10-05 ユーザー）。要るなら WS172 の設計で |
| m1〜m5 | CTAPHID の CID・nonce の照合、channel busy、`maxMsgSize`、CBOR の正規の key の順（長さが先）・重複の key、PIN の長さは code point・64 byte の詰め、protocol 1・2 の IV と HMAC の長さ、低い S を強いない、鍵の型は -7 だけ | p004 の実装の注意として記録 |
| m2 | 64 個の queue と 7609 byte の message（129 packet） | 記録（v1 は queue 64。溢れは古い物から捨てる。libpasskey は読む側を急ぐ。足りなければ queue を大きくする） |
| m6 | `seat_input` は `event` だけを見る | **p002 で直した**（prefix を引数に、hidraw の戻しは 0600 root） |
| m7 | devfs の事象は無い | **p002 で直した**（hidraw が自分で `KERN_SYSTEM_EVENT_INPUT` を出す、subject `hidrawN`。compositor は input の事象で一覧を読み直すだけ） |
| m8 | `os-freebsd.c` の段が無い | p006 に入れる |
| m9 | `CCID_TRANSMIT` は固定幅の pointer、ioctl の大きさは 13 bit、group の文字 | p003（hidraw は 'H'、smartcard は 'S'） |
| m10 | 拡張の CCID の message の最長と 64 KiB の予約 | p003（APDU を `dwMaxCCIDMessageLength - 10` に絞る） |
| m11 | Wycheproof は Apache-2.0 | p004 で写すなら license を記録 |
| m12 | OTP の keyboard の interface は evdev のまま、触れると OTP の文字列が打たれる | 記録（判断 V2） |

### 9.11 人間の判断が要る点（見直しの後）

| ID | 問い | 案 |
| --- | --- | --- |
| V1 | **touch hijack**: hidraw は複数の open で、revoke が無い。seat の利用者の別の process が鍵に要求を出し続けると、login の時の指で他の要求に署名させられ得る。(a) UAPI に排他の `HIDRAW_GRAB`（grab した open だけが report を受け、他の write は EBUSY）を足し、認証の道（WS172 の passkey）は grab して使う。(b) seat の利用者に hidraw を渡さない（U3 を取り消す。認証は root の passkey だけが鍵を開く。browser の WebAuthn は後の題）。(c) 今のまま | (a)。UAPI の追加なので承認が要る |
| V2 | YubiKey の OTP の keyboard の interface: 今は evdev の keyboard として使われ、鍵に触れると OTP の文字列が focus の欄に打たれる | 当面そのまま（OTP の非対応は「OTP の機能を使わない」こと）。困るなら usb-hid がその interface（YubiKey の vendor・usage が keyboard の interface）を捨てる |

## 結果

（設計の第 1 版。判断 H1〜H6 待ち）

2026-10-05 generation19: ユーザーの決定（hidraw・共通の CCID・ACR1252U・libpasskey・OpenSSL）で第 2 版（§9）に改めた。U1〜U5 待ち。
