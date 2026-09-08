# p034 調査記録

Status: completed。q124。

## 実機再現

p033-final2-dhcp-recovery で実 AX211 の DHCP 応答を一時的に抑止した後、
再接続 generation=5 の association step=9（opcode=05 group=03）で firmware fatal。
LMAC id=00000071、UMAC id=20000110。per-resource rollback と global stop が失敗した後、
`association rollback failed generation=5 result=5 phase=6 step=13 failure=13` が
約4,000行以上反復し、ネットワーク復旧も止まった。コンソール入力にも割り込んだ。

証拠: ../temp/p033-final2-dhcp-recovery/console-redacted.log、events.json、runner.log。
185秒でゲストを診断のため停止、iwlwifi 復元・元 image 不変を確認。
DHCP 応答停止→再開の受け入れは失敗扱い。後続 association-cancel は未実行。
ユーザー報告と同じ文字列かは不明だが、無限の反復出力を実機で再現した。

## 原因と修正設計

global stop が失敗すると runtime_active=0、quarantined=1 となり、poll は再試行しない。
common 側は1 tick間隔で disconnect を再試行するが、driver は firmware rollback を
繰り返すだけで、runtime_active=0 のため global stop の再試行へ進まない。
既存の独立 retirement worker へ、この失敗を引き渡す経路が欠けている。

修正は失敗した global stop を stop_pending として既存の独立 worker に引き渡す。
既存の参照 join・common quiesce・checked reset・指数バックオフを再利用し、
その間は common timer が同じ rollback を反復しない。
確認不能な DMA を解放せず、最初の失敗は通知する。
firmware assertion 自体の原因はこの時点では未確定で、復旧欠落と区別する。

## 実装と host gate

- key/association の global stop と fatal poll cleanup が失敗した際、独立 retirement worker へ引き渡す。
  既存 close owner がいる場合は完了通知を横取りしない。
- stop_pending が common timer の admission を止め、独立 worker が参照 join・quiesce・
  checked global stop・common close の再整合を実行する。既存の1/2/4/8/16秒バックオフを利用する。
- 初回の global stop deferred には error/runtime state/IRQ/DMA retention を出す。
  disconnect rollback の診断にも既存の頻度制限を適用するが、停止回復が主修正である。
- PCI fixture: key install failure＋IRQ drain failure で stop_pending が立たない修正前失敗を確認。
  修正後は再試行失敗中の DMA 保持、成功後の解放、再 open、fatal poll 経路も合格。
- PCI fixture の通常・ASan/UBSan・analyzer・amd64/i386 syntax PASS。
- 実 common WLAN の独立 retirement/backoff/status/concurrent cancel/reuse fixture 全 gate PASS。
- DHCP/strerror/network status fixture 通常・sanitizer・analyzer PASS。
- 通常 amd64 make -j16 と専用 DHCP loss fixture image build PASS。

リファクタリング後のテスト接続も修正した。fragment extractor は指定した source のみを
抽出できるようにし、無関係な旧 pc98-auto section の消失で PCI fixture が止まる問題を解消。
common fixture のソースパスを実際の kern/net/wifi へ更新した。
抽出した PCI translation unit は現行本体そのものの該当 section で、別実装ではない。
証拠: ../temp/p034-host-logs/。実機再試験は進行中。

## 実機再試験で確認した第二の不一致

p034-dhcp-recovery でも同じ SESSION_PROTECT/SESSION_REMOVE の firmware fatal を再現。
修正した引渡し経路を実機で通り、global stop deferred は一度、rollback 診断は2行に収束し、
status は down / stop-pending=no になった。無限ログの原因修正はこの実機経路でも確認した。
ただし generic interface は UP のままで、自動復旧は成立せず診断停止した（ホスト復元済み）。

inet-socket の SIOCSIFFLAGS は UP の重複設定で driver open を呼ばない。
独立 retirement は radio を閉じても、generic administrative owner の hold を勝手に
解放しない。この状態で wifi up は ioctl の成功だけを根拠に、radio を開かず成功通知していた。

wifi up を修正し、generic UP と radio down の不一致時に限り administrative down を通し、
停止済みを status で確認してから up を行う。stop_pending 中は EBUSY で所有権を守り、
稼働中の repeated up は再起動しない。up/down とも最後の radio status を確認して成功を返す。
これは明示 up 操作の再整合であり、driver が generic owner の参照を横取りする変更ではない。

fixture は修正前に「generic UP concealed a stopped radio」で失敗、修正後は正常再開、
稼働中 up の維持、stop_pending 中の不介入が通常・sanitizer・analyzer PASS。
この追加修正を含む最終 story/build と実機再試験を継続する。

## 両修正後の実機回復

p034-final-dhcp-recovery は同じ firmware fatal を再現し、独立停止の引渡しと
wifi up による再整合を経て、手動介入なしで L2/DHCP/ping 3/3 に回復した。
応答再開から ping 確認まで136.808秒（scan・無線再試行を含む）。
通常 dhcpc を checksum 検証付きで復元し、disable/enable 後の再取得・ping 3/3 も合格。
全体316.105秒、iwlwifi復元・元image不変。OFFER/boundは各2回。

同じ fatal が起きても無限 rollback 出力にはならず、global stop deferred 1行、
rollback failed 2行で停止が完了した。firmware assertion の発生要因そのものは未修正で、
今回確認したものは異常後の停止・復旧不能の原因修正である。
直接コマンドの固定seed反復と association cancel は継続中。

## 直接操作と association cancel の実機結果（中間）

- p034-final-random-882211: seed=882211、12 operations と準備 scan/up、
  接続の background 実行に対する0/1秒後の downを含む。最初の直接接続は2試行目で成立。
  operation 2 の connect＋down で同じ SESSION_PROTECT fatal を再現したが、
  deferred stop が回復し、以後の up/scan が継続できた。
  最終 down 後の20秒無操作観測で反復診断なし、その後 net wifi の L2/DHCP/ping 3/3 PASS。
  全体438.418秒、iwlwifi復元・元image不変。
- p034-final-association-cancel: 最初の試行は観測前に終了したため次を観測。
  実際の接続途中の status を確認して disable し、正しい鍵・enableで
  L2/DHCP/ping 3/3 PASS。全体148.204秒、ホスト復元・元image不変。
- 第二seed=211882も12 operations完走。20秒の無操作観測で診断0行、最後の自動L2/DHCP/ping 3/3 PASS。全体464.687秒、iwlwifi復元・元image不変。

## 完了判定

2 seed × 12 operations、association cancel、異常を実際に再現する DHCP loss/recovery の
全実機 gate が完了した。通常 PCAT/PC98/amd64 build、Wi-Fi32 通常・sanitizer、
PCI/common/DHCP focused gate が合格。p034 completed。
ファームウェア SESSION_PROTECT の assertion 自体は残件として明示し、
無限出力と停止・復旧不能の修正から区別する。
