<!-- awesome-plan project=zedbsd record=ws157 -->

# WS157: Keiland の app: 写真の管理

<!-- awesome-plan-current:start -->
Status: incomplete（2026-10-08 q902 P1 の照合: p001・p004・p005 cleared（2026-10-07 Q1、T1-331）。ベータ2 の範囲の残りは無い、続きはベータ3）
Primary Milestone: MG006
Related Milestones: —
Parent: [Master](../master.md)
Queue: [Codexメディア管理実行記録](../ws197/codex-queue.md) active（media-uat-20261010）。共有Queue投影はQ1。
Resume point: 2026-10-10 p006/p007 cleared。Media JSON CLI/compositor/PhotosとPhone＋DnDをmain 874e12d3bへ統合。実機GUI UATは未実施。WS全体はベータ3の残る受入を含みincomplete。
Target: **ベータ3**（続き）（2026-10-07 ユーザー「下記をベータ3に移動します。・左手デバイスOSK、ゲームパッドOSK, 写真の続き, カレンダーの続き, IMEの続き、POSIX, NVMe, make, RTL8822C, Sleep」）
<!-- awesome-plan-current:end -->

## 単一目標

Keiland の標準 app として、写真を集めて整理し、見る app を作る。

## ユーザーの指示（2026-10-05、原文）

「Keilandアプリとして、写真管理ソフト、音楽プレイヤー、動画プレイヤーを追加します。それぞれWSがなければ立ててください。写真管理ソフトは要件の検討からスタートですね。」

## Phase

| Phase | 内容 | Status | 依存 |
| --- | --- | --- | --- |
| [ws157-p001](phase001/phase.md) | 要件と設計（2026-10-07 ユーザーの要件で書き直し、D1〜D7） | cleared | — |
| [ws157-p002](phase002/phase.md) | 最初の既定案の library（~/Pictures を読む） | uncleared・canceled（置き換え） | — |
| [ws157-p003](phase003/phase.md) | 最初の既定案の app | uncleared・canceled（置き換え） | — |
| [ws157-p004](phase004/phase.md) | library・db（月ごとの TSV・album ごと）・取り込み（複写、重複は取り込まない）と host 試験 | cleared（2026-10-07 Q1、T1-331） | p001 |
| [ws157-p005](phase005/phase.md) | app（取り込み・album の card・縮小画像の cache、AAT） | cleared（2026-10-07 Q1、T1-331） | p004 |
| [ws157-p006](phase006/phase.md) | メディアCLI・compositor API・Photos移行 | cleared | p004 source |
| [ws157-p007](phase007/phase.md) | 最終全文規約・build・回帰 | cleared | p006、ws197-p011 |
| [ws157-p008](phase008/phase.md) | folder選択・背景import/list・大きなJPEGの表示修正 | in-progress | p006 source、ユーザーJPEG |
| [ws157-p009](phase009/phase.md) | 今回UAT全sourceの全文規約/host/build/main統合 | in-progress | ws197-p012、ws127-p013、p008 |

## p001 の観点（要件の検討）

- 何を管理するか: library（取り込み先の folder を見張るか、指定の folder を読むだけか）、album・日付・場所（EXIF の GPS）・人（顔の認識は範囲か）・お気に入り。
- 見る: 一覧（日付の timeline、grid）、拡大・slideshow、動画（WS122）も同じ library に入れるか。
- 編集: 回転・切り抜き・明るさなど簡単な補正の範囲。元の file を書き換えるか（非破壊の編集）。
- 取り込み: camera・SD card・USB（PnP の通知 WS132）からの取り込み。
- 形式: JPEG・PNG・HEIC・RAW など（今の画像の decoder の独自実装（libjpeg-compat・libpng-compat）との関係、HEIC の codec の license）。
- 共有: クラウドストレージ（WS146・WS147）との関係。
- 既存の Image Viewer（WS128 の imageview）との役割の分け方。
- 他の app（Apple Photos・Google Photos・Shotwell・digiKam）の調べ。模倣の範囲に注意（Files の Tags の件と同じく、特定の製品の固有の UI の写しは避ける）。

## 2026-10-10 メディア管理の先行実装

ユーザーの新規承認により、PhotosのDB処理をCLI所有にしcompositor APIから使う。Phoneの＋とDnDは同じAPIを使う。既存Photos p004/p005のcleared履歴は維持、新p006/p007とws197-p011で変更後を検証する。MMS写真/動画送受信は基盤完成後に接続。旧途中treeは未統合。共有master/Queueへの投影はQ1に保留。

2026-10-10構造更新: ws157-p006はメディアCLI/API/Photos、ws157-p007はその最終全文規約、ws197-p011はPhone選択/DnDと自身の全文規約。p011はp006の検証済みAPI出力に依存する。全scopeはユーザーのメディア管理先行承認を維持。

## 2026-10-10 保存形式の変更承認

ユーザー指定を優先し、従来の `~/Pictures/Library` 月別TSV保存を現scopeで置き換える。`~/Pictures/Media/metadata.db` はversion付きJSON、原本copyは `Media/Files/YYYY/MM/dd/名前`。JPEG EXIF撮影日時がなければPNG/JPEG/動画等は取り込み日で整理する（元ファイルmtimeは使わない）。日付・バイト数・画像寸法・hash・原名・favorite・rotation・albumを保持し、未知のJSON fieldを更新時にも保存することで撮影地等へ拡張可能にする。既存Libraryの自動移動/削除はしない。旧ファイルはそのまま、必要な原本はmediastorage addで再取り込みできる。p006設計・p007検証・p011の選択元へ同じ承認を反映。以前のcleared履歴は旧形式の履歴として維持する。

## 2026-10-10 scoped clearance

p006・p007をcleared。main `874e12d3b`、[最終検証](tests/verification-20261010.md)。JSON保存・metadata/path-only・spawn/pipe・Wayland通知の現仕様を満たす。WS157全体はincomplete、実機UIの[確認手順](tests/mediastorage-usage.md)を残す。関連Phoneはws197-p011 cleared、MMS接続はws197-p010の未完了を維持。

## 2026-10-10 Photos UAT修正の追加

ユーザー指定でp008/p009を追加。共通chooserにdirectory選択を追加し、Photos import/listをprivate Wayland clientを持つworkerへ移す。metadataのUI所有/FD handoff/通知coalesce/close/dirty marksを維持。実機JPEGはdecode成功後の16MiB GPU resource上限が原因で、Image ViewerのCPU sampling fallbackへ修正。[ws197-p012](../ws197/phase012/phase.md)/[ws127-p013](../ws127/phase013/phase.md)の変更もp009で検証する。ベータ3のWS全体scopeは維持。
