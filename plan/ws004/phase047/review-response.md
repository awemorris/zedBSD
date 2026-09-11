# AX211 / RTL8822BU レビューへの対応

日付: 2026-09-06。Q085 / WS004 p047。
入力は `/home/awe/claude/zedBSD/plan/ax211-report-1.md` と
`/home/awe/claude/zedBSD/plan/rtl8822bu-report-1.md`。両ファイルは変更していない。
判定対象は `/home/awe/zedBSD` の現行ソース。セキュリティ監査ではなく、
停止・再開・スキャン・後片付けの機能整合を対象とする。

## 共通の修正方針

ドライバだけ停止して共通WLAN側のキー/association所有が残ると、次のopenでも
古いinverseが失敗し続ける。逆に、共通側の処理がまだ実行中なのにドライバを
リセットするのも不正。そこで、共通closeの試行、共通呼出元の静止確認、
ドライバの停止確認、共通closeの再実行、という順序を両ドライバに設けた。

内部API `wlan_station_quiesce_begin/end()` は、既存のclosing/lifecycle/
active/controlゲートを使い、ハードウェア停止の間だけ新規共通処理と
別のclose/detach/shutdownを止める。beginがEBUSYならresetせず、終了していない
呼出元・URB・DMAの所有を保持する。endはclosingを解除せず、その後の共通closeが
キー削除、association解除、radio停止を順に確認して解除する。
公開ユーザーABI、Wi-Fiコマンド、ファームウェアは変更しない。

`session_stopped` / `hardware_stopped` は完全な停止を確認した場合だけ立つ。
次のハードウェア取得前に消す。その間だけ、全体停止によって存在しなくなった
キーなどのinverseを成功とする。稼働中の別世代に対するESTALEは保持する。
close未完了は `close_pending` に残し、次のopenが停止・共通closeを再試行して
から新しいセッションを開始する。単にエラーを成功へ読み替える修正ではない。

## AX211

| # | 判定と対応 |
| --- | --- |
| 1a / 1b / 1d | 正常に停止できた場合まで永久quarantineにしていた点は妥当。recovery、scan異常、key/association異常の停止結果が0ならquarantineしない。失敗した接続そのもののエラーは返す。scan_stopでは、スキャン失敗と停止成功を分け、全体停止を確認した場合は退去成功とする。停止後は明示down/upで再開できる構造とし、キーの残った共通状態も再照合する。 |
| 1c | 世代不一致だけで現行セッションを止める点は妥当。disconnectとassociation_clearのESTALEはそのまま返し、全体停止へ昇格させない。ただしESTALEを「何も残っていない」とは解釈しない。別の接続が存在する可能性がある。 |
| 2 | common closeの失敗だけで停止を飛ばす点は妥当。`ax211_pci_close_locked()` がjoinと共通停止バリアを通った後にchecked stopを実施する。join失敗まで無条件にresetする提案は不採用。静止できない呼出元は保持し、close_pendingから再試行する。 |
| 3 | joinがstation closeの再試行時間を消費する点を修正。両者をそれぞれ有限5秒の窓とし、detach側もclose用期限を再計算。ただし「その絶対期限がキー削除へそのまま渡される」は不正確。共通 `station_wpa_cleanup_deadline()` とdriver rollbackは元々別の期限を作る。無期限化やjoin成功の仮定はしない。 |
| 4 | 繰り返し拒否ログは妥当。ioctl/connect/management TXの拒否を共通の1秒レート制限で抑制。根本の停止/回復結果は記録する。なお「quarantine中にもconnect ioctlを必ず300回再試行」はユーザーランドの返値分岐次第であり、実測値とは扱わない。 |
| 5 | 共通側の未完了closeが次回scan profile更新を阻む問題は妥当。新規boot/profile更新より先に停止後の共通closeを完了させる。共通scan_stopのEBUSYは再試行を保持しつつSCAN_FAILEDにしない。ただし元コードもscan_retry_deadlineを設定しており、「再試行されない」は誤り。abort受付だけで0を返す提案はproducer退去保証に反するため不採用。profile更新の省略も停止不整合を隠すので採用しない。 |
| 6 | DMA/IRQ停止を確認できないときのring保持はリーク修正対象ではなく必要な所有保持。停止失敗時に無条件で解放・初期化する変更は不採用。完全停止時だけリング解放と接続情報消去ができる既存の順序を保持する。 |
| 7 | runtime_activeだけでopen成功にする条件は不十分。admissionが開き、recoveryが未処理/実行中でないことも必要とした。満たさなければEBUSY。 |
| 8 | ENODEVと共通close成功を同一視しない。close経路は共通closeが0になるまでclose_pendingを保持する。共通carrier-downは、参照を保持する削除済みnet_deviceが実際にcarrierを持たないと確認できた場合だけ退去成功とする。キー/scan/radio由来のENODEVは成功に変換しない。 |
| 9 | 現在のdeadlockとの指摘ではなく将来の契約変更への注意として妥当。3か所のscan reportはコピー/ラッチとworker wakeupのみで、同期radio callbackを行わないことをコメントに明記。不要なロック解除や新しいpin経路は追加しない。 |
| 10 | 「未知の通知はすべてfatal」は不採用。`ax211_pci_runtime_event_dispatch()` の未知通知の末尾はreturn 0。association通知もEXPIRED/IGNORED/DUPLICATE/STALEを許容している。長さやepochが壊れたenvelope、既知のプロトコル違反を無条件に捨てる変更はしない。 |

