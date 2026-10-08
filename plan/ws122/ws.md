<!-- awesome-plan project=zedbsd record=ws122 -->

# WS122: 動画プレーヤアプリ

<!-- awesome-plan-current:start -->
Status: completed（2026-10-08 Q1 の判定（sweep-beta2-rc2 §2.2）。旧: incomplete）
Primary Milestone: MG006
Parent: [Master](../master.md)
Queue: なし
<!-- awesome-plan-current:end -->

## 結果（2026-10-08 完了）

動画の player: libavcodec の package（FFmpeg 9.0.2、LGPL の構成）、dlopen の add-in の videoplayer（再生・一時停止・seek、T1-133・T1-191）、全画面と game mode の direct scanout（T1-237・T1-244）。

## 制限・移管

GPU の decode は WS083、独自の container の読みは libmedia（WS177 の案 T）、独自の AAC は後。全文規約はベータ3。実機の direct=1 は UAT。回帰に使う試験（run-host-codec.sh・run-host-mediafile.sh・sample.mp4・host-layout.c）は plan/tools へ移す案（Q1）。

## Phase

| Phase | 内容 | 最終の状態 |
| --- | --- | --- |
| ws122-p001 | 要件・設計（2026-10-05 の計画の段 1〜4、libavcodec の package の license の構成と dlopen の add-in の扱いを含む） | cleared（2026-10-05） |
| ws122-p002 | 簡単な player（開く・再生・一時停止・停止・シーク、audiod の音）、ベータ1 | cleared（2026-10-05） |
| ws122-p003 | 独自の container の読み込み（MP4・Matroska/WebM の demux）、ベータ2 の段 2 の前半 | cleared（2026-10-07） |
| ws122-p004 | player を mediafile と libavcodec の dlopen の add-in（software decode、header 無し）で完成（2026-10-05 | cleared（2026-10-05） |
| ws122-p005 | ws122-p005: 設計 — 動画の全画面と合成を通さない直接の scanout（game mode） | cleared（2026-10-07） |

Phase の directory とこの WS だけの試験は、完了の規則（AGENTS.md）で削除する（git の履歴に残る、削除は Q1）。
