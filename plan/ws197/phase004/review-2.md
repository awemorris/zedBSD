# ws197-p004 詳細設計 第 2 版の確認（review-2）

対象: [phase.md](phase.md) 第 2 版（agent/p1 の e9a737e2f）と [review-1.md](review-1.md)。bluetoothd は branch `agent/p1-ws197` の c56043c2b。
日付: 2026-10-10（design-reviewer）。file・build・試験・QEMU は触っていない（読むだけ）。(推測) は確かめられなかった点。
答え: 第 3 版で全部を本文に入れた（各節の `[MA]`〜`[MF]`・`[m1]`〜`[m14]` の印）。

## 結論

blocker 0・major 6（新しい誤り 4、部分的に閉じた物の残り 2）・minor 約 14。§5.1・§5.2・§5.4 は minor を入れれば p003 i07 に進めてよい。p004a〜c は MA〜MF を入れた第 3 版の後。

## review-1 の状態

B1 一部（done の errno と result の ring の不一致 → MA）、B2 一部（借りの中の順 → MB、印の喪失 → MC）、B3 閉じた（m4 あり）、M1 閉じた（P7 待ち、ME の矛盾）、M2 ほぼ（m5・m7）、M3 一部（MD、m8・m9）、M4・M7〜M11 閉じた、M5 一部（ME）、M6 ほぼ（m12）、M12 一部（MF、m11）、M13 ほぼ（m6）。

## Major

- **MA**: `done(uint request, int error)` を `system_view_result` に入れると書くが、それは `KL_SYSTEM_RESULT_*` を受けて `system_view_error_of` で直す。ENOTCONN・ECONNRESET・ETIMEDOUT・EMSGSIZE・ENOBUFS の対応が無く全部 EIO になり、§8 の unknown と failed の区別が壊れる（2 度送り）。直し: (a) `done` は `KL_SYSTEM_RESULT_*` を運び LOST・TIMEOUT・TOO_LARGE・NO_ROOM・NOT_CONNECTED を足す（推し）、(b) errno のまま入れる関数。
- **MB**: 借りに入った `page_end`（約 100 byte）を後の小さい `done`（16 byte）が追い越しうる（wire.c の 1 MB の判定）。直し: 借りがある間は保証の event を全部借りの後ろに順に積み、item は捨てて done を ENOBUFS に。
- **MC**: `KL_PHONE_ITEMS` の印が 16 の ring から落ちると app が item を取らず queue が止まる。直し: app は CHANGED_PHONE のたびに 0 まで取る。印は目安。
- **MD**: 番号の鍵「末尾 9 桁」で `090-1234-5678` と `080-1234-5678` が同じ（070 も）。米国の市外局番の先頭だけ違う番号も。英字の送り手は空。直し: E.164 に正規化、国番号の既定（P8）、英字・短い番号は文字列のまま。
- **ME**: Settings が phone の状態を読めず `link_set` の interface も無い（listen は gate、opcode の表に無い、link に enabled・profiles・present が無い）。直し: `link_set`（opcode 7）と `watch_link` を今の版で gate 無しに、link の欄を足す。
- **MF**: §3.4 で app が同期の item にも通知を出す（初回の同期で 500 通、compositor の通知と二重）。直し: live の新しい受信だけ。

## Minor（要約）

m1 失敗した同期の item は保存してよい（目印を進めないだけ）。m2 期限を段に（backend 120 < compositor 150 < libkeiland 180）。m3 背面の session の compositor が uid の上限 4 に掛かる（記録）。m4 PUSH の後の持ち主の変化で ERROR permission は 2 度送りを招く → 接続を閉じる。m5 cursor に limit が無い、capped の決め方（folder の大きさで）、500 は MSE が新しい順の時だけ「新しい 500」。m6 backend 2 の v1 send の channel と gate、v1 received は一切送らない。m7 深い同期を `deep_at` で、notify=0 の 5 分ごとは since を「前の開始 − 10 分」に。m8 failed にした自分の file が sent の folder と重なれば sent、zone=local の時の ±10 分の外れ。m9 partial は Source の file も候補に、既読の反映は dir=in。m10 errno の全部を unknown・failed に分ける。m11 `lock_text` を `kl_app_notify` にも、WS156 の通知が program を起動できるか未確認。m12 backend の配列の長さに名前、id は 0 にならない、page_end は伸ばせない（記録）、size が小さい時 EINVAL。m13 listen の gate の答えと見る時。m14 自分の PAIR・LINK・FORGET の後すぐ SHOW と SUBSCRIBE。

## 確かめて合っていた物

接続 7 本（compositor の Bluetooth 4・phone 3）、seat の人は予約の判定を受けない、SUBSCRIBE 2 本で足りる、map の PAGES 2・QUEUE 24、wire 65532 に item 約 17 KB、`KL_BACKEND_BT_ADDRESS_MAX` 18・`_REASON_MAX` 32、kind 10〜12、result の ring 32、map の pushed と保留の SENT。

## 判定（第 2 版）

GO: §1・§1.1（m1）、§1.2（MA を反映）、§5.1・§5.2（m4・m5 は i07 で）、§5.3（m12・m14）、§5.4（m3）、§6.1・§6.2（m7）、§6.4・§6.5、§8（m10）、§9（P8 を足す）、§10、§11（§11.6 を除く）、§12（試験を足す）。
not-GO: §0 の番号の鍵・§2・§6.3（MD）、§3（MA・MC・ME・MF）、§4（MA・MB・ME、m6・m13）、§7（MF・m11）、§11.6（ME）。
