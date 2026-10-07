# P1 Queue lane

| Queue / attempt | Phase | Scope | Approval | Timebox | State |
| --- | --- | --- | --- | --- | --- |
| q590 / q590-i01 | [ws004-p051](../../ws004/phase051/phase.md) | BUG-134: AX211 の passthrough での再現・解析・driver の修正と確認（phase.md の範囲） | 2026-10-02 user「PCIパススルーでQEMUを起動してデバッグしておいてほしいです。これは最初に取り組みましょう！」 | 4 時間 | finished / uncleared（実機試験待ち） |
| q594 / q594-i01 | [ws129-p009](../../ws129/phase009/phase.md) | デモの image を CI 設定の土台に、boot-test の timeout | 2026-10-02 user「デモのイメージはCI設定をベースに変更しましょう。」 | 3 時間 | finished / cleared |

| q596 / q596-i01 | [ws005-p018](../../ws005/phase018/phase.md) | WiFi の利用者の流れの調査と契約 | 継続 dispatch（user 2026-10-02） | 3 時間 | finished / cleared |

| q598 / q598-i01 | [ws033-p001](../../ws033/phase001/phase.md) | USB の LAN の hotplug を QEMU で | 継続 dispatch | 3 時間 | finished / uncleared（WiFi 優先で中断） |
| q599 / q599-i01 | [ws005-p019](../../ws005/phase019/phase.md) | BUG-138 WiFi menu・利用者の join の実装 | 2026-10-02 user「実装をお願いします。優先度高いです。」 | 4 時間 | finished / uncleared（networkd の owner 変更がユーザーの明示の承認待ち） |
| q601 / q601-i01 | [ws118-p001](../../ws118/phase001/phase.md) | 5320 の遠隔 log 用 image | 継続 dispatch | 3 時間 | in-progress（再開） |
| q599 / q599-i02 | [ws005-p019](../../ws005/phase019/phase.md) | network group の利用者に WiFi の制御を許可し desktop から on/off・join（i02） | 2026-10-02 user（明示の承認）:「WiFiの制御は、networkグループに入っているユーザには許可する、でどうですか？」 | 4 時間 | finished / uncleared（permission の再拒否、ユーザーの直接の許可待ち） |

Next（予約）: BUG-052（優先、まず再現の確認）→ BUG-120 → BUG-143（優先）→ 低優先度の確認だけの BUG-036・033・027・103。BUG-135 は WS131 p005・p006 の後

## Merge requests

