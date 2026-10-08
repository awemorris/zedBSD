<!-- awesome-plan project=zedbsd record=ws157 -->

# WS157: Keiland の app: 写真の管理

<!-- awesome-plan-current:start -->
Status: incomplete（2026-10-08 q902 P1 の照合: p001・p004・p005 cleared（2026-10-07 Q1、T1-331）。ベータ2 の範囲の残りは無い、続きはベータ3）
Primary Milestone: MG006
Related Milestones: —
Parent: [Master](../master.md)
Queue: なし（q835 は終了）
Resume point: 2026-10-08 q902 P1 の照合: p004・p005 cleared（T1-331）。T1-314 の FAIL は古い image（protocol の 2026-10-08 の節）。Photos の drag の元は WS189（T1-434・437）。続き（写真の続き）はベータ3。旧: p005 の T1 の結果（`apps.photos.*`）。
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

## p001 の観点（要件の検討）

- 何を管理するか: library（取り込み先の folder を見張るか、指定の folder を読むだけか）、album・日付・場所（EXIF の GPS）・人（顔の認識は範囲か）・お気に入り。
- 見る: 一覧（日付の timeline、grid）、拡大・slideshow、動画（WS122）も同じ library に入れるか。
- 編集: 回転・切り抜き・明るさなど簡単な補正の範囲。元の file を書き換えるか（非破壊の編集）。
- 取り込み: camera・SD card・USB（PnP の通知 WS132）からの取り込み。
- 形式: JPEG・PNG・HEIC・RAW など（今の画像の decoder の独自実装（libjpeg-compat・libpng-compat）との関係、HEIC の codec の license）。
- 共有: クラウドストレージ（WS146・WS147）との関係。
- 既存の Image Viewer（WS128 の imageview）との役割の分け方。
- 他の app（Apple Photos・Google Photos・Shotwell・digiKam）の調べ。模倣の範囲に注意（Files の Tags の件と同じく、特定の製品の固有の UI の写しは避ける）。
