<!-- awesome-plan project=zedbsd record=ws143p005-review1 -->

# ws143-p005 設計の review 1（2026-10-08、dcf6843ab）

対象は `plan/ws143/phase005/phase.md`（commit dcf6843ab）。design-reviewer の敵対的レビューで、読むだけにした（file の変更、build、QEMU、rm は行っておらず、`.internal/` も読んでいない）。

- **HAL の変更は要らない。** `include/kern/input-device.h` に `drv_input_device_number` を足すのは kern の header で、HAL ではない。
- **仕様の値の照合について。** 下の照合はレビュー者の記憶によるもので、仕様の PDF とは照合していない。

---

## Blocking（実装の前に設計を直す）

**B1. システム全体の input device は 8 つまでで、設計はこの上限を扱っていない**
- 根拠:
  - `src/drivers/generic/input.c:40` が `INPUT_DEVICE_MAX 8U`。325〜335 行で、満ちると `ENOSPC` を返す。
  - 番号の slot が空くのは cdev の最後の参照が消えた時（2634〜2653 行の `input_device_release`）。compositor が古い node を閉じるまでは空かない。
  - phase.md は INPUT_DEVICE_MAX に一度も触れていない。§5 の errno の対応も、ENOSPC を `descriptor` 扱いの「他の errno」に落とす（252 行）。
- 起こる状況:
  - 5330 では次の合計で 8 を超える: PS/2 で 2（`ps2-8042.c:1211,1236`）、i2c-hid の touchpad、USB の receiver（interface ごとに device）、BT のキーボードとマウス。touch を持つ device はさらに 2 つ使う。
  - `HID_HOST_OPENS_MAX 8` なので、乗っ取られた `_bluetooth` の子は slot を全部使える。その後に挿した USB キーボードが登録できない（local DoS）。
  - QEMU でも PS/2×2、usb-kbd、usb-mouse、BT の 3 つで 7 になり、§7.5 (5) の再接続が不安定になる。
- 修正案:
  - INPUT_DEVICE_MAX を例えば 32 に上げる。compositor の `KWL_INPUT_MAX 16`（`userland/desktop/wayland/kwl.h:73`）との関係も確かめる。
  - hid-host が全部の slot を取れないよう、予約か上限を設ける。
  - ENOSPC を `ERROR input-full` に対応させ、試験に入れる。

**B2. PAIRED の後に切って繋ぎ直す形（Q4・§4.12）と `hid=1` の条件のため、普通の HID の相手が最初の接続に失敗する**
- 根拠:
  - phase.md の該当箇所:
    - §4.1 189 行: router は bond 済みの HID（`hid=1`）だけを受ける。
    - §4.7 246 行: `hid=1` は最初の SDP の成功で付く。
    - §4.9 263 行: page scan は `hid=1` の bond がある時だけ。
  - p004 の Q5 は「接続を保つのは HID（p005）の仕事」（phase004/phase.md:233）としており、p005 はそれを逆にしている。
- 起こる状況:
  - HIDNormallyConnectable=false で HIDReconnectInitiate=true の device（§6 の 02 そのもの）は、pairing の後に切られると自分から繋ぎ直そうとする。
  - この時点ではまだ `hid=1` が無いので、page scan は off で、router も Reject する。
  - host からの page には（NormallyConnectable=false の意味から）答えない見込み（推測）。結果として二度と繋がらない。
  - pairing の ACL の上で HID の channel を自分から開く device も、L2CAP で断られる。
  - loopback の 02 は自分の SDP の値（NormallyConnectable=false）と食い違って host の page に答えるので、QEMU ではこの誤りが見えない。
- 修正案:
  - HID らしい device の PAIRED では、ACL の持ち主を router で hid に移し、同じ link の上で SDP と channel を開く。相手から開かれた HID の PSM もその link で受ける。
  - 最低限でも、PAIRED の時点で HID 候補の bond に印を付け、page scan を on にし、相手からの接続を受ける。
  - Q4 は、この得失を直してユーザーに問い直す。

