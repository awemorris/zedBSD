<!-- awesome-plan project=zedbsd record=ws202-p013 -->

# ws202-p013: 5330 の UAT（ユーザー）と結果の反映

Status: planned
Disposition: normal
Parent: [WS202](../ws.md)
見積もり: 2 LW（UAT の一覧の用意と結果の記録。ユーザーの時間は含まない）
依存: p012

## 目的

ユーザーが 5330 で、libavcodec の無い image の Video Player と Music を使い、再生の質を確かめる。

## image

`plan/ws202/tests/config-media.mk`（CI の構成から libavcodec を外した物。graphical login・Video Player・Music・audiod を含む）。Q1 が build し、ユーザーが USB に書く。
UAT の試料は `/home/awe/zedbsd-media/` の sample（`sample-h264-high-aac.mp4` 等）と手元の動画・.m4a を、ユーザーか Q1 が SSH で kei の home に送る。

## UAT の一覧

| # | 項目 | 手順 | 期待 |
| --- | --- | --- | --- |
| U1 | 動画の再生 | Files から `sample-h264-high-aac.mp4`（または手元の H.264＋AAC の mp4）を Video Player で開く | 絵と音、止まり・途切れが無い |
| U2 | 音と絵の同期 | 口の動き・手拍子のある手元の動画 | ずれが分からない |
| U3 | seek | 矢印で 10 秒の前後、bar を drag | すぐ新しい位置から、乱れた絵が出ない |
| U4 | 一時停止・再開・全画面・終わり | Space、F11、終わりまで | 期待通り |
| U5 | 縦横比・色 | 非正方の画素・色の範囲の違う動画があれば | 歪まない、色が自然 |
| U6 | Music | 手元の .m4a（iTunes・CD の取り込み）を数曲、次の曲、seek | 雑音・途切れが無い、曲の終わりで次へ。iTunes の file の頭に約 46 ms の無音が残ることがある（`iTunSMPB` を読まない、制限） |
| U7 | HE-AAC（H3 (a) の時、手元にあれば） | 低い bitrate の .m4a | 鳴る（高音が欠けるのは仕様） |
| U8 | 扱わない形 | interlaced・10 bit・HEVC の mp4 があれば | notice で落ちない |
| U9 | desktop の滑らかさ | 動画を再生しながら window を動かす | desktop が引っかからない |

## 記録

- ユーザーの回答を ws.md の「5330 の UAT」の節（この Phase で足す）に原文で。問題は Bug Board か、担当の Phase を uncleared に戻すかを Q1 が決める。
- UAT の後、docs（p011）に実機で確かめた範囲を反映する。
