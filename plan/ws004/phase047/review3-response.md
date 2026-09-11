# review-3 への回答: 停止未完の進行と観測

追記: この回答後、ユーザーが設計と受け入れ実行を承認。
[P048 の実装・検証結果](../phase048/results.md)に完了を記録した。
以下の「未実装」「停止を維持」は回答時点の記録であり、現在の状態ではない。

日付: 2026-09-06
対象: `/home/awe/claude/zedBSD/plan/review-3.md` と `/home/awe/zedBSD` の現行コード。
今回は対応可否の検討。以下の追加設計は未実装であり、実機での再現確認もしていない。

## 結論

停止未完を次の open だけに委ねる点と、それをユーザーランドから正確に
観測できない点は、残る機能上の問題として認める。前回の修正は所有保持と
再 open の経路を改善したが、停止要求後に自動で後始末を完了させる保証には
達していない。§1.5(a)(b) の方向で対応するのがよい。

§1.4 の静止と解放を分離する考え方自体は有効。ただし「解放しない操作は
実行中の呼出元がいても安全」という前提は成立せず、提示された分割のまま
無条件に実行することには反対する。将来、並行キャンセル専用の契約を作る
余地はあるが、既存 stop 関数をそのまま流用できるという意味ではない。

## 指摘の確認と訂正

1. **自動再試行の欠落は妥当。** 両ドライバの `close_locked()` は
   join/barrier 失敗で `close_pending` を残す。`net_device_schedule_poll()`
   は open_count が非0かつ closing でない場合しか予約しない。
   RTL の `operation_leave()` も opened などを条件にする回復用の予約で、
   遅延 close の実行経路ではない。通常の down 後に、明示 up なしで
   close_pending を回収する仕組みが足りない。detach/shutdown は別の経路。
   根拠: `src/drivers/pci-intel-ax211.c:4087`、
   `src/drivers/usb-rtl8822bu.c:5331`、同 `:703`、
   `src/kern/net/net-device.c:1145`。

2. **「状態が DOWN になる」は正確ではない。** 共通 station が closing の間、
   STATUS も通る `station_find_enter()` は ENODEV を返す。
   ドライバが読み取り ioctl を許していても、その先で拒否される。
   AX211 の最初の pin join で戻る場合は共通 close の前なので、共通側の
   closing が必ず立つという説明も修正が必要。
   `wifi down` が確認するのは汎用の行政状態変更の成否であり、RF 停止の成否
   ではない。networkd は無線列挙時の STATUS エラーを continue で飛ばすため、
   停止未完のデバイスが一覧から消える可能性がある。これは表示文言以上の
   問題である。一方、停止手順は disconnect/search-stop の先行エラーを保持
   するため、networkd が毎回必ず停止成功と誤認するという断定もしない。
   根拠: `src/kern/net/wlan.c:1095`、同 `:3104`、
   `userland/base/wifi/main.c:632`、
   `userland/base/networkd/main.c:3620`、同 `:3747`。

3. **停止処理はメモリ解放以外にも競合する。**
   AX211 の `intel_ax211_mmio_stop()` は nic_lock_depth、prepared、
   reset_done、apm_ready を変更し、MAC access request を解除して software
   reset する。所有状態を触らないレジスタ操作とは言えない。
   RTL の `rtl8822b_radio_stop()` は末尾で radio オブジェクト全体を memset
   するので、提案の「静止」側に既に共有状態の消去が含まれている。
   この消去を移動しても、進行中のレジスタ列と停止のレジスタ列の排他は必要。
   また `rx_submit()` は starts_active を取得した後、ロックを離して
   setup/submit する。一度 cancel しただけでは、まだ submit 前の呼出元の
   後続 submit を静止済みと扱えない。既存の submit 後の再確認・cancel と
   join が停止確認には必要になる。
   根拠: `src/drivers/intel-ax211-mmio.c:309`、
   `src/drivers/rtl8822b.c:3328`、
   `src/drivers/usb-rtl8822bu.c:2012`。

