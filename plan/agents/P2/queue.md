# P2 Queue lane

| Queue / attempt | Phase | Scope | Approval | Timebox | State |
| --- | --- | --- | --- | --- | --- |
| q591 / q591-i01 | [ws099-p020](../../ws099/phase020/phase.md) | BUG-125 の原因特定と compositor の修正 | 2026-10-02 user「作業を開始しましょう。」 | 4h | finished / uncleared |

| q609 / q609-i01 | [ws099-p023](../../ws099/phase023/phase.md) | BUG-136/137 と直す前からの C 基準の失敗 | user 2026-10-02 | 4h | finished / cleared |
| q618 / q618-i01 | [ws114-p008](../../ws114/phase008/phase.md) | KDE の server decoration、既定 SSD | user 2026-10-03 | 4h | finished / cleared |
| q620 / q620-i01 | [ws099-p021](../../ws099/phase021/phase.md) | C2 の geometry・BUG-127 | 自走 | 3h | finished / uncleared |
| q622 / q622-i01 | [ws094-p014](../../ws094/phase014/phase.md) | 規約の指摘の直し | 自走 | 2h | finished / cleared |
| q625 / q625-i01 | [ws099-p024](../../ws099/phase024/phase.md) | BUG-147 の試験の頑健化 | 自走 | 3h | in-progress |

Next（予約）: debug を続ける（BUG-147 の後: 優先度の高い bug。ws089-p019 と ws113 は WS131 の後に組み直す）

## Merge requests
| P2-001 | q591 | f1af6cc7f..8bc057b36（base 901037f9f） | wayland/{preferences,seat,toplevel,zwl.h}・popup-probe・plan/ws099・zdesktop-p076.sh・BUG-125 | integrated e90d816c2 |
| P2-002 | q609 | d8943fde2・fb32c0a0b・2624e7365・e20409bbd（前回 8bc057b36） | xserver・notes・terminal・popup-probe・wlshm・vmunix.mk・plan/ws099・BUG-136/137 | integrated d5e5cfa6a |
| P2-003 | q618 途中 | 0ce650b2a | userland/tests/wlshm/Makefile.linux・freebsd（p023 の link の回帰の修正） | integrated 55383d678 |
| P2-004 | q618 | 9f274fc64・fc3c52383・0ea947662・df0b57acb | wayland/{decoration,protocol,objects,zwl.h,extras.h}・plan/ws114 | integrated 8140d36a9 |
| P2-005 | q620 | f0ee0d951（前回 df0b57acb） | plan/ws099/phase021・BUG-127 | integrated eb7235789 |
| P2-006 | q622 | 5ca38bb84・71f726edc・af3b29c53（前回 f0ee0d951） | wayland/{display,menu,desktop}・files/{thumb,ui-context}・imageview/image.c・plan/ws094 | integrated 0f6480f77 |

2026-10-03 / N=3: user「BUG-150は今実行してOKです。FreeBSDホストを空けたので使ってください。Emacsは入ってます。」→ P2 generation4 を q636（ws128-p010、BUG-150）で起動。5320（FreeBSD）は P2 が使い、P3 は FreeBSD の確認を済ませた。
| P2-merge q863 | q863-i01 | c98a659ec・52a2e4007（base 777137731） | wayland/touchpad.{c,h}・plan/ws159/tests/host-touchpad.c・plan/ws183 | integrated b776026ca（2026-10-08、実機の確認待ち） |
| P2-merge q864 p001 | q864-i01 | 77fe44dc8（merge 0f16839ac、base b776026ca） | wayland/{lock-clock.{c,h},glass.{c,h},greeter.c,Makefile*}・plan/ws187 | integrated 07096625d（2026-10-08、QEMU の PNG は p003 の後） |
