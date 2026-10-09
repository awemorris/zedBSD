# ws197-p001: 第 2 版（18e611ecc）の design-reviewer の指摘（2026-10-09 深夜、agent a6b845b338bd97e31）

編集・build・QEMU なし、.internal/ と GPL の source は読んでいない。「(spec, verify)」は仕様の記憶、「推測」は code からの推論。

## p002 の前に直す

- **S1（major）** session の同期の command の待ち（最大 2 秒、`session.c:398-419`、118 行）の間の受けは 32 KB の ring で、満ちると黙って捨てる（1589-1592 行、`session.h:45`）。L2CAP basic は再送しないので RFCOMM・OBEX が壊れ、credit も食い違う。→ 与える credit の合計を queue の一部に抑え、link ごとの drop の印で RFCOMM の session を作り直して同期をやり直す。
- **S2（major）** `session_flush` は全 link の FIFO で、controller の ACL buffer の pool は共有（`session.c:1889-1904`）。frame の数の割り当てでは先頭の詰まりと pool の共有が解けない。→ link ごとの in-flight の上限（phone は pool.total−2）と round-robin、HID の遅れの試験。
- **S3（major）** credit を socket の出力の queue に結ぶのは誤り（HFP の AT・MNS が止まり SLC・登録が切れる、自分の OBEX の timeout）。→ credit は bluetoothd の中の buffer の空きだけ、client の遅さは PAGE の間隔と dropped の印で吸収。
- **S4（major）** handoff は同期の hook（`pair.c:1700-1735`）で非同期の SDP を条件にできない。pairing の時点で持ち主が居ない。bond に pairing した人の uid が無い（`keys.h:37-57`）ので、持ち主の無い bond 済みのスマホを別の人が取れる。→ `PAIR addr phone=1` で pairing した client の uid を持ち主として bond と同時に記録、持ち主の無いスマホは LINK で取れない、LINK off は enabled=0、FORGET は .phone も消す、handoff は CoD と phone の意図だけ。
- **S5（major、spec, verify）** MAP 1.2 以降・PBAP 1.2 は GOEP 2.0（OBEX over L2CAP）を必須にしている記憶。MAP 1.4 を名乗って GoepL2capPsm を出さないのは不適合の可能性、1.1 を名乗ると features が使えない可能性。ERTM を作らない決定とぶつかる。→ p002 の前に節を確かめ、必要なら ERTM の後回しをユーザーに戻す。
- **S14（major）** Write Scan Enable は hid.c だけで HID の bond がある時だけ（`hid.c:259-261,2366-2384`）。phone と HID の page の調停が無い。→ scan と page を 1 つの部品で調停。
- **S18（minor）** p004 の依存に WS143 p006 が無い。p002 の依存「WS143 p005 cleared」は i04（実機の門）を含み強すぎる → 「i02・i03 の merge」。p002 の credit が p003 の outq を前提（S3 で解消）。

## p003 の詳細設計まで

- **S6（major、spec, verify・推測）** 最初の同期の GetMessage がスマホの未読を既読にし得る → listing の read を記録し、Get の後に戻す、または subject で済ませる。
- **S9（major）** phone 以外の client は今の `btd_write`（`main.c:1650-1703`）で loop を 1 秒止め得る。client の枠 8（`main.c:75`）のうち compositor が 4 本（`bluetooth-zedbsd.c:22`）。→ outq を全 client に、枠の増加か予約。
- **S10（major）** seat の判定はその時の `stat("/dev/gpu0")` だけで、logout・切り替えの event が無い → 定期の見直しか `/dev/system` の seat の event、持ち主の client の切断も合図に。
- **S20（minor）** `PHONE SEND … length=n` の受け側が無い（入力は 512 byte の行、`main.c:752-797`）。SUBSCRIBE の event と request の答えの混ざり。→ binary の mode と SUBSCRIBE 専用の接続。
- **S23（minor）** MAS の操作（PAGE・live の Get・既読・送信・SetPath の状態）の直列化の queue。

## p004・p005 の詳細設計まで

