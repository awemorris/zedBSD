<!-- awesome-plan project=zedbsd record=ws089-p019 -->
# ws089-p019: titlebar の検索欄から Down・Up で app の検索結果へ移る（compositor 側）

Status: cleared（2026-10-08 Q1 の判定（sweep-beta2-rc2 §2）: p012 の q805 の compositor の直しで済み、T1-238 PASS）（旧: planned）
Disposition: normal
Parent: [WS089](../ws.md)

## 範囲（ws089-p012 / q619 の未達、Q1 の技術判断で compositor の Phase に分けた）

zdesktop の titlebar の field（`userland/desktop/wayland/titlebar-shell.c`）が Down・Up を食べ、Settings に届かない。field が Down・Up を受けたら、field を LEFT（または同等の終わり方）で終えて client に key を渡すか、client へ転送する。Settings（と titlebar の検索欄を使う他の app、Files）で、欄から Down で結果へ移れることを確かめる。C 基準と titlebar の試験の回帰、boot-test。所有 path: `userland/desktop/wayland/titlebar-shell.c` ほか要る所（P2）。
