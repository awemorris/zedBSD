# ws197-p001: 第 1 版（711caae2f）の design-reviewer の指摘（2026-10-09 深夜、agent abb385aae6187503d）

reviewer は ws197・ws143・ws170 の記録、Guardrail、bluetoothd・audiod・phone-shell.c・libkeiland・Phone の store.c・USB の core と xHCI・EHCI・usb-bt.c を読んだ（編集・build・QEMU なし、.internal/ と GPL の source は読んでいない）。「(spec, verify)」は reviewer の仕様の記憶で、文書で確かめていない物。各指摘への答えは [phase.md](phase.md) の第 2 版の `[Rn]`。

## Blocker

- **R1** 電話から Phone の app の保存までの経路が data を失う: libkeiland の phone の event の ring は 16 個で満ちると古い物を捨てる（`userland/desktop/libkeiland/system/system-private.h:36`、`system-view.c:638-642`）。SYNC の数百通・千件を event で流すと消える。本文は `KL_PHONE_TEXT_MAX` 1024（keiland.h）・`PHONE_TEXT_MAX`（`phone-shell.c:44`）で切れる。app が閉じている間の受信はどこにも保存されない。sync の目印が bluetoothd にしか無い。→ app の目印から pull する同期（page と ack）、本文の上限と超えた時の定め、app の起動時の再同期。
- **R2** router と session が phone link を運べない: 相手からの Connection Request は HID の `wants` だけ（`router.c:513-523`）、Link Key Request も pair と HID だけ（544-560）。SCO の Connection Request は断られ、Synchronous Connection Complete（0x2C・0x2D）は router の switch に無く pairing へ（312-313）。session は ACL の link だけを数え（`session.c:~1679`）、SCO の H4 の packet は event として解析されて壊れ物と数えられる（655-666）。SCO の buffer の数も無い。pairing の handoff は HID だけ（`main.c:331`、`hid.c:~850` が周辺機器でない class を断る）。pair の l2cap に accept の hook が無い。HID 5＋phone＋pairing＝8 で「断る 1 本」が無い。→ session・router・pair・main の変更を部品に、phone の wants・claims、Link Key Request、SCO の振り分け、session の SCO の経路と buffer、handoff の鎖、link の数え直し、page scan と再接続の方針。

## Major

- **R3** 許された人が他の人のスマホを奪える（bond は system 共有、スマホの許可は機械に与えられる。D8 の人なら誰でも PHONE LINK on できる）。→ 持ち主の変更は持ち主か root だけ、新しい持ち主は FORGET と再 pairing、「居る」は seat の人、居ない間は Connection Request を断る、uid の削除・再利用、Q2 に。
- **R4** socket の出力が daemon 全体を止める（`btd_write` は長い物を切り、EAGAIN で 1 秒まで poll して client を落とす、`main.c:1650-1703`）。SUBSCRIBE は無い（F-086）。→ client ごとの non-blocking の出力の queue と上限、RFCOMM の credit を queue の空きに、SUBSCRIBE は F-086 の一部の昇格、SCO の時間の予算。
- **R5** session の 16 frame の共有の送りの queue（`session.h:307`、`session.c:453-456`、満ちると ENOBUFS）で RFCOMM の credit だけの UIH が失われ相手が止まる、HID の出力が飢える。→ link・持ち主ごとの割り当てか phone の自分の queue、credit は捨てない（数えて送り直す）。
- **R6** 依存を満たしたように書いている（WS143 p005 は in-progress、p006 は test-wait、p003 i02 は未着手）。WS197 の code は 10/17 まで保留の branch で、WS143 の router.c・hid.c・pair.c・session.c はまだ変わる。→ p002 は WS143 p005 cleared と merge の後、p008 は p003 i02・p005 i04 の後、保留の branch の取り込みの規則。
- **R7** iPhone の MAP が HFP の接続を先に要るなら、Q13 の早い確かめは p006 まで成り立たない（ユーザーの順と衝突）。→ ユーザーに: 遅い確かめを受ける／HFP の SLC（音なし）を前に／Android だけで早い確かめ。
- **R8** MAP の SetNotificationRegistration・SetMessageStatus・UpdateInbox の Put は End of Body に filler 0x30 が要る（Body の無い Put は削除の意味）(spec, verify)。
- **R9** bMessage の `LENGTH` は `BEGIN:MSG<CRLF>` から `END:MSG<CRLF>` までの全体を数える (spec, verify)。機種で違う。→ 寛容な読みと仕様どおりの組み立て、両方を試験に。
- **R10** SDP の server: 128 bit の UUID での検索（Base UUID で 16・32・128 を照合）、ServiceRecordHandle（0x0000）と BrowseGroupList（0x0005＝PublicBrowseRoot 0x1002）が無い。Device ID の VendorIDSource 0xFFFF は「未割当」でなく予約 (spec, verify)。→ 照合と 2 属性を足し、Device ID の record は外す。
- **R11** 連絡先と message の設計が WS170 の実の保存と合わない: store は `contacts/` の直下だけを読み上限 1024（`userland/desktop/phone/store.c:12,42,395-410,659`）、message は `messages/<連絡先の id>/` で、知らない番号は連絡先を作る（30 日の同期で大量の連絡先）。同期ごとの作り直しは cloud の上で file を大量に書き換える。PBAP の UID は任意。→ WS170 の store の変更を明記、差分の更新、Q3 を費用つきで、WS170 の code の変更は Q1 が割り当てる WS を跨ぐ仕事。
- **R12** app が閉じている間の SMS の通知が未設計（banner は着信だけ）。→ compositor の message の banner（lock の画面・他の人への見え方を含む）か、受け入れを狭める。
- **R13** 試験の相手が未確認: dongle は WS143 で尋ねたが答えが無い（`plan/ws143/phase005/phase.md:458`、2 本要る）、obexd の MSE は MNS の event を送れない見込み（推測）→ project で書く小さな台本の MSE（host の BlueZ の RFCOMM の socket の上）、PipeWire の AG は modem が無く RING・+CLIP を台本にできない → oFono＋phonesim か台本の AG、QEMU の SCO（qemu-xhci の isochronous と usb-host の passthrough、clone の CSR の SCO の不良）は未確認、両端が zedBSD の code の試験は対称の誤りを捕まえない → 仕様の vector と実機の trace を正解に、mSBC は独立の decoder と照合。
- **R14** p007 に大きな kernel の仕事が入り、Q9 が事実と違う: USB の core には isochronous の URB の API が既にある（`include/drivers/usb/usb.h:223` `struct drv_usb_iso_packet`、958 `drv_usb_urb_setup_isochronous`、`src/drivers/usb/usb.c:2308`）、無いのは host controller の側（xHCI の Isoch TRB、EHCI も ENOTSUP `pci-ehci.c:1823`）。それは kernel の内部で UAPI ではない。usb-bt は interface 1 を取らない（`usb-bt.c:12-14`）→ `drv_usb_interface_claim`（usb.h:743）と `drv_usb_interface_set_alternate`（738）。一番危険な xHCI が最後で p006 に理由なく依存。→ 別の Phase か WS に分けて p001 の後に並行、Q9 は `BT_PACKET_SCO` と alternate の ioctl だけ、「HAL の変更は無い見込み、DMA・cache の API が要れば止めて聞く」。