**B3. 相手から来る ATT の request に答えない**
- 根拠:
  - §4.5 の 220〜223 行は client の役だけを定めている。
  - ATT では、request には必ず response が要る（Core Vol 3 Part F の transaction の規則。30 秒の timeout で link を切る）。
- 起こる状況:
  - HOGP の device が自分から Exchange MTU Request を送ったり、central の GATT の DB を発見したりすると、答えが無いので 30 秒で相手が切る。
  - loopback の 04 は相手からの request を出さないので、QEMU では見つからない。
- 修正案:
  - 最小の ATT server を足す。Exchange MTU Request には Response で答える。発見の request には Attribute Not Found（0x0A）、他の request には Request Not Supported（0x06）の Error Response を返す。command は捨てる。
  - loopback の 04 に「接続の直後に自分から Exchange MTU Request を送る」振る舞いを足す。

**B4. bond の file に descriptor を入れると、bond 全体が読めなくなる**
- 根拠:
  - `userland/base/bluetoothd/keys.c:31` が `KEYS_TEXT_MAX 2048U`。177〜180 行で、2048 byte 以上の file は EBADMSG になる。
  - `btd_keys_list` は読めない bond を飛ばす（285〜288 行）。
  - §4.11（278 行）は `hid_descriptor_<n>=` を最大 32 行足す。合計は約 8.8 KB になる。
- 起こる状況:
  - descriptor を cache した device の link key が読めなくなる。Link Key Request への Reply、BONDS、page scan の判定が全部失敗する。
  - 「p004 の試験は通る」という記述は成り立たない。
  - 再 pairing の時の `btd_keys_write` は struct の field しか書かないので、`hid=*` が全部消える。
- 修正案:
  - descriptor の cache は別の file に置く（例 `<addr>-bredr.hid`、`btd_address_type_parse` が拾わない名前）。
  - read-modify-write の口と、再 pairing の時に HID の field をどう扱うかを設計に書く。

**B5. controller が消えた・reset された時に HID の device を片付けない（key が押されたまま残る）**
- 根拠:
  - `btd_close`（`main.c:491-523`）が片付けるのは pair と scan だけ。
  - session は reset の notice と Hardware Error で ERROR になる（`session.c:642-646,676-680`）。この時 Disconnection Complete は来ない。
  - §4.7 の 250 行は、片付けを Disconnection Complete の時だけにしている。
- 起こる状況:
  - resume の後の re-enumerate、USB の抜き差し、0x80 の reset で、hid-host の fd が開いたまま残る。押された key が離されず、受け入れ 3 [N14] に反する。
- 修正案:
  - `btd_hid_lost()` を btd_close と session の ERROR から呼び、全部の fd を close し、l2cap と ATT を捨てる。`wanted` は保つ。
  - host 試験と、loopback の 0xFC03（withdraw）で key が離れることを QEMU で確かめる。

**B6. bond 済みの HID の address から来る pairing の event への答えが決まっていない（security）**
- 根拠:
  - §4.1 の 190 行は、IO Capability Request、User Confirmation、PIN、Link Key Notification を、bond 済みの HID の address なら hid に配る。
  - しかし §4.7 と §4.8 に、hid がどう答えるかが無い。
  - 今の pair.c は、自分の相手でない address には Negative で答えている（`pair_io_request`・`pair_confirm`・`pair_key_request`）。
- 起こる状況:
  - BD_ADDR を偽った相手が、pairing の mode の外で Just Works の再 pairing を始められる（実装が受ける形にした場合）。
  - Link Key Notification で保存した鍵が上書きされ得る。design §6.5 の「pairing の mode の外では断る」に反する。