4. **I/O キャンセル、無線停止、鍵の不存在は別の証明。** RX URB の cancel は
   EP0 で実行中の `drv_usb_control()` のキャンセルではない。割り込み mask や
   PCI bus master 無効化だけでも、RF 停止や CAM 消去を証明できない。
   チップの停止/reset 完了と、旧呼出元がその後に状態を書き戻せない保証が
   揃えば、解放より先に「旧ハードウェア世代を無効化済み」と判定する設計は
   可能。しかし `session_stopped` / `hardware_stopped` を単なる mask/cancel
   完了へ弱めてはいけない。join の5秒超過だけから、すべての呼出元が永久に
   ハングした、あるいは強制停止で必ず戻るとも断定できない。
   RTL のレジスタ I/O は USB core のキャンセル/drain にも依存する。
   根拠: `src/drivers/usb-rtl8822bu.c:1014`、同 `:2148`。

## 推奨する追加設計

行政状態の down、ドライバ停止の pending/failed/complete、デバイスの物理的な
不存在を区別する。down の受付時点で新しい通常操作を閉じ、完全停止・共通所有
の清算が終わるまで pending を保持する。

- **デバイスの open 状態に依存しない停止処理の実行経路。** close 失敗時に予約し、
  最後の lease 返却などで起こす。lease が0でも共通 barrier 取得や実際の停止が
  失敗する場合があるため、leave の通知だけでなく、期限付き再試行と backoff
  も必要。永続的な故障は停止未完として保持し、「数秒で必ず回収」とはしない。
- **仕事そのものの寿命と排他。** driver/device 参照、予約の重複抑制、open との
  直列化、detach/shutdown 時の予約停止と join を設計する。既存 net core は
  close を非同期 producer の退去境界として記述しているので、参照保持を含めて
  この契約と整合させる。IRQ/USB completion や最後の leave の中で、同期 close
  を直接呼ぶ実装にはしない。汎用 workqueue が既に使える前提でも進めない。
- **停止中も取得できる読み取り専用 snapshot。** 共通状態の値だけを
  DISCONNECTING に変えても、入口の ENODEV は解消しない。オブジェクトの寿命を
  保護した観測経路と、ドライバの停止結果を共通側へ反映する契約が必要。
  DISCONNECTING は通常の切断にも使うため、停止未完を明示する状態/フラグの
  方が意味を分離しやすい。公開 ABI の表現と互換性は実装時に確定させる。
- **wifi/networkd の停止完了条件。** 行政状態だけで完了にせず snapshot を照合。
  既知の無線の観測失敗を「無線ではない／既に停止した」に変換しない。
  保持する対象は名前だけでなくデバイスの同一性を確認し、抜去と名前再利用を
  区別する。net wifi は未完了を RETIRING として保持し、利用者にも示す。

これは `operation_leave()` に予約を1行追加するだけの修正ではないが、
P047 の停止契約と P012 の観測・退去処理を合わせて修正する範囲で対応可能。
完全に故障したデバイスまで「close が戻れば必ず物理停止済み」を保証する
設計にはせず、未完了を隠さず、回収可能になれば自動で進むことを保証する。

## 補足2件と後続の受け入れ

AX211 の通常 detach は `net_device_gone()` → graph detach → session stop の順。
gone は close callback の終了を待つ。station が未接続の部分公開経路も存在するが、
net_open は station_attached を要求する。確認した呼出経路では、station を解除した
後に稼働中セッションの close hook が初めて走る順序は見つからなかった。
NULL を理由に無条件 stop を追加する修正は、今回の指摘からは必要と判断しない。

`wlan_station_quiesce_begin()` が EBUSY の場合も closing を残す契約のコメント
追記には賛成。実装を進める際に、停止要求を維持する意図を明記する。

後続の受け入れにはレポートの遅延・可視性確認を加える。ただし、共通 active、
driver lease、submit 前の RX、EP0、URB drain、停止自体の失敗を分ける。
up なしで停止が完了すること、失敗時に pending が残ること、detach/open と
競合しても古い仕事が新世代へ作用しないことを確認する。
AP の接続一覧は stale entry が残り得るため、局が消えるまでの時間だけで
RF 停止の瞬間を判定せず、停止結果・転送状態なども照合する。

今回は資料とソースの静的照合および検討記録のみ。ソース変更、ビルド、実機・
QEMU・受け入れシナリオの実行は行っていない。受け入れ前の停止を維持する。