| P1-001 | q590 | 5a9da8080（base 257b2bd9f） | plan/ws004・BUG-134・Bug Board | integrated 487372080（BUG-134 の衝突は P1 側を採用） |
| P1-002 | q594 | 22efda2be（base c4ed68f12、前回 5a9da8080） | plan/ws075/demo・plan/tools/boot-test.*・plan/ws129/phase009 | integrated eeecec752 |
| P1-003 | q596 | 79126012c..3019d674b（前回 22efda2be） | plan/ws005/phase018 | integrated 7cbbd3fc5 |
| P1-004 | q598 | a4669dc8e..cb6ce3e76（前回 3019d674b） | plan/ws033/phase001 | integrated b828c5372（中断、hotplug の ue1 が RX/TX 0 の途中所見） |
| P1-005 | q599 | c26fb2c78（base 4a835cb24、前回 3019d674b、q598 は cb6ce3e76 で統合済み） | wayland/network.c・plan/ws005/phase019・BUG-138 | integrated b67aa0f88 |
| P1-006 | q601 途中 | c3103cdd4・ce94725fb（前回 c26fb2c78） | ws118 の config・script・phase001 | integrated 03d75d732 |
| q599 / q599-i03 | ws005-p019 | generation2: networkd・UI・B3 の実装、QEMU・passthrough | user 承認 | 4h | finished / uncleared（AP の資格情報待ち） |
| P1-007 | q599-i03 | ff357e48c..c662d6e33 | networkd・network.c・settings/network.c・plan/ws005・BUG-138 | integrated 364ea5f22 |
| q599 / q599-i04 | ws005-p019 | 実 AP の join の試験 | user の資格情報 | 2h | finished / uncleared（permission の拒否） |
| P1-008 | q599-i04・q601 途中 | fa2289544・c47e5e499（head c2b561542） | plan/ws005/phase019・plan/ws118 | integrated fd09a68c7 |
| q607 / q607-i01 | ws005-p024 | 起動時の自動再接続 | user | 3h | in-progress |
| q607 / q607-i01 | ws005-p024 | 旧設計（member の store を起動時に読む） | — | — | finished / uncleared（permission の拒否、設計はユーザーが改訂） |
| P1-009 | wrap | b6e5ac6ef・348850406・d108d0a0d | plan/ws005/phase024・plan/ws118 | integrated 14ea3f606（phase024 の衝突は両方を残した） |
| q611 / q612 | ws005-p019・p024 | 実 AP・自動再接続 | user | — | finished / uncleared（5330 の hang） |
| P1-010 | q611・q612 | a148d504c..80ad2fd54（前回 d108d0a0d） | net・networkd・sessiond/session.c・plan/ws005 | integrated 3410cab0f |
| q616 / q616-i01 | ws127-p002 | Files の改善 | user の採否 | 4h | in-progress |
| P1-011 | q616 | b90364e74..4251edc86（前回 80ad2fd54） | files・libkeiui（scroll-bar）・keiui.h（KUI_VERSION 12）・plan/tools/files・plan/ws127・BUG-140/141/142 | integrated 89ec51678 |
| q617 / q617-i01 | ws089-p010 | Settings の回帰と棚卸し | user | 3h | in-progress |
| P1-012 | q617 | e553fa5dd（前回 4251edc86） | plan/ws089・BUG-146 | integrated 715c15750 |
| q619 / q619-i01 | ws089-p012＋C1＋C4 | Settings の改善 | user | 3.5h | in-progress |
| P1-013 | q619 | 589f3cbca（前回 e553fa5dd） | settings・plan/ws089 | integrated ae604faae |
| q621 / q621-i01 | ws128-p002・p003 | Notes の Open・Save As、Text Editor の Find & Replace・Open Recent | 自走 | 4h | in-progress |
| P1-014 | q621 | 5f58fe7d3・64137d607（前回 589f3cbca） | textedit・notes・plan/tools/textedit・plan/ws128 | integrated d25eedcae |
| q623 / q623-i01 | ws128-p001＋F-062 | 他の標準アプリの棚卸し | 自走 | 3h | in-progress |
| P1-015 | q623 | 0a5e8ae42..e2e051cdb（前回 64137d607） | plan/ws079/tests/demo-s8-s9.sh・plan/ws128 | integrated 619490afe |
| q624 / q624-i01 | ws073-p045 | BUG-135 | user の優先順位 | 4h | in-progress |
| P1-016 | q624・q626 | 39afe4c1c・51af2d726・d82e16c17（前回 e2e051cdb） | src/drivers/fs/ufs.c・plan/ws073・BUG-135・plan/ws005/phase020 | integrated c71a7cb98 |

2026-10-03 / q631-i01（generation6）: host の memory の圧迫でラップアップ、uncleared。agent/p1 51b798274..9eb337f03 未統合。q631-i02（generation7）: user「N=2に上げて、P1も再開します。」、ws005/phase020 の q631 の再開点、in-progress。Next の BUG-052 は Q2 に移管済み（予約から外す）。

| P1-merge q631 | q631-i01・i02 | 51b798274..ba5e3013f（base c0ca07df4、merge ae55fdb46） | src/kern/net/wifi/{wlan.c,wlan-frame.c}・userland/base/net・networkd/main.c・settings/{network.c,page-network.c,settings.h}・plan/ws005 | integrated 01f4c5e05（P1 の全 image build warning 0・boot-test PASS・store/model host 試験 PASS。main との差は plan だけ） |

Next（予約、2026-10-03 user）: q635 BUG-149（kernel の poll の SHUT_WR の POLLERR）。開始条件: q631 の ws005-p020・p024 が cleared。

2026-10-03 / P1 の後: user「P1でBUG-149をクリアした後、ほかに関連バグが見つかっていなければ、いったんラップアップして、P3をN=1のシリアル区間で実行しましょう」→ q635 が cleared かつ関連の新しい bug が無ければ P1 を通常のラップアップで終了、以後 N=1（P3 だけ）。関連の bug が見つかったら Q1 が user に報告して判断を仰ぐ。
| P1-merge ws113-p015 | — | 43d35865a（kl_scroll）・9d04591db・583caebf8 | wayland/{apps-bar,arrange-shell,heads,plane,shell,...}・libkeiland scroll・plan/ws113 | integrated 79b9b1b83（2026-10-08、T1-380 待ち） |