- **S7（major）** pull の同期の残り: libkeiland の page の buffer（`system.c:3188-3234`、`system-view.c:631-647`）、compositor は client の出力が 1 MB を越えると event を ENOBUFS で捨てる（`wire.c:115-122`）、`dropped` は libkeiland の中で起きる、目印に重なりが無い（遅れて届く SMS・timezone）、cursor を接続をまたいで残すと意味が変わる。→ page_end に数と error、request ごとの buffer、live の通し番号、24 時間の重なり、cursor は 1 回の同期の中だけ。
- **S8（major）** `struct kl_phone_event` は公開の struct（`keiland.h:1652-1665`）で大きさを変えると ABI が壊れる。manager の版は libkeiland ごと（`system.c:3360-3362`）で app ごとに分けられない。→ 新しい関数（`kl_system_phone_sync_page`・`kl_system_take_phone_item`）を足し、呼んだ app だけ新しい event。
- **S11（major）** 今の通知は lock の画面で出さない（`notify-popup.c:345-357`、WS156）、2 つの button は通知の model に無い、lock の画面の応答は新しい UI、名前は app の store にしか無い。→ WS156 の notify を使い、button と lock は WS156・lock の持ち主への依頼、Q8 を衝突を説明して尋ね直す、名前は `+CLIP` の alpha・bMessage の originator の vCard（spec, verify）か番号。
- **S12（major）** store は item が連絡先に付く model（`store.c:198,267,519`）、番号の正規化、file の名前が決まらない形で 2 台の同期で重複、`bt-<address>/<hash>` の `/`、全部を読む store の上限（6000 < 5000+1024）。→ 会話の key の model を WS170 の設計の変更に、Source の hash から file 名、正規化の規則、上限と見積もり。
- **S24（minor）** 保留の branch で版（manager 24、KL_VERSION 75）を「次の番号」に固定すると衝突 → 番号は merge の時に Q1。

## p007a・p007b の詳細設計まで

- **S13（major）** loop を止めるのは同期の HCI command（最大 2 秒）も。SCO は同じ node の読みに来るので別の thread に分けられない（推測）。UAPI の形が変わり得るので Q9 は p007a の詳細設計の後に。
- **S15（minor）** SCO の Disconnection Complete は route の表に無く捨てられる（`router.c:439-470`）、Connection Complete は link type を見ない（325-358）、同期の接続を断るのは Reject Synchronous Connection Request 0x0434（spec, verify）。
- **S16（minor）** bt-hci の class の driver（`src/drivers/generic/bt-hci.c`・`bt-hci-proto.c`）の SCO の経路、isochronous では NAK で待たせられない（古い SCO を捨てる）、isochronous の frame から SCO packet の組み立て。

## 随時

- **S17（minor）** snoop は生の H4（`main.c:2027-2042`）で続きの断片に header が無い → phone の handle の ACL は L2CAP の header より後を全部伏せ、SCO は記録しない。試験の正解は伏せない台本の相手から。
- **S19（minor、spec, verify）** phone link には認証された（MITM）link key を要求する。
- **S21（minor）** `main.c:1503-1510` は `btd_ask` の中、seat の判定は 1567 行からの関数（1590 行付近）。WS143 の tests の config は WS143 の完了で消えるので WS197 の tests に。audiod の device の rate は 48 kHz と決まっていない（`device.c:100`）。
- **S22（minor）** 見積もりに S1・S2・S9・S8・S11・S12・S16・S14 が入っていない。p004 の 18 LW は過小。
- **S25（minor）** HID の上限 6→5 は phone link を有効にした時だけ枠を予約すれば減らさずに済む。

## R1〜R24 の状況（reviewer の判定）

解決: R6・R7・R8・R9・R10・R14〜R20・R23。ほぼ解決: R13（dongle は未回答）・R24（見積もり）。一部: R1（S7・S8）・R2（S4・S14・S15）・R3（S4c・S10）・R4（S9・S3・S13）・R5（S2・S1）・R11（S12）・R12（S11）・R21（S8）・R22（S17）。

## 結論

p002 はまだ始めない。S1〜S5・S14・S18 を直した第 3 版が p002 に関わる節だけの短い再確認を通れば始めてよい。
