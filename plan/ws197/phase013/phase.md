# ws197-p013: Bluetooth無応答の診断と復旧操作

Parent: [../ws.md](../ws.md)
Status: cleared
Disposition: normal
Queue: [bluetooth-recovery-20261010](../codex-queue.md)
Approval: 2026-10-10ユーザー「Bluetoothサブシステムが応答してないっぽい」「bluetoothdが固まってるならデーモン再起動の方法」「デバイスが固まってるなら、デバイスを開き直したりリセットしたりする方法が必要」。以前のSSH/実機更新/main統合承認を保持。
Scope / Criteria: 実機で無応答を切り分け、既存service restartを確認。bt show等に有限応答待ち、bt checkでHCI応答確認、bt reopenでnodeを閉じ再初期化、bt resetで既存BT_IOC_RESET後に再初期化。保存bondとpower設定を保持。HCI待ちをdeadline内に終え、nonblocking node readとpollのevent判定を補う。短いhost/build warning0/変更source全文規約/main統合まで。
Prerequisites: main 1b07745e4、WS143既存controller/privsep/USB reset UAPI、WS197 p005/p012出力。
Standards: [C全文](../../coding-style.md)、[Guardrail](../../guardrail.md)、[automation](../../standards/automation.md)。HAL/UAPI/desktop protocol/toolchain/shared boards変更なし。
Design: /sbin/service restart bluetoothdはdaemon無応答用（既存実装）。応答するdaemonはCHECK/REOPEN/RESETのCLI requestへ対応、既存D8 permissionを適用。CHECKはHCI Read Local Version Information、stateは変更しない。REOPEN/RESETは現在のconnections/scan/pairを終了し、failure stopを解除して一度再初期化。RESET成功時だけ対象controllerのfirmware-load記憶を解除し、bootloaderへ戻った場合の再loadを許可。CLIはnonblocking socketとmonotonic deadline付きpoll/buffered readで応答待ちを終了（show/check5秒、reopen/reset60秒、既存一般操作30秒/scan指定秒+15秒、pair/agentの人の応答待ちは従来どおり）。追加指示によりroot親の監視を含める。main loopだけがALIVEを送信、親は通常15秒/初期化90秒で停止を検知、子へTERM・3秒後KILLで回収しexit1。initの既存restart=on-failureが再起動する。無入力pollは最大1秒。compositor/sessiondへの新root RPCは不要。
Verification bounds: 短いprivate host probeとnamed bt/bluetoothd build、実機では有限commandの応答/再open/resetを各1回。停止注入は初回でnative CLI alarm方式の不備が判明したため、修正後に追加1回（合計2回）。QEMU/網羅回帰/負荷試験なし。停止の具体的stackは取得できておらず、wait補完だけで根因修正済みとはしない。pairing情報の削除/再pairは実行しない。

## 実機の初期診断

親7/子9のprocessとservice runningが残る一方、bt showは6秒timeout。旧子はPBAP SDP query以後log停止。既存sudo service restart bluetoothdはexit0、親398/子399へ置換、bt show STATE ready/exit0を確認。スマホ再接続はauthentication status0x05で終了しており、daemon/HCI応答とは別条件。個人のaddress/本文/鍵は記録しない。

## 追加指示と設計revision

ユーザー「スマホ側のエラーで接続が削除されました。今はそれでいい」「bluetoothdの応答がないときに、sudo service restart bluetoothdが実行される仕組みがほしい」「sessiondにリクエストしますか？」。スマホ認証/再pairは今回扱わない。既存root親が子の進捗を監視して異常終了、initの既存failure restartへ接続する設計を提案して実装する。限定新scopeはprivsep進捗/監視、正常終了と異常終了の区別、実機で1回の停止注入→自動再起動。init既定はfailure restart上限5回であり変更しない。停止根因は未確定のまま保持。


## 検証・結果 / 2026-10-10

[検証記録](../tests/bluetooth-recovery-verification-20261010.md)。短いhost probe: silent CLI timeout/fresh CHECK、HCI continuous unrelated packetsのdeadlineとclosedfd/HUP、停止子のTERM/KILL/reap→parent exit1はPASS。全文規約/manual review・style-check6source total0・diff-check clean。最終named bt/bluetoothd build warning0/error0。

実機の初回停止注入は親501/子502→watchdog log→親523/子524、HCI ready。CLIのalarm式はhostではPASSだがnativeでは12秒のSSH timeoutとなった。未達を保持し、socketをnonblocking、読み取りをdeadline付きpollとbufferへ変更。実機追加停止注入でbt showは5.11秒/exit2、13:36:13停止→13:36:28 watchdog→13:36:33 ready（親583/子584）。再起動完了前に先行したreopen/check/resetはsocket断/接続拒否で、device操作の成功には数えない。完了後のreopen・resetは各exit0、各直後のCHECKもexit0。

スマホ側の接続削除はユーザー了承済みの現状として保持。bond/power設定を削除せず、desktop/Phone/sessiondを再起動せず実機bt/bluetoothdを更新。現象の元のblocking stack/根因は未確定。自動再起動は既存initの最大5failure restart制限を保つ。WS全体p009/HFP/PBAP/MMS送信は本scope外。共有Board/cache/GitHub投影はQ1。


## Clearance / main read-back

bt-recovery-i01 cleared。source/検証記録7fa0a0e9cをmainへfast-forwardし、HEADのread-back・clean treeを確認。限定criteria（CLI deadline/ fresh CHECK/REOPEN/RESET/親監視/host/全文規約/build/実機/main）は満たす。WS197全体はincomplete、shared Board/cache/GitHubの投影とIssue closeはQ1へ引き継ぎ。元hangの具体的根因が分かる新証拠は別の調査scopeで扱う。