- 修正案:
  - pair が持ち主でない時は、IO Capability を Negative（0x18）、Confirmation・Passkey・PIN を Negative、Link Key Notification は保存しない、と明記する。
  - LE の HID の link では、SMP は Pairing Failed（Pairing Not Supported）で答え、Security Request には暗号化を始めて答える。
  - router の host 試験に、これらの場合を入れる。

**B7. PAIR と CONNECT の排他が無く、p002〜p004 の回帰は設計のままでは PASS しない**
- 根拠:
  - §4.12 は、01（class 0x002540 で Peripheral）と、表に無い device（「試す」）を、pairing の後に自動で CONNECT する。
  - `bt-pair-p004.sh` は、その直後に 01 を再 pairing し（57 行）、07・05・06 を pairing し（62〜66 行）、FORGET する（69 行）。
  - loopback は BR/EDR の handle が 0x0040 の 1 つだけで、`device[6]` も 1 つだけ（`bt-hci-loopback.c:189-204,1184-1203`）。
  - 設計にあるのは「SCAN と PAIR の間は新しい CONNECT を始めない」だけで、CONNECT 中・OPEN 中の PAIR は定めていない。
  - 04 を LE の広告の報告に足すと、`BT SCAN devices=4`（`bt-daemon-p003.sh:47`、`bt-pair-p004.sh:82`）が 5 になる。R14 と §7.5 (7) の「PASS のまま」と矛盾する。
- 修正案:
  - CONNECT 中・OPEN 中の PAIR は busy にするか、HID の流れを片付けてから始める。PAIR は自動の page より優先させる。
  - 自動の CONNECT は class・appearance が HID の device だけにし、「試す」は外す。
  - loopback を link ごとの状態に作り直す。
  - 04 は pre-bonded なので広告には出さないか、p003・p004 の期待値の変更を i02・i03 の範囲に明記する。

---

## Should（実装の前に直すのが望ましい）

**S1. 暗号化の前に来た L2CAP の request に、即 Security Block を返している（§4.2 198 行、§4.7 248 行）**
- 受ける側（acceptor）の普通の手順は Pending（result 0x0001、status は認証待ち）を返し、自分から Authentication Requested と Set Connection Encryption を出して、終わってから最終の答えを返す形。
- 相手が自分から暗号化しない場合も考えていない。
- loopback に「暗号化の前に PSM 0x11 を要求する」変種を足して確かめる。

**S2. LE の再接続の直後の notification を失う**
- bond 済みの client の CCC の値は、接続をまたいで保たれる（GATT の規定。節の番号は未確認）。
- そのため Q6 の「LE は CCC を書くまで notification が来ない」（361 行）は誤り。再接続で起こした最初の key や click が、「知らない handle」として捨てられる。
- handle と Report Map を cache するか、(handle, 値) を setup まで待ち行列に入れ、後で流し直す。

**S3. HOGP の上限の扱い（§4.6）**
- characteristic が 17 以上で `protocol` として切ると、report の多いキーボードやマウスが使えない。上限を上げ、余りは無視する形にする。
- HID service が複数ある時に最初の 1 つしか使わないと、consumer control の key などを失う。service ごとに hid-host の device を作る。
- Read Blob の終わり: 長さが MTU−1 の倍数の時は、Invalid Offset（0x07）、Attribute Not Long（0x0B）、空の応答のどれも「終わり」と扱う。

**S4. LE の filter accept list・resolving list と session の扱い**
- initiating の最中に list を変えるのは仕様で許されていない（Command Disallowed）。hold/release を、PAIRED・FORGET・refresh の時にも使う。resolving list を変える時は、address resolution を一度止める。
- LE Enhanced Connection Complete を受けるには、LE の event mask の変更（`session.c:1288` の `0x87,0x01` には subevent 0x0A の bit が無い）と、`session_counted_event`（1688〜1694 行は subevent 0x01 だけ）の変更が要る。file の表に session.c を足す。
- 背景の auto-connect の scan に pair と同じ 60 ms/30 ms（`pair.c:256-258`）を使うと、50% の duty でずっと動き、電力と Wi-Fi との共存に響く。低い duty の値を決める。

