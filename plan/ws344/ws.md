<!-- awesome-plan project=zedbsd record=ws344 -->

# WS344 libwebp-compat（自前の WebP の decoder、libwebp と互換の API。libbrowser の画像、VP8・VP8L・ALPH・RIFF）

- Master: [plan/master.md](../master.md)（2026-11-02 11:31 作成）。Primary Milestone: MG006。
- Status: incomplete（P2 が p001 の設計）
- 目標: WebP の画像（非可逆の VP8 の key frame、可逆の VP8L、透明度の ALPH、RIFF の入れ物、animation は後）を読む自前の module を作り、libbrowser の img_sniff・img_decode に RIFF/WEBP の枝を足す。Windows 版のブラウザ（static link）と zedBSD の両方で使う。
- 出典: RFC 9649（WebP）、RFC 6386（VP8）。試験の期待の値は libwebp（BSD）の出力と照らす（libwebp は試験の道具としてだけ host で使い、tree に取り込まない）。
- 進め方: 標準廉価実装手順（docs/agent/spec-module-delegation.md）。Opus が設計（module の分け方・header・試験の道具）、Sonnet が実装。

## ユーザーの決定

- （2026-11-02 11:32）ユーザーから、libwebp-compat（libwebp と互換の API を持つ自前の library、libpng-compat などと同じ形）を標準廉価実装手順で実装する、という趣旨の指示を受けた。
- （2026-11-02 11:31）ユーザーから、BUG-405（about.google の画像が WebP で届き表示されない）を、libwebp の package でなく自前の WebP の decoder で直す、という趣旨の答えを受けた。

## Phase

| Phase | 目的 | 状態 | 依存 |
| --- | --- | --- | --- |
| ws344-p001 | 設計（標準廉価実装手順 §4）: module・header・骨格・試験の道具・実装の Phase の表、試験の画像の集め方（libwebp の cwebp で作る、適合の試験の画像） | in-progress（P2 q1939） | — |

## Resume point

- 2026-11-02 11:31 Q1: 作成。p001 の担当を割り当てる。
