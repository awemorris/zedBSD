# ws197-p001: 第 3 版（e6db3a2ce）の p002 に関わる節の短い確認（2026-10-09 深夜、agent a42a036a4bbde0983）

編集・build・QEMU なし、.internal/ と GPL の source は読んでいない。「(spec, verify)」は仕様の記憶、「推測」は code からの推論。

## 判定

**p002 は GO（条件付き）**。p001 の第 4 版は要らない。条件: T1〜T3 を p002 の詳細設計に書き、それが design-reviewer を通るまで session.c・pair.c・l2cap.c・router.c・hid.c の code に手を付けない。T4 を p002 の受け入れに足す。OBEX の Connect と SDP の record の版を code にするのは Q16 の答えの後。

| 指摘 | 判定 | 理由 |
| --- | --- | --- |
| S1 | 部分 | credit の合計の上限と drop の印は入った。印の範囲（事象・signalling）、知らせる時機、回復の段階が未定（T1） |
| S2 | 部分 | in-flight の上限と round-robin は入った。session の送りの表 16 frame は全 link で共有で、HID は ENOBUFS を捨てる（T2） |
| S3 | 解決 | §9.1 の古い期待値だけ残る（N1） |
| S4 | 解決 | handoff の移し替え（T3）と FORGET の権限（N2）が新しく残る |
| S5 | 部分（門として受け入れ） | 仕様の確かめを p002 の最初に、Q16。reviewer の記憶でも MAP 1.2 以降・PBAP 1.2 は GOEP 2.0 必須 (spec, verify) |
| S14 | 解決 | 細部は N3 |
| S18 | 解決 | §3.6 とのずれ（N3） |

## 新しい指摘

- **T1（major）** `session_enqueue`（session.c:1563-1611）は同期の command の待ちの中から呼ばれ、ring が満ちると ACL も事象も区別なく捨てる（1591-1593）。Connection・Disconnection Complete は数えた印の後に捨てられ得て（1584-1586）router は見ない。断片の組み立て（acl.c:93-146）は欠けた frame を黙って捨てる。→ 印は handle ごとに ACL・事象・signalling を区別、持ち主への知らせは待ちの後に dequeue の側で順に、回復は段階（RFCOMM の data だけなら PSM 3 の channel を閉じる、signalling・事象なら ACL を切って linkmgr で page し直す）、数えた接続の事象は捨てない（事象の余白の予約か ACL を先に捨てる）。WS143 の HID にも同じ危険。
- **T2（major）** `btd_session_send` は全 link で 16 frame を越えると ENOBUFS（session.c:452-456）、HID は戻り値を捨てる（hid.c:2521、3152）。→ session の表に link ごとの上限（phone は 8 まで）か予約、phone は自分の queue からその上限の中でだけ移す、phone が満杯の間も HID の signalling が ENOBUFS にならない host の試験。
- **T3（major）** pairing から handoff の時に `btd_l2cap_drop` は相手に何も送らず channel を FREE にする（pair.c:1733、l2cap.c:335-346）、組み立ても消える（1734）。スマホが開けた SDP の channel（Pending を含む）が黙って消え、SDP が timeout し得る（推測）。→ l2cap.c に pair の表から phone の表へ channel と組み立ての状態を移す API、移せない物は Disconnection Request の後に捨てる。handoff の hook の型に phone=1 と uid が無い → 型を変えるか pair から読む口。
- **T4（major）** p002 は HID の経路を変えるのに HID の回帰の門が無い。→ 保留の branch の image で WS143 の `bt-hid-p005.sh`・`bt-pair-p004.sh`・`bt-daemon-p003.sh`・`bt-loopback-p002.sh` を T1 に。WS143 が先に完了する時は script を plan/tools か tests/ のシナリオへ移すよう Q1 に。

## 細かい点

- **N1** §9.1 の「出力の queue」の行の「credit が止まる」は S3 で捨てた設計の期待値。
- **N2** §8.2 の「持ち主か root」と Q2 の「持ち主か管理者」の食い違い。今の FORGET は seat の人と wheel なら誰でも（main.c:1375-1387）→ `.phone` のある bond の FORGET を持ち主と root に限ると明記。
- **N3** §3.6 の「WS143 の p005 の後」と §12 の「i02・i03 の merge」の食い違い。linkmgr の範囲（pair の Create Connection pair.c:290・LE pair.c:275、hid.c:2889・2974 の auto-connect）と、page の結果を router から linkmgr へ返す hook。
- **N4** handoff の Class of Device は最後の scan の記録から（hid.c:837-848）で、scan の表に無いと 0 で断られる → 代わりの手。Just Works の鍵の時の書く順、`.phone` の書き込みの失敗、SDP の ServiceRecordHandle は 0x00010000 以上 (spec, verify)。
- **N5** in-flight の上限「pool − 2」は小さい pool で 0 以下 → max(1, …)、LE が BR/EDR の pool を共有する時（`le_shared`）の予約の意味。