TX completion timeoutは単なる無線ACK不成立と同じではない。送信リングに残る
完了未報告のslotの期限であり、ACK失敗を報告済みの送信とは異なる。
回復契機を削除せず、正常停止後に永久に利用不能になる点を修正した。
今回AX211のfirmware自動再bootは追加していない。停止したepochの再開経路は
明示down/upであり、すべての障害からnet wifi単独で自動復帰するとの主張はしない。
既存net_deviceのclose hookはvoidのままなので、downの終了コードだけで
ハードウェア停止成功を判定することもできない。未完了状態は保持し、診断と
次のopenの返値で確認する。close hook全体のABI変更は今回行っていない。

## RTL8822BU

| # | 判定と対応 |
| --- | --- |
| 1 | common closeの失敗でハードウェア停止を省略する点は妥当。common barrierを得た後は、inverseの先行エラーにかかわらずchecked stopを試行。停止確認後に共通closeを再実行。停止失敗や共通呼出元の未終了は保持し、次のopenで再試行する。 |
| 2 | 固定1秒のretry窓は短い。4個のキーinverse、association clear、disconnect、500msのTX report退去、1秒の余裕を含む式に変更。現在の定数では7.5秒。これは再試行窓であり、単一同期callbackをプリエンプトする期限ではない。各callback自身の有限期限は保持する。 |
| 3 | producer joinが無期限である点は妥当。`rtl8822bu_wait_activity()` を5秒の有限joinに変更してエラーを返す。join失敗時はURBをcancel/drain/releaseせず、radioも実行中の呼出元の下ではresetしない。後続open/closeで再試行する。 |
| 4 | rx_stopがclosingを残す点は妥当。同期stopの終了時にはclosingを解除し、失敗はquarantineで表現する。ただしradio resetが成功してもURB drain失敗を打ち消さない。両方の成功前に再利用しない。次のopenは保持した停止を先に再試行する。 |
| 5 | カウンタの2用途だけを理由に排他を解除する提案は不採用。これはproducerのjoin対象であると同時に、最後のTXの終了からCAM/BSSID変更までを保護する。変更側がtx_quiescingを閉じて既存TXの退去を待つ順序を保持する。別フラグだけを導入してdata TXとの競合を許すと、旧キー/旧チャネルの送信が割り込む。 |
| 6 | 「ARP/DHCP/EAPOLが普通のscan各stepに衝突する」という因果は現行条件と一致しない。scan_channel_startはconnection_generationが0のときだけ許可され、通常のdata/connection management送信は有効な非0の接続を必要とする。scan probeとchannel変更の排他も意図的。通常の接続中background scanを本変更で追加しない。 |
| 7 | ESTALEをENODEVとまとめて成功にする提案は不採用。別の現行世代が存在し得るため。closeは実際にradio/host停止を確認してから共通所有を退去する。停止確認フラグがある場合だけ旧inverseの不存在を証明できる。 |
| 8 | EP0エラーの大量ログは妥当。register transferとprocessing-delayの失敗表示を同じ1秒レート制限にした。元のエラー返却とrecoveryのエラー計数は変更しない。 |
| 9 | 各チャネルで無限に同じ失敗を表示するという説明は過大。set_channel失敗は共通scanを失敗させ、radio OFFならdriver側もquarantine/recoveryへ移るため、そのまま全チャネルを進み続けない。このstage診断は残す。低層registerの反復表示は#8で抑える。 |
| 10 | 現在のscan_stopには非同期firmware scan producerの停止操作がなく、該当するscan観測世代を解除する。将来の操作のために架空の失敗を追加しない。新しい非同期producerを実装する際に必要な停止契約は既存headerに明記されている。 |

## 確認と受け入れ保留

実装はadapter 2ファイル、共通 `src/kern/net/wlan.c` と内部header
`include/kern/net/wlan.h` に限定。RFテーブル、USB端点設定、firmware pinは不変。
構成別ビルド結果は `phase.md` に記録する。実機・QEMU・30シナリオ・新しい
driver単体fixtureは、この受け入れ前チェックポイントでは実行していない。
既存fixtureに新しい内部barrierを反映する作業も受け入れ準備として残る。

後続の受け入れで最低限確認する内容:

1. 各adapterでidle/scan/handshake/data通信中のdown/upを反復。
2. 共通active caller、driver lease、URB drainそれぞれの遅延・失敗を区別し、
   未確認のreset/releaseがないことと、その後の最初のopenで再試行すること。
3. full stop成功後の残存キー/association inverse、scan abortのEBUSY、
   真のESTALE、removed-device carrierの区別。
4. core barrier中のioctl/worker/close/detach/shutdownの排他と解除。
5. AX211完了通知欠落から停止/down/up再開、停止失敗時のDMA保持、IRQ数。
6. RTL8822BU未完了RX drain後のquarantine保持、物理抜去、再接続、ログ上限。
7. Q085 P012の30シナリオと実機のnet wifi状態・L2/L3の整合。

ビルド成功を実機安定性の確認とは扱わず、p047とp012はin-progressのまま保持する。
