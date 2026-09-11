# ws025-p033 作業記録

Status: completed。2026-09-09、q124。以下は修正・検証の経過。

## 初期実機観測

10.0.10.25 の 8086:51f0 / 8086:4090 rev 01 を、singleton IOMMU group 11
から QEMU q35/KVM へ VFIO で割当てた。SSH は独立した USB Ethernet。
標準 amd64 UEFI/xHCI image の disposable copy、4 CPU/1 GiB、仮想 NIC なし。

初回の enable → set-key auto は、追加 enable や手動 dhcpc なしで
WPA2 認証と IPv4 lease 取得、ルーターへの ping 3/3 に成功した。
接続中 enable の反復は接続を維持した。disable → enable も、途中操作を止めて
観測すると自動で回復し、再び ping 3/3 に成功した。
従って、この時点ではユーザー報告の恒久的な接続不能を再現したとは言えない。
AX211 の「association hardware ready」は認証・DHCP 完了の保証ではない。

証拠: ../temp/p033-baseline/console-redacted.log、observations.log。
初回の手動観測はコマンド時刻が不十分だったため、再利用可能な
../tests/run-p033-ax211-session.py で時刻付きの再検証を行う。
ホストは終了時 iwlwifi、空 driver_override、独立 SSH route、VFIO holder なしを確認した。
秘密を含む raw console はリモートの root 専用 directory 内、保存する結果は redact する。

## 確認できた不整合と修正

従来の dhcpc は OFFER や lease を printf していたが、networkd の child runner は
stdout/stderr を一時ファイルへ捕捉し、管理 DHCP 経路はその内容を消去していた。
これにより DHCP が実際には成功していても、コンソールでは進捗が見えなかった。

ユーザーの追加指示に従い、dhcpc の運用メッセージは /dev/console への直接 write
へ変更した（usage は通常のコマンド出力）。printf との重複出力も不要な opt-in flag もない。
O_WRONLY|O_NONBLOCK|O_NOCTTY|O_CLOEXEC で開き、一度だけ bounded write して閉じる。
出力失敗は DHCP の成功・失敗や errno を変えない。

- offered: 検証済みの最初の OFFER を選択した時点で一度。interface/address/server を表示。
- bound: ACK の後、address/route/resolver の反映まで成功してから表示。
- timeout/rollback: 失敗 stage を同じコンソールへ表示。
- networkd: L2 認証が完了して DHCP を起動する地点を明示。

以下の遅延境界も実機観測と追加 fixture で確認し、networkd/wifi を修正した。AX211 の追加修正は p034 に分けて記録する。

## 検証

- userland network recovery fixture: 通常・sanitizer・compiler analyzer PASS。
  OFFER 重複、parser reject、不正 source port、ACK timeout、resolver commit failure、
  console open failure を追加して確認した。parser の reject は fixture injection である。
- Wi-Fi30: 通常 30/30、ASan/UBSan 30/30 PASS。
- 通常 amd64 make -j16 build PASS。
- 更新通常 image の AX211 受け入れ: 実行中。最終結果は追記する。

受け入れ未完了のため p033 は completed にしない。

## 追加で確認した選択境界

更新 image の実機ログでは、初回のキー後設定から接続まで約80秒、
後の disable/enable では約160秒かかる試行があった。
スキャン中の各 poll の deadline が更新され、接続前に次世代の scan へ進むことも観測した。

静的に、prepare_wlan_radio は RUNNING 以外なら必ず search-start する。
従って collect_profile_radios の30秒待ちが終了した後、次の再試行までの間に
COMPLETE になったスナップショットを読む前に、新しいスキャンへ移れる。
タイミング次第で選択の機会を繰り返し失う。この境界は修正対象とする。

方針: 無条件に待ち時間を伸ばすのではなく、interface/ifindex ごとに最後に
選択で消費した snapshot generation を保持する。未消費の COMPLETE は先に読み、
消費済みなら次の scan を開始する。list parser が検証した generation で消費を記録する。
空の結果や候補なしでも消費して、将来の AP 出現を新しい scan で探せるようにする。
別 device identity には消費状態を継承しない。WIFI1 の wire format は変えない。
追加 story 31/32 で、待ちの終了後に到着する COMPLETE と、空の COMPLETE 後の再検索を検証する。

## スナップショット修正の検証経過

