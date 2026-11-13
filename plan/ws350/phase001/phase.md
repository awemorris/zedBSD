<!-- awesome-plan project=zedbsd record=ws350-p001 -->

# ws350-p001 Pi 500 を機種の表に載せる

- WS: [ws.md](../ws.md)。Queue: q2042-i01（P1、worktree `/home/awe/zedBSD-worktrees/p1-pi500`、branch `agent/p1-pi500`）。
- 状態: cleared（2026-11-13 P1）

## 変えた所

- `README.md` の「Hardware Support」の表に Raspberry Pi 500（Arm64、Cortex-A76、VideoCore VII）の行と、その下に説明の段落: Pi 5 の image で動く、外部の利用者の報告（BCM2712 D0・8 GB、2026-11-03 の source、Pi 500 の dtb を足して起動、desktop・日本語入力・Wi-Fi・有線・browser・SSH・shutdown）、今の image は dtb を持つので変更なしで起動する見込み（Pi 500 では未確認）、HDMI の音は Pi 5 と Pi 500 でまだ無い。
- `README.md` の「Try zedBSD」の機種の list を「Raspberry Pi 4/5/500」にし、Pi 500 は Pi 5 の image を使う事と HDMI の音が無い事を足した。
- `plan/tools.md` の実機の表: Pi 5 の行を今の address（10.0.30.5、2026-11-13）と chip（D0、firmware は C1 の tree を渡す）に直し、Pi 500 の行（外部の利用者の機械、報告の要点、[WS348](../../ws348/ws.md) の後は dtb を足さずに起動する見込み、HDMI の音は [WS349](../../ws349/ws.md)）を足した。
- docs/ の他の機種の表: `docs/` の下に機種の一覧の表は無い（`grep -rn "Raspberry Pi 5" docs/` は GPU・電源の reference だけ）ので変えていない。build の手順の文書（`docs/howto/build-from-source.md`）の config の表には「Raspberry Pi 5 and Raspberry Pi 500」と書いた。

## 確かめ

- 文書だけの変更。markdown の表の列の数を目で確かめた。docs/ から plan/ への link は無い。
- 未実施: Pi 500 の実機（機械が無い）。