**S5. 再接続の方針（§4.9）**
- host から page する device を 60 秒おきに永久に page し続けると、電波を使い続け、利用者の PAIR ともぶつかる。回数に上限を付け、きっかけ（resume・CONNECT など）で再開する形にする。
- resume の「5 秒の間に report が無ければ切る」は、生きているが打鍵の無いキーボードを resume のたびに切る。L2CAP の Echo Request で生存を確かめる形にする。

**S6. Accept Connection Request の role**
- 0x01（slave のまま）だと、相手から 2 台以上が繋ぐと scatternet になる。0x00（master になる）を推奨する（推測。HID 1.1.1 の推奨は未確認）。

**S7. QoS が Guaranteed の時の Unacceptable**
- 相手の device によっては、これで接続をあきらめる恐れがある。こちらは QoS を実施しないので、記録して受けるのが安全（推測）。

**S8. write の文脈での kernel の stack**
- glue の report の経路は、`struct hid_report_input`（約 2 KiB、`hid-report.h:59-64`）、touch の出力（約 1.2 KiB）、key の bitmap を 16 KiB の syscall の stack に置く（`src/hal/amd64/task.h:20`）。その上に 512 byte の bounce がある（`syscall.c:2961`）。
- input-inject は、わざとこれらを stack に置かず open の状態に持っている（`input-inject.c:59-79`）。
- glue も作業領域を `struct hid_input` に持つ（呼び手が直列にするので安全）。

**S9. syscall の bounce の分割で report が黙って 2 つに分かれる**
- `kern_malloc` が失敗すると、write は 512 byte ずつに分かれる（`syscall.c:2812-2833,3002-3035`）。
- 513 byte 以上の report は、512 byte の report と残りの report として decode される。§3 の 180 行の「EINVAL になり得る」は正しくない。
- report ID ごとの長さで厳密に検査し、daemon は短い write を誤りとして扱う。

**S10. USB の回帰の確かめ方の穴**
- (a) FIDO の raw の経路が試験に無い。raw の経路は `fetch_layout`、`activate`、`unpublish`、`publish_report` に絡んでいる（`usb-hid.c:688-703,1476-1480,1203-1205,1053-1057`）。一方 QEMU の試験には USB の raw の device が無い（hidraw-p002 は loopback）。このままでは Q1 の条件の「WS161 の hidraw と矛盾しない」を確かめられない。
  - raw の分岐は書き換えない（規約の全文への合わせも含めて）。
  - その分岐の host 試験を足すか、5330 で本物の YubiKey を T1 で試す。QEMU の `u2f-emulated` が使えるかは未確認。
- (b) QEMU の `send-key` は送り先の device を選べない。PS/2 のキーボードがあるので、event が PS/2 に行き、偽の FAIL になり得る。`input-send-event` の `device` を使い、usb-kbd に `id=` を付ける。
- (c) QMP の `device_add`・`device_del` で、抜き差し（detach と unpublish、§2 の危険 (c)）と、抜いた時の key の離しを試験に足す。
- (d) §7.1 の新旧比較で、emit の列だけでなく、register に渡す情報（capability、axis、properties、名前）も比べる。
- (e) build の範囲:
  - pcat も usb-hid.o を link する（`platform/pcat/vmunix.mk:114`）。hid-input.o を足す。
  - `CONFIG_DRIVER_USB_BT` は pcat でも既定で y（Makefile:231）なので、それを登録の条件にすると pcat の link が崩れる。`CONFIG_HID_HOST` を新しく作る。
  - `hidraw-describe.c` は USB_HID の時しか link されない（`platform/amd64/vmunix.mk:264`）が、hid-host は `drv_hidraw_describe` を使う。

