<!-- awesome-plan project=zedbsd record=queue-q404 -->

# Queue q404: 実 package を guest で build（ws046-p004）

<!-- awesome-plan-current:start -->
Status: finished（2026-09-24）
Active Queue: なし
Executor: メインセッション（サブエージェントは使わない）
<!-- awesome-plan-current:end -->

Approval: 2026-09-24 ユーザー指示「自走を続けてください。」と WS046 の計画。範囲は [ws046-p004](../ws046/phase004/phase.md)。
Timebox: このセッション。

| Order | Attempt | Phase | Status |
| --- | --- | --- | --- |
| 1 | q404-i01 | [ws046-p004](../ws046/phase004/phase.md) | cleared（ユーザー判断。guest の compile の遅さは BUG-033 として p007 に、残りの build は p008 に引き継ぎ） |

依存: ws046-p003・p006（cleared）、WS042・WS043（完了）。人間の判断は要らない。

Upcoming Work Outlook: ws046-p007（guest の clang の遅さ、BUG-033）、p008（guest で package を最後まで）、p005（規約と回帰）、WS047 p001、WS045、WS049〜WS052（優先度の指示待ち）。

結果: ws046-p004 cleared（ユーザー判断で引き継ぎ付き）。host で coreutils 9.12 を我々の make で build・check でき（coreutils の試験 FAIL 0、gnulib の 1 件は GNU make でも同じ環境の失敗）、
guest では expat の configure と、再帰の make・libtool・clang の連鎖まで動いた。guest の compile が遅すぎる（8 file に約 51 分、clang の CPU 時間は一部だけ）ので BUG-033 とし、
ws046-p007 で測って直す。guest での package の build の残りは ws046-p008。
