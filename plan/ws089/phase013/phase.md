<!-- awesome-plan project=zedbsd record=ws089-p013 -->

# ws089-p013: About の memory と Storage の使用量

Status: cleared（2026-10-08 Q1 の判定（sweep-beta2-rc2 §2）: T1-278 (1) PASS、About に Memory の行）（旧: in-progress（2026-10-06 q821 P2: About の memory の行を実装、build warning 0 と host 試験 PASS。QEMU は T1 待ち））
Disposition: normal
Parent: [WS089](../ws.md)
Queue: q821（2026-10-06 ユーザー「採る（第 1 段で）」、Q1 が libkeiland の API の追加を許可）
依存: p010、[proposed/libkeiland-system.md](../proposed/libkeiland-system.md) の許可（KEILAND_VERSION を上げる → WS113 p005 と直列）
目安: 2h（1 Queue）。実行者の目安: phase-runner-mid
所有 path: `userland/desktop/libkeiland/`（system の照会）、`include/libc/keiland.h`、`userland/desktop/settings/about.c`・`page-about.c`

## 範囲

About の頁に memory（全体と使用中）を出す。Storage の頁に volume ごとの使用量（statvfs）が無ければ足す。

## 受け入れ

About に memory の行、host の試験（`host-render.c` の about）と guest の画面。libkeiland の host 試験 PASS。

## 検証の方法と範囲

host と QEMU の Venus。 やっていない確認は「未実施」と書く。

## 未決の判断

libkeiland の API の追加の許可（main）と採否（ユーザー）。

## Event

2026-10-02 / ws089-beta1-plan: fg019 の計画で新設。

## 実装（2026-10-06 P2、q821）

- **libkeiland の API の追加は要らなかった**: 案（`keiland_system_get_info`）の代わりに、既存の machine の monitor（WS134 p012、`kl_system_monitor_open`・
  `kl_system_monitor_take`、compositor が採る `memory_total`・`memory_free`）を使う。zedBSD・Linux・FreeBSD の backend が既に memory を読むので OS ごとの
  code も要らない。`KEILAND_VERSION` も変えない（Q1 の許可は使わなかった）。
- **About**（`page-about.c`）: 頁を初めて描いた時に monitor を開き（2 秒ごと、`ABOUT_MONITOR_MS`）、「This computer」の card に「Memory」の行
  （例「16 GB (9.5 GB free)」、10 GB 未満は小数 1 桁）。monitor の無い desktop では行を出さない。`system.c` の `se_system_poll` が `se_about_follow` で
  新しい frame を取り、About の表示中なら描き直す。窓を閉じる時に monitor を system より先に閉じる。log `ZSETTINGS ABOUT monitor open=0|1`・
  `ABOUT memory total= free=`（最初の 1 回）。
- **Storage の volume の使用量**: 既に有る（`look.c` の `se_look_volumes`、statvfs、ws089-p023 の Storage の頁）ので変更なし。
- 試験の stand-in（`plan/ws089/tests/host-kl-system.c`）に monitor の 3 つの関数（`HOST_MEMORY` で 16 GB・9.5 GB free の frame）。

## 試験と結果（host、2026-10-06）

| コマンド | 結果 |
| --- | --- |
| `sh plan/ws089/tests/run-host-about-memory.sh <scratch>`（新規: monitor の有る時は行、無い時は無し） | 3/3 PASS、PNG を目視（Memory の行） |
| `run-host-welcome.sh`・`tr-host-test.sh` | PASS |
| build: zedBSD・keiland-linux の `bin/settings` | warning 0 |

未実施: QEMU（About の memory の値が compositor の monitor から来ること）は T1。

## q830（2026-10-06 P2）: T1-269 で Memory の行が PNG に出なかった

- 証拠: `/home/awe/zedBSD-worktrees/t1/build/t1-269-run/`（`logs/apps.settings.about.log`、`png/apps.settings.about-about.png`）。This computer は 5 行。
- 原因: 描画の誤りではなかった。log では memory の後に layout が作り直され、「Show Welcome again」の y が 40 下がっている（787 → 827）ので、行は後の frame で描かれている。monitor を 2 秒の周期で開いていたため、最初の memory の到着が About の表示から最大 2 秒遅れ、試験の撮影がその前だった。
- 修正: `page-about.c`。monitor を 250 ms で開き、最初の memory が来たら 2 秒で開き直す（`ABOUT_MONITOR_FIRST_MS`）。
- 確認: `run-host-about-memory.sh` に、250 ms で開いてから 2000 ms で開き直すことの確かめを足して PASS。settings の build は warning 0、style-check は 0。T1 の再試験（AAT `apps.settings.about` の PNG に Memory の行）が要る。