**S11. 試験の再現度と、作り手が同じことの限界**
- 外部の判定を足す。bluetoothd に btsnoop の記録を書かせ（host 試験と QEMU の両方）、host の tshark の独立した dissector で SDP・HIDP・ATT・L2CAP の byte を照合する。
- design §10.2 の CSR の dongle と host の BlueZ を、SDP・GATT の相手として使う手もある（dongle が手元にあるかは未確認）。
- loopback の振る舞いの追加:
  - 04: LE Enable Encryption の LTK・EDIV・Rand を、pre-bonded の値と照合する（違えば 0x06）。
  - 02: Write Scan Enable の page の bit が立っている時だけ Connection Request を出す。
  - 02: 自分の SDP の値どおりに、host の page を断る。
  - channel が開いた直後（0 ms）に DATA を送る。
  - 暗号化の前に L2CAP の request を出す。
- loopback の link ごとの作り直し、時刻で動く振る舞いと command の経路との排他（struct の注記は「device は command の経路だけ」）は、file の表の「変更」より大きな仕事。

**S12. attempt の区切りと依存**
- p004 は test-wait（i02 は T1-405 で FAIL、i03 を再依頼中。phase004/phase.md:7）。p005 の i02 の依存「p004 の i02 の T1 PASS」は、すでに成り立たない。
  - p005 は pair.c・l2cap.c・keys.c・main.c・loopback を変え、p004 もまだ同じ file を変えている。依存は「p004 の cleared（最後の attempt が main に入っていること）」にする。
- i04 の「p004 の i03」は、5330 の実機の attempt を指すのか QEMU の直しを指すのか曖昧（ws.md は「i03 は 5330」と書く）。
- Q2 は UAPI の追加なので、「入れて実装し、断られたら外す」でなく、i01 の前に決めてもらう。
- B2・B3・S1・S2 は実機でしか見えない。i04（任意）のままだと、p006 の desktop の後の p008 で初めて分かる。p006 の前に、BR/EDR と LE の実物 1 台ずつで確かめる gate を置くことを推奨する。
- i01 は危険の大きい refactor と新しい UAPI を合わせて持つので、二つに分ける余地がある。

**S13. Q1 の条件の「compositor の evdev に届く」の証拠**
- p005 の試験は、root の evdev-probe で読むだけ（R13 で p006 に回している）。
- p005 の時点では kernel の evdev の node までしか示せないことを Q1 に明示するか、sessiond が node を渡すところまでを確かめる手順を足す。

---

## Minor

- **M1.** 事実 23 の ioctl の group の一覧が欠けている（'G' は gpu の header に多数、'f' は `fcntl.h:43` と `ioctl.h:43` の FIONBIO）。'h' が空いていることは正しい。
- **M2.** parser は EOPNOTSUPP も返す（`hid-report.c` の 383、1109、1165、1259、1515、1630、1660、1790 行）。
  - §3 の errno の一覧に無く、§7.2 (a) の fuzz の判定（0・EINVAL・E2BIG・ENOMEM だけ）では偽の FAIL になる。
  - FIDO の EOPNOTSUPP と意味が重なるので、FIDO には別の errno を使う。
