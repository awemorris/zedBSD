<!-- awesome-plan project=zedbsd record=ws197-p010 -->

# ws197-p010: MMSの写真・動画の受信と送信

Phase ID: ws197-p010
Parent: [WS197](../ws.md)
Status: uncleared
Phase disposition: normal
Queue: [Codex承認済み実行記録](../codex-queue.md)、2026-10-10ユーザーの4機能実装指示。
Branch: codex/ws197-mms-media、base main 9dfebc99b。
Prerequisites: 既存MAP Get/Push・MMS text reader・PBAP/Phone保存、mainの実sourceを確認済み。
Standards: [Guardrail](../../guardrail.md)、[C全文](../../coding-style.md)、[automation](../../standards/automation.md)。例外なし。master/共有Queue/他P tree/toolchainは書かない。自分のnamed buildだけ行い、QEMU/全make checkを追加しない。

## 承認・範囲

ユーザー「写真の受信、動画の受信、写真の送信、動画の送信も一緒に実装してしまいましょう。」を4機能全体の承認として記録。p004の旧MMS添付除外をこの範囲で変更する。p005の完了条件は変更しない。HFPは変更しない。実機からの実際の送信はユーザーが操作する。

## 設計・手順

1. OSに依存しない独自Zlib MIME codecを `userland/base/libmms/` に置く。既存bluetoothdのMMS text readerをwrapperで維持し、PhoneとzedBSD phone backendが同じcodecを使用。base64/quoted-printable/identity、既存charset/階層上限を維持。text/plainとimage/*/video/* partを取得し、decoded媒体は計8MiB、MIME bodyは16MiB、media最大16個。上限超過/読めない媒体は明示エラー、内容を文字として表示しない。
2. MAP GetMessageで添付を要求し、MMSのMIME bodyをblobとして中継、写真/動画だけのメッセージも取り込む。送信はPhoneが標準MIMEを作り、libkeilandのFD request→compositor→zedBSD backend→bluetoothd→TYPE:MMS PushMessageへ運ぶ。送信成功/失敗/切断は既存request状態へ接続。daemonは永続保存しない。
3. Wayland phone interfaceをversion 29、libkeilandを81へ拡張。MIMEを匿名FDで渡し、public item末尾に有無付きdescriptorを追加。旧callerには旧サイズのfieldsのみを渡しFDを解放、新callerはdescriptorをcloseする。backend itemは次takeまで所有、public itemはcallerに所有移譲。本文は従来のUTF-8、写真データはwireの文字列に入れない。OS別unsupported backendはENOTSUP。
4. Phoneのstoreが画像/動画を `~/Documents/Phone/` の管理下へ保存し、再open・同期重複/同一画像複数partを扱う。番号/本文をlogに保存しない。写真は既存PNG/JPEG/GIF decoderで実画像を描く。動画はtimelineに実媒体の項目を表示し、クリックでVideo Playerを開く（codec対応/fallbackは既存appの責任）。Attach File chooserで写真/動画を選び、既存送信操作で送る。自動変換/圧縮は今回追加しない。
5. 最終変更source全体をC全文/境界/format/style-checkで確認、MIME roundtrip/画像・動画part/バイナリ/上限、Get/Pushのbyte、FD所有/旧ABI、backend→Phone保存/再open/重複、実画像描画、chooser/sendの接続のhost回帰と named build warning0。実機4項目はユーザーに具体的な手順を提示して検証し、未実施と成功を分ける。

## 完了条件・検証境界

4機能の取得/中継/保存/表示または再生起動/送信が実装され、host回帰とzedBSDのbluetoothd/wayland/libkeiland/Phone named build warning0、Linuxの変更した共通interface build、全文規約が通る。carrier/スマホ側のサイズやMMS Push未対応は明示エラーで表示し、SMSやMMS本文を壊さない。実機UATは別途ユーザーの結果を記録する。unknown filename代替textの詳しいbMessageはまだ未取得。

## 2026-10-10 追加と設計更新

p004のMMS text-only判断から、ユーザー指示で写真/動画の双方向転送へ拡張。p010を追加し、p004とWSへ相互参照を保存。WS全体はincomplete、p009の最終WS conformanceはp010も含む。共有master/Queueへの投影はQ1へ保留。

## 2026-10-10 メディア管理を先行する指示

ユーザーがCLI所有のメディアDB、compositor経由の取得・追加・監視、Photos再読込、Phoneの＋とDnDを先行指定。MMS送受信接続はその後（確認質問の前提と回答）。従来のPhone独自メディア保存の設計を置き換える。現在のMIME/Get/Push/backend変更は未統合・未完了の途中資料としてこのworktreeに保存。共有codecの画像/動画各262147 bytes roundtripとbMessageはhost PASS、bt-bmsg 90 checks PASS、MAP回帰は旧仕様assertを修正中。新メディアAPIの完成後、FD運搬と送受信を新基盤へ接続して再検証する。Phaseはclearedにしない。

2026-10-10再設計の反映: メディアCLI/compositorの[ws157-p006](../../ws157/phase006/phase.md)とPhoneの[ws197-p011](../phase011/phase.md)の出力を次回の前提にする。取得/保存はパスとメタデータのAPI、draft選択は共通media chooserに接続する。元の写真/動画4機能の義務は保持するが、ユーザーの最新限定により今回Queueで実装再開しない。旧MIME試作のscope/evidenceは[履歴](../history/mms-prototype-20261010.md)へ保存。
