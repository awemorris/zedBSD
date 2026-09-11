# ws025-p035: 複数無線インタフェースの自動接続と選択遅延

日付: 2026-09-09
Phase ID: `ws025-p035`
Status: completed; q124。詳細は [実行結果](results.md)。
Parent: [WS025](../ws.md)
依存: p034 の実機反復試験を完走してから実行する。

## 目的・環境

最新ユーザー依頼により、AX211 と追加 USB WLAN を同時に QEMU へ渡し、
wlan0/wlan1 がある状況の net wifi enable と、約1分待つ接続遅延を再現・修正する。
追加機器は 2357:0138 TP-Link Archer T3U Plus（RTL8822BU）、USB topology 3-3、480Mbps。
SSH 用 0bda:8156 / topology 4-3 / enx6c1ff71a08b6 とは別物である。
現在 USB WLAN のホスト driver は none。AX211 は既存の singleton VFIO group を用いる。
デバイス番号の固定ではなく topology と VID/PID を検証し、対象以外を渡さない。
90 active minutes ごとの証拠・進捗レビューを継続する。

## 手順と設計方針

1. 既存 VFIO runner に対象 USB WLAN だけの optional usb-host 接続を追加し、
   終了時の USB 解放、AX211/iwlwifi、独立 SSH 経路を確認する。
2. 通常 image の両無線を識別し、profile 未設定の enable→set-key auto と、
   set-key auto→enable、repeated enable、disable→enable を再現する。
3. 両者の up、scan start、候補を含む COMPLETE、connect admission、L2、DHCP、ping を
   分けて計測する。wlan0 が AX211 と決めつけず、driver/device identity を記録する。
4. collect_profile_radios が全 radio terminal または30秒期限まで候補選択を遅延させる
   経路を検証する。既に完了した観測に有効な候補があれば速やかに試行し、
   未完了 radio を単なる列挙順のために待たない。
   同時に観測した候補は既存の profile/radio 順で決定的に選ぶ。
   未完了候補は将来の再試行で利用できるようにし、早い radio の失敗が遅い radio を
   恒久的に隠さない。明示 SSID/credential/owner と1本の managed L2/L3 契約は維持する。
5. full-band scan 自体の時間と管理側の無駄な待ちを区別する。既存の安全な channel/profile
   境界で改善できる範囲を検討し、未検証の regulatory 条件変更や偽の COMPLETE は行わない。
6. 遅い/失敗/候補なし radio と早い有効候補、候補失敗後の他 radio、順番入替えを
   host stories に追加し、通常・sanitizerと関連 parser/selection tests を通す。
7. make -j16 後、同じ実機コマンド列で改善を測定する。単独 AX211 の p033/p034 回帰も確認する。

## 受け入れ

- 両インタフェースが存在する状態で、enable/鍵登録の両順序から追加操作なしにL2/DHCP/pingが成立。
- 片方の遅延・失敗で他方の利用可能な候補を30秒期限まで待たせない。
- 接続途中・scan途中の disable、repeated enable、再 enable で owner/子/route が増殖しない。
- 勝者でない scan は停止できる。スキャン中の list は既存どおり nonblocking でよい。
- 冷起動と再接続の時間、最初の候補から接続試行までの遅延を修正前後で記録する。
  Windows/Linux 並みと測定なしに主張しない。full scan の残る実測限界は明示する。
- 通常成果物、host tests、実 AX211+RTL8822BU 試験、終了時ホスト復元の証拠を残す。

coding-style.md を適用。build/test/runtime/本体編集は直列。commit、aggregate make check、
.internal 参照なし。SSID/鍵を計画へ保存せず、公開結果は redact する。

## スキャン滞在時間の追加設計（baseline 後に確定）

共通層の dwell が全チャネル100ticks（100Hzで1秒）固定で、RTL8822BUの15チャネルと
AX211の各 firmware scan に不要な待ちが生じることを確認した。
ソフトウェアscanは既存のACTIVE_ALLOWED判定を維持し、active100ms/passive200msを初期値として
実機検証する。channel tuneの完了後から計り、単なるdeadline短縮で途中のBSSを捨てない。

firmware scan はchannel completionを既に得ているため、内部profileにoffloaded-dwell属性を加え、
common generation/stepに一致する完了通知を新しいlatch APIで渡す。
共通層は完了通知で次チャネルへ進み、1秒は異常時の上限として残す。
通知前にタイマーだけで完了扱いにはしない。誤世代・誤step・遅着・missing completion・
cancelとfinal producer stopをhost testで検証する。
WLANのuser ABI/listのnonblocking/COMPLETEの公開barrierは変えない。
AX211のfirmware内passive dwell（既存110TU）とregulatory channel集合も変えない。
