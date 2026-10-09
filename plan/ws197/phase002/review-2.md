# ws197-p002: 第 2 版（07c0df571）の短い再確認（2026-10-10、design-reviewer agent a70a3277735e7af2e）

判定: **i01〜i04 は GO**。B1・M1〜M6 は直っている（link ごとの封と skip_continuing、notice_after の期日、印を handler の前に消す、last_cid は着いた順、pending は期日の知らせも見る、Hardware Error・Data Buffer Overflow は積む時に処理、scan の report は最下位、照合は address も、DATA のその他の CID の行、Reject Synchronous 0x042A と AES-CCM の 0x0E、同じ枠への移し替えと refuse_left、断りの順、PAIRED の行、HID の同時接続の上限、Negative Reply、RFCOMM の m12〜m15、FCS の 1C・D7、試験 (a)〜(g)）。i05〜i08 に新しい指摘は無い。

残る minor（実装の中で本文と一緒に直してよい、再確認は要らない）:

1. §3.3: `session_link_add`・`session_link_remove`（session.c:1774-1840）で drop の状態（sealed・skip_continuing・drop_*・notice_after・last_cid）と送りの上限（frame_limit・inflight_limit）を初期値に戻す。知らせを待つ間に link が消えたら知らせを出さない。
2. §3.4: enqueued・dequeued・notice_after は unsigned の差で比べる（回っても期日を誤らない）。
3. §7.3: 先頭の signature が古い（signal・size・signal_length）。
4. §8.4: 「相手の PN の CL が 0xF でない時は DM」は開いていない DLC の PN だけ。開いた DLC への PN（CL 0、§5.5.3）には今の値を返す。
5. §8.3: PN 無しの SABM を受けるのは、その session で credit の流れが既に決まっている時だけ。最初の DLC の PN 無しの SABM は DM（§6.5.1）。試験に「最初の DLC の PN 無しの SABM → DM」「2 つ目の PN 無しの SABM → UA と credit」。
