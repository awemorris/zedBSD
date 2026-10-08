<!-- awesome-plan project=zedbsd record=ws188-p002a -->
# ws188-p002a: Files の Today の空き容量を desktop の FILESYSTEMS へ

Status: cleared（2026-10-08 Q1、T1-428 PASS）
Disposition: normal
Parent: [WS188](../ws.md)
由来: Q1 の判断（2026-10-08、p001 の M6）「Files の statvfs（Home の空き容量）は machine の FILESYSTEMS で移す小さい Phase（ws188-p002a）を足す（規則に沿って app から system の mount を見ない）」

## 実装

- `files/ui-home.c`: `fm_home_gather` の `statvfs(app->home)` を除き、`board->opened_today` を持って新しい `fm_home_summary` が Today の行（今日開いた数と、分かっていれば「N free」）を書く。gather は `app->home_free_wanted` を立てる。
- `files/main.c`: main の loop の system の poll で、wanted なら `kl_system_machine_query(KL_MACHINE_FILESYSTEMS)`（待ちの request が無い時だけ）、`KL_SYSTEM_CHANGED_MACHINE` で `main_home_free_take`（file system の中で home の path の最も深い場所、"/home2" は "/home" の下でない）→ `home_free`・`fm_home_summary`・dirty。log `FILES HOME free path=… available=…`、`HOME free result errno=…`。
- `files/files.h`: `fm_dashboard.opened_today`、`fm_app.home_free_known`・`home_free`・`home_free_wanted`、`fm_home_summary` の宣言。
- desktop が FILESYSTEMS を持たない時（古い compositor、Keiland でない）は空き容量の部分を出さない（今は statvfs で出ていた。境界の規則による後退）。

## 確認（2026-10-08、P2）

| 確認 | 結果 |
| --- | --- |
| zedBSD `build/ws188-p002/bin/files`（config-amd64-zdesktop.mk） | rc 0、warning 0 |
| keiland-linux all | rc 0、warning 0 |
| `sh plan/tools/files/host-build.sh` | rc 0 |
| QEMU（Today の行に「N free」が出る） | 未実施（T1 の依頼） |


## Q1 の判定（2026-10-08）

T1-428 PASS: `ZFILES HOME free path=/ available=660127744`・errno=0、Today の画面に「0 files opened today · 660.1 MB free」（PNG を Q1 が目視）。cleared。