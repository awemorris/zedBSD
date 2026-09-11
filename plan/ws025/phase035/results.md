# p035 調査記録

Status: completed。q124 の二台構成・単独回帰・host/build gate 完了。

## 接続確認と既知の待機経路

追加機器は USB 3-3 の 2357:0138、bcdDevice=0210、480Mbps、host driver none。
現行 usb-rtl8822bu の Plus product/release match に一致する。
通常 amd64 image に RTL8822B firmware とライセンスの組込みも確認済み。
SSH 用の r8152 / 0bda:8156 は USB 4-3、対象とは別。

collect_profile_radios は全 ready radio の list を巡回し、全て terminal になるか
30秒の選択期限まで待ってから candidates を作る。
select_manual_radio も、先に列挙された radio の terminal を待つ。
同じ時点で利用可能な候補間の決定的順序と、未完了 radio の待機を分離する必要がある。
実機測定前なので、これだけをユーザーの全症状の原因と断定しない。

fixture 準備: run-intel-ax211-vfio-qemu.sh の optional USB companion と
run-p035-dual-wifi.py。USB topology/VID/PID、SSH 経路独立を検証し、
終了時の USB 使用者なしと元の unbound 状態も確認する。

最初の fixture は topology の bus番号とport番号が同じ3-3を誤って拒否した。VFIO変更前の失敗であり、iwlwifi・空override・SSH経路・VFIO未使用を別途確認した。入力チェックを修正して baseline2 を開始。この失敗をOS不具合や合格結果に数えない。

## Dual-radio baseline / host correction

通常p034 imageでUSB= wlan0、AX211=wlan1を同時公開。enable→set-key autoで、
action開始9.307秒、RTL scan COMPLETE観測30.916秒、OFFER49.663秒、
L2+IPv4観測49.865秒、ping 3/3完了56.565秒。
操作開始→IPv4は40.558秒、ping完了47.258秒。遅い接続を再現したが接続自体は成立した。
計測は外部controllerの観測時刻であり、RF処理だけの時間ではない。
`temp/p035-baseline2-enable-first/`にredacted console/events/runnerを保存。
USB解放・iwlwifi復元・元image不変を確認した。

networkdは同一sweepで利用可能な候補があれば選択し、未完了radioのterminalを待たない。
manual選択の先行radio待ちも除去した。先に失敗した候補はskipし後着候補を待てる。
追加stories33–36（遅い先行/後行、manual、早い失敗後の後着成功）は
修正前33で遅延assert FAIL、修正後36全件ordinary PASS。
36初回はfixtureのsuccessful connectionsとattemptsの混同を修正して再実行した。

common dwellはactive100ms/passive200ms、firmware完了イベントを持つAX211は
OFFLOADED_DWELL属性と世代/step一致のchannel-complete latchに変更。
READYの直後でworkerより早い完了、誤世代/step、停止EBUSY時の公開保留、
missing-completionをtimeout failureにするhost testsを追加。
common ordinary/sanitizer/analyzer/amd64+i386 ABI PASS。
既存cache eviction fixtureは1BSS毎に1tick進め64ticks費やしていたため、
短いdwell内で最古entryの順序を維持した同一tick複数受信に変更した。
既存passive fixtureは200ms未満でadvanceしないassertを追加した。

## 最終host/build・初回実機

common追加gateに、2チャネルの完了独立性、cancel後/再start後の旧通知拒否、
software dwellへのfirmware完了通知拒否も追加。通常/sanitizer/analyzer/ABI PASS。
AX211 PCI通常/sanitizer/analyzer/両ABI syntax、Wi-Fi36 sanitizer、child parser、
managed-wlan/selection fixture PASS。通常amd64 make -j16 PASS。
image SHA256 `796702a49c5aeec7457bbb73899c2470f9b83927afaf2998e282cf79cd752925`。
source/image manifestは `temp/p035-built-sources.json`。

`temp/p035-final-enable-first/`：action開始9.283秒→OFFER/bound観測15.807秒（6.524秒）。
IPv4+connected観測18.436秒（9.152秒）、ping 3/3完了25.136秒（15.853秒）。
修正前と同じcontroller観測でOFFER約40秒→約6.5秒、ping約47秒→約16秒。
repeated enable、disable-enable、短いenable-disable-enableもL2/DHCP/ping PASS。
各接続後のloserはscan=cancelled・未authorized・IPv4なし、DHCP子は増殖なし。
全体92.573秒でUSB解放/iwlwifi復元/元image不変。
この初回fixtureは短いenable-disable間にscanを直接観測していないため、
途中cancelの厳密な証拠は次のkey-first fixtureでstatus観測を追加して得る。

`temp/p035-final-key-first/`もPASS。今回はAX211=wlan1が初回winnerとなり、
操作開始→connected+IPv4観測18.962秒、ping3/3まで22.990秒。
異なるwinnerでもloserのscan停止・未authorized・IPv4なし・DHCP子増殖なしを確認。
再enableで新しいOFFERを発生させず既存leaseを保持した。
同一guest command内でenable→両radio status→disableを実行し、
scan=scanningを実際に観測したうえでcancelし、次のenableでL2/DHCP/pingを回復した。
初回2つの実機gateでUSBとAX211の両winnerを実測した。

進捗レビュー：p035の主要な二台構成gateは完了。範囲を拡張せず、
AX211単独12操作のp034回帰とPCAT/PC98 buildを残りの有限gateとして進める。
時刻は外部controllerによる観測、電波環境は今回の2.4GHz実験AP。
5GHz、複数AP roaming、Windows/Linuxとの同条件ベンチマークはこの結果から主張しない。

## AX211単独回帰

`temp/p035-ax211-regression/`：seed882211 / 12操作を159.343秒で完走。
途中operation2で同じfatal firmware interrupt、deferred global stop、rollback failureを
再現したが各1行にとどまり、後続up/scan/connectへ復帰した。
最終down後20秒間の無操作観測は診断0行、stop-pendingなし。
その後net wifi set-key auto→enableからL2/DHCP/ping3/3へ14.354秒で復帰。
USB companionはこの試験には渡していない。iwlwifi復元・元image不変を確認した。

残存事項：AX211 SESSION_PROTECT関連のfirmware内部assertそのものは解決したと扱わない。
p034/p035は停止失敗後の無限診断・復帰不能を直し、今回も実際の同じエラーからの復帰を検証した。

## 完了

PCAT・PC98の通常 make -j16（既存config/ciを明示）もPASS。
amd64通常成果物、common/PCI/36 stories/selection/child host gates、
二台の両順序・反復・実scan取消し、AX211単独反復の全受け入れ条件を満たした。
ホスト検証ログは `temp/p035-host-logs/`、実機は上記各tempディレクトリに保存。
共通層のAPI追加はkernel内部だけで、user ABIとlistのnonblocking契約を維持した。
commit、aggregate make check、.internal読み取りは行っていない。