- **M3.** touch の名前「<name> Touchscreen」は 63 byte を超え得る。register は ENAMETOOLONG で断り（`input.c:2468`）、publish 全体が失敗する。setup に touch の名前が無いので、切り方を決めるか field を足す。
- **M4.** HIDP の「1 + 1024」は `BTD_L2CAP_MAX 1024`（`acl.h:26`）を超え、組み直しの段階で捨てられる。自分の MTU が 672 なので、実際の report の上限は 671。SDP の 1008 を「672 でも 1 回で返せる長さ」とする理由も誤り（害は無い）。
- **M5.** appearance は「上位 6 bit」でなく、category の上位 10 bit。範囲 0x03C0〜0x03FF は正しい。
- **M6.** 63 byte の boot keyboard の descriptor の例は HID 1.11 の Appendix E.6。B.1 は boot の report の形の節。
- **M7.** report の write の EINVAL を daemon がどう扱うかが決まっていない。宣言より長い report（padding を付ける device）は、EINVAL で断らず切り詰める方がよい。
- **M8.** `struct hid_host_device` の reserved を使い、`flags`（report ID を使うか）と `report_max` を返すと、Q12 の推定を kernel の layout と照合できる。この ioctl は open の mutex の下で行う。
- **M9.** setup の途中の失敗（prepare は成功し publish が失敗）では layout を destroy し、やり直せるようにする。子が SCM_RIGHTS の fd を受け取る前に死ぬと、送られる途中の fd が open の枠を占め続ける。
- **M10.** setup の「100 ms おきに 3 回」のやり直しは、1 本の thread の loop を止めないよう timer で作る。
- **M11.** `signal_configure_request` の `response[16]` は、QoS（6 + 24 byte）や RFC（6 + 11 byte）の Unacceptable の答えが入らないので広げる。
- **M12.** HID_CONTROL の SUSPEND・EXIT_SUSPEND は host から device への message。sleep の時に送る案もある（任意）。

---

## 照合した物（記憶による照合、仕様の PDF とは未照合）

**正しいと見た値**
- ATT: opcode と error code、Read Blob と MTU−1 の関係。
- GATT: UUID（0x2800・0x2803・0x1812・0x180F・0x180A・0x2A4A〜0x2A4E・0x2A22・0x2A33・0x2A19・0x2A50・0x2A05・0x2902・0x2907・0x2908）、Report Reference の形、PnP ID の形、Protocol Mode。
- HIDP: type、HID_CONTROL の parameter、HANDSHAKE の値、DATA の type、SET_PROTOCOL の値。
- SDP: PDU ID、parameter の並び、continuation、data element の type と size index、HID と PnP の属性の ID。
- L2CAP: Connection Response の result、option の type、QoS の 22 byte、RFC の 9 byte、Configure の result。
- HCI: 0x0409・0x040A・0x0411・0x0413・0x1405・0x1408・0x0C1A（page scan は bit 1）・0x2011・0x2027・0x202D・0x2019 の parameter の順・0x200E、subevent 0x0A。
- その他: CoD の Major class Peripheral（0x05）。

**code と照合して正しかった物**
- `sizeof(struct hid_host_setup)` は 4324。
- rdev `0x00130000` は空き。
- devfs の名前の規則が "hid-host" に当たらないこと。
- usb-hid の行番号（事実 1）。

**未確認のまま残る物**
- LE Connection Complete で RPA が identity に戻るか。
- HID 1.1.1 の role の推奨。
- QoS Guaranteed への相手の反応。
- freestanding と ASan の組み合わせ。

---

## 実施と未実施

- 実施:
  - plan: phase.md 全文、design.md §5.2〜§6.6・§9・§10、ws.md、phase004 の該当行。
  - kernel: `usb-hid.c`、`input.c`、`input-device.h`、`hid-report.[ch]`、`hidraw.c`、`input-inject.c`、`devfs.c`、`syscall.c`、`vfs.c`、`bt-hci-loopback.c`、`platform/{amd64,pcat,arm64}/vmunix.mk`、`Makefile`。
  - bluetoothd: `session.[ch]`、`l2cap.[ch]`、`acl.h`、`pair.c`、`keys.[ch]`、`main.c`、`privsep.[ch]`。
  - compositor と sessiond: `input-zedbsd.c`、`wayland/input.c`、`kwl.h`、`sessiond/seat.c`。
  - 試験: `bt-pair-p004.sh`、`bt-daemon-p003.sh`、`guest.py`、`hidraw-p002.sh`。
- 未実施: build、host 試験、QEMU、実機、仕様の PDF との照合。
