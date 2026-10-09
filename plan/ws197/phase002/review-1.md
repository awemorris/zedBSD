# ws197-p002: 詳細設計の第 1 版（9060f17e3）の review（2026-10-10、design-reviewer agent a70a3277735e7af2e）

読むだけ（編集・build・QEMU なし、.internal/ と GPL の source は読んでいない）。仕様の値は公開の PDF の抜き書き（Core 5.4、RFCOMM 1.2、TS 27.010、MAP 1.4.2、PBAP 1.2.3）で照合。

## 判定

一部 GO: i05〜i07（rfcomm.c・obex.c・sdps.c と sdp.c の一般化、WS143 の file に触れない）は今から可。i01〜i04（session.c・router.c・pair.c・l2cap.c・hid.c）は B1・M1〜M6 を本文に直し短い再確認の後。

## blocker

- B1: 封が 1 回の待ちで解け、drop の後の同じ link の ACL（続きの断片）が知らせより先に渡り、`btd_reassembly_feed`（acl.c:118-140）が別の frame の続きをつなぐ（handler の中の command の待ちが ring に積む、session.c:240-265・781-784）。→ link ごとの封を知らせを渡し終えるまで。知らせの後は first の断片まで続きを捨てる（か持ち主が組み立てを捨てる）。2 つの待ちが続く場合の試験。

## major

- M1: 封が全 link に効き、phone の洪水で keyboard も作り直しになる。→ link ごと。§15 の行を消す。
- M2: `last_cid` を dispatch でも更新すると、ring の古い packet の dispatch が後に積んだ物の CID を上書きする。→ 着いた順（read の時）だけで。
- M3: `btd_session_pending` が `queue_used` しか見ず（session.c:388-397）、loop が poll で待つ（main.c:625-669）と知らせが次の packet まで出ない。→ 知らせが残る間は pending 1、handler の前に印を消す。
- M4: SCO・eSCO を Reject Connection Request（0x0409）で断るのは根拠が無い。Core Vol 4 Part E 7.7.4 は Accept/Reject Synchronous、7.1.28 は AES-CCM の link への SCO を Reject Synchronous（0x042A）reason 0x0E で断るよう shall。→ 0x042A、AES-CCM なら 0x0E 他は 0x0D、Synchronous Connection Complete（0x2C）は明示して捨てる、router_connected と linkmgr へは link type ACL の Connection Complete だけ。
- M5: local CID は枠の番号（l2cap.c:218・712、signal_find 1113-1121）。「空いた枠へ」写すと引けない。→ 同じ枠へ。空いていなければ断る。試験で CID で引ける事。
- M6: 回復の表に RFCOMM 以外の CID の DATA の行が無い（HOG の ATT 0x0004、LE の SMP 0x0006）。→ HID は control・interrupt・ATT のどれでも hid_fail、pair は持ち主の link の DATA なら何でも stop。

## minor

m1 Hardware Error・Data Buffer Overflow が捨てられ得る（session.c:690-707）。m2 scan の report が ACL より優先され、捨てると EVENT で HID・pair を止める → 最下位、EVENT を立てない。m3 照合が handle だけ → address も。m4 0xF0 は bluetoothd の private と注記。m5 p002 の phone の wants は認証の無いまま Pending で止まる、assign と Accept が本文に無い。m6 phone が受けず HID が受けると pair の Pending の channel が btd_l2cap_drop で黙って消える（pair.c:1733）。m7 断りの signal を 1 つの C-frame に積むと MTUsig の最小 48 を越え得る。m8 HID の上限が枠と接続の数で食い違う。m9 phone=1 で保存の Just Works の鍵があると pair_key_request（pair.c:1000-1008）が使い、何度でも unauthenticated。m10 PAIRED の行は handoff の前に作られる（pair.c:1478-1497）。m11 linkmgr の page_end の呼び出しの不足、LE と BR/EDR の page の同時は調停しないと書く。m12 PN の無い DLC と DLCI 0 の N1（既定 127）、初期 credit 0。m13 credit の無い相手は予算が成り立たない → DISC。m14 N1 の −6 は 1 多い（−5）。m15 衝突は CONFIGURING・OPEN の間も、最後の DLC の閉じは相手と重なってよい。m16 btsnoop を伏せる判断が handoff の前の packet で漏れる → pair->phone の間も伏せる。m17 i02 が i04 の linkmgr に依る → 順を直す。m18 FCS の試験に length を含む正解が無い（記憶: `03 3F 01` → `1C`、`03 73 01` → `D7`）。

## T1〜T4・N3〜N5

T1 部分（B1・M2・M3・M6）、T2・N5 満たす、T3 部分（M5・m6・m7）、N4 答えている（m8・m9）、N3 答えている（m11・m17）、T4 受け入れに入っている。仕様の値（DLCI の方向、C/R、FCS の範囲、PN、T1 60 s、MSC 0x8D、OBEX の code、SDP の PDU・error・handle、Key_Type、MAP・PBAP §9）は照合して合っていた。