## Minor

- **R15** OBEX の Connect で MAP の MapSupportedFeatures（tag 0x29）と PBAP の PbapSupportedFeatures（tag 0x10）を送る (spec, verify)。
- **R16** RFCOMM: N1 は自分と相手の L2CAP の MTU の小さい方から 6 を引く、最初の credit K は 7 まで、MSC は仕様の必須、PN の答えが CL=0 の時の扱い、profile が無効か持ち主が居ない時の HF・MNS の channel は DM。
- **R17** OBEX: Unicode と byte 列の header の長さは HI と長さの 3 byte を含む、空の Unicode（長さ 3、NUL 無し）は正しい（SetPath の root、一覧の Name）、Connection ID は request の最初の header、最大 packet 長の最小は 255。
- **R18** MAP: `datetime` はスマホの local time（UTC の offset は任意）→ UNIX 秒と FilterPeriodBegin への変換、MAP 1.3 以降の Database Identifier と Persistent Message Handle、DOCTYPE（内部 subset 無し）は飛ばす。
- **R19** §3.3 の自己矛盾（SDP に HID の規則と bond していない相手の SDP）。SDP は mode 4 の security level 0 (spec, verify) で Pending は相手を止め得る。→ bond 済み・暗号化を保ち、誰が暗号化を始めるかを書く。
- **R20** 通話の音: SCO の clock と audiod の device の clock のずれ（適応の間引き・挿入）、audiod の録音の変換は線形補間で anti-alias が無い（`mix.c:81-93,245`）、laptop の speaker の echo の打ち消しが無い（Q7 で headset を勧める）、mSBC の空中の frame は 60 byte（H2 2・frame 57・pad 1）、sync word 0xAD。
- **R21** `kl_backend_phone` の unsupported の実装（`libkeiland-backend/unsupported/`）と check.sh、Settings の「スマホとして使う」の libkeiland・compositor・backend の経路（`kl_system_bluetooth_*` の拡張、KL_VERSION）、`phone.backend` の値、v1 の client の版の判定。
- **R22** btsnoop の trace（`-s`）は ACL を 1100 byte まで記録する（`snoop.h:33`、`main.c:2027-2042`）→ RFCOMM の payload を伏せるか phone の link で `-s` を断る、p008 の実の SMS の証拠の扱い、cloud の `~/Documents` に置くことを Q2 に。
- **R23** 参照の誤り: 226 行の「§11 Q9」は Q14、`btd_sdp` は sdp.h:56-65、§3.4 の 200 通と Q4 の 500 通、Guardrail の配置の規則は 33 行あたり。
- **R24** Class of Device の service class の bit と EIR の UUID（未確認）、suspend の振る舞い、Calls が無効でも HF の record を出すと相手が繰り返す、見積もり 121 LW は R2・R4・R5・R11・R21 を含まない。

## reviewer の結論

層・profile と役の選び方、protocol の定数（OBEX の header id、MAS・MNS・PSE の Target の UUID、SDP の属性 0x0317・0x0311、RFCOMM の制御の値、FCS の範囲と CL 0xF0・0xE0、HFP の SLC の順、AT の command、Voice Setting 0x0060・0x0063）は正しい。今の code との統合（R1〜R5）が block。第 2 版で R1〜R14 を直して再 review すれば p002 を始められる。protocol の層の指摘のうち p003（MAP）の前に要るのは R8・R9 だけ。