追加 story 31 は修正前に AUTO_SEARCHING のままとなり失敗、修正後に
一度の scan から CONNECTED へ到達して合格した。story 32 は空の snapshot の
消費後に新しい scan を実行し、後から見える AP へ接続することを確認した。
既存30本＋追加2本は通常・ASan/UBSan とも 32/32 PASS。
managed WLAN / radio preparation / selection / Wi-Fi child parser fixture も PASS。

新しい待ち境界の fixture 作成時、daemon の nanosleep が実時間を待つ一方で
fixture clock が進まない不整合を発見した。最初の試行は停止し、daemon と wifi child
の両方が同じ advancing fixture clock を使うよう修正してから、修正前の失敗を確認した。
この fixture 準備時の停止を production の不具合や合格結果に数えない。

実機の先行比較（ping 確認までの観測時間であり、厳密な無線性能 benchmark ではない）:
- logging 修正のみの enable → key → L2/DHCP/ping: 82.994秒。
- snapshot 修正後の同じ順序: 44.727秒。OFFER/bound はキー設定から約38秒で観測。
- 前者は scan generation を進めてから接続、後者は最初の完了 snapshot から接続。

## DHCP 応答停止の受け入れ方法

AP の設定変更は行わない。専用 image にだけ p033-dhcpc-drop fixture を追加し、
使い捨てゲストの /sbin/dhcpc を一時的にこの実行ファイルへ差し替える。
実際の AX211 で受け取った UDP 応答を gate file の存在中だけ消費し、
本体の DHCP parser に渡さない。通常版 dhcpc 本体、networkd、WLAN/AX211 は模擬しない。
実 packet の抑止ログと offer timeout を確認後に gate を除去し、追加 enable や
手動 dhcpc なしに復旧するかを確認する。終了時は通常 dhcpc も復元して再確認する。
これは実 AP の DHCP daemon を停止した試験とは区別して報告する。

## 接続失敗後の再試行の所有者

machine wifi child が admitted connection の失敗後にも自前でフルスキャンを再試行し、
30秒の child deadline に到達してから networkd がさらに再試行していた。
machine モードは admitted attempt の ETIMEDOUT/ECONNRESET を直ちに返し、
networkd に次の試行を一元化した。直接 wifi の再試行と admission 前の再選択は維持する。
追加 fixture は修正前失敗、修正後 通常・sanitizer・analyzer PASS。
最終 Wi-Fi stories は通常・ASan/UBSan とも 32/32 PASS。
実機で ECONNRESET が Unknown error と表示される libc の欠落も補った。

## 実機試験の中間結果（p034 修正前）

- p033-final-enable-first: 318.955秒で全項目 PASS、iwlwifi 復元・source image 不変。
  enable→key、反復 enable、disable→enable、scan cancel、誤キー→正キー、各 L2/DHCP/ping 3/3。
- p033-final-key-first: 102.935秒で PASS、同じ復元確認。独立した新規ゲスト。
- p033-final-association-cancel: 観測時には接続試行が終了済みで fixture 前提不成立。
  接続途中を観測してから取消すよう最大4試行の観測に修正。合格とは数えない。
- p033-final2-dhcp-recovery: 実 UDP 応答の抑止と OFFER timeout、DHCP child 残存0を確認。
  応答再開後、firmware fatal→停止失敗→rollback エラー反復を再現。185秒で診断停止し、
  ホスト復元・元 image 不変を確認。受け入れは失敗、p034 の原因修正を先行する。
- p033-acceptance-2 は machine retry 修正のため途中停止した比較観測。完走 PASS と数えない。

個々の接続時間には無線再試行を含む。固定条件の性能 benchmark とはしない。
通常PCAT/PC98/amd64 buildは machine retry 修正後 PASS。p034を含む更新通常 image で
残りの受け入れと回帰確認を完了するまで p033 は in-progress とする。

## 最終受け入れ

p034 の停止回復と wifi up 再整合を含め、残る実機 gate も完了した。
p034-final-dhcp-recovery は応答抑止 timeout→子残存0→応答再開→自動L2/DHCP/ping、
通常 dhcpc 復元と再 enable の双方が PASS。途中の firmware fatal からも手動介入なしに復旧。
p034-final-association-cancel は実際の接続途中で disable した後の再接続・DHCP/ping PASS。
2 seed の直接操作反復後も自動接続・DHCP/ping PASS。各ゲスト終了後にホスト復元と元image不変。
既存32 storiesは最終wifi up修正後も通常・sanitizer32/32、3アーキテクチャbuild PASS。
以上により p033 completed。続く複数無線と選択待ち遅延は追加依頼の p035 に分ける。
