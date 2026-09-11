<!-- awesome-plan project=zedbsd record=ws025-p034 -->

既存計画の取り込み。本文はローカルの現行記録、リポジトリ資料へのリンクは基準コミットの履歴です。Issue作成・open状態は実行承認や未完了判定を意味しません。Awesome Planの状態同期導入前のため、実際の状態は本文を参照してください。

元資料: `plan/ws025/phase034/phase.md`

親: [ws025](https://github.com/awemorris/zedBSD/issues/26)

# ws025-p034: AX211 直接操作反復後のエラー出力と復旧

日付: 2026-09-09
Phase ID: `ws025-p034`
Status: completed; q124。修正・host gate・実 AX211 VFIO 受け入れ完了。詳細は [results.md](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws025-io-memory-cache/phase034-ax211-operation-recovery/results.md)。
Parent: [WS025](https://github.com/awemorris/zedBSD/issues/26)

## 目的と承認

ユーザーが報告した、直接の `wifi wlan0 up/down/connect` をランダムに
繰り返した後の無限コンソールエラーを再現し、原因を修正する。
具体的なエラー文は手元にないとの回答。発生箇所を先に断定しない。
最新の再現・修正依頼を q124 への追加実行承認として扱う。
90 active minutes ごとの証拠・進捗レビューを継続する。

## 手順

1. AX211 の poll、割込み、接続・停止・復旧の所有権、世代、join を静的に確認する。
2. p033 と同じ独立 SSH 経路の実機 AX211 VFIO、使い捨てイメージを使う。
   自動管理を disable し、直接操作を固定 seed・有限回数で反復する。
   完了待ち操作と、接続途中への down/up の割込みを両方含める。
3. コマンド順、時刻、最初のエラー、操作終了後のログ増加と状態を記録する。
   無限継続は試験全体を無期限にせず、無操作の観測窓で判定する。
4. 反復を生む lifecycle/所有権の原因を直し、必要ならエラー状態の通知単位も整理する。
   ログを隠すだけの修正では完了としない。DMA や既存操作が生存している間の
   強制解放、成功の偽装、無期限 join は導入しない。
5. 原因に対応した focused regression、通常 amd64 ビルド、同じ seed の実機再試験、
   別 seed と net wifi 自動接続の回帰確認を順に行う。

## 受け入れと証拠

- 修正前の再現コマンド列と反復エラー、または再現できた範囲を区別して残す。
- 同じ操作列の後に down が有限時間で終了し、無操作で同一失敗が出力され続けない。
- up/connect または net wifi enable で、再起動なしに接続・DHCP・LAN 疎通が復旧する。
- 初回の診断情報と失敗結果は保持する。操作中の正常な状態通知は異常と数えない。
- 関連 host regression と通常 build が合格し、VFIO 終了後の iwlwifi・経路・所有解放を確認する。
- 実機で未再現なら修正完了と推定せず、条件と残件を results.md に記録する。

coding-style.md を適用。ビルド・テスト・runtime・本体ソース編集は直列。
commit、aggregate make check、.internal 参照なし。SSID/鍵は stdin で渡し、計画や公開ログへ保存しない。
再利用 fixture は WS tests、使い捨て証拠は temp/p034-* に置く。
