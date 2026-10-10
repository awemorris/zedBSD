<!-- awesome-plan project=zedbsd record=ws202-p001 -->

# ws202-p001: 設計（design.md）と review

Status: in-progress（2026-10-10 第 2 版。H1〜H6・J1〜J6 の回答の後に cleared）
Disposition: normal
Parent: [WS202](../ws.md)
見積もり: 4 LW（第 1 版 3、第 2 版 1）
依存: —

## 範囲

[design.md](../design.md) を書く。実装・build はしない（2026-10-11 ユーザー「設計だけ書いてください。実装は別なセッションで行います。」）。

## 経過

- 2026-10-10 第 1 版: 今の形の事実、mp4（既存の `mediafile/mp4.c`）、Vulkan Video の H.264、AAC-LC、共通の部品、A/V、API、試験、license、判断の点 H1〜H6。
- 2026-10-10 design-reviewer の [review-001](../review-001.md)（e8adcdf89）: H 7・M 13・L 13 と判断の点 J1〜J6。
- 2026-10-10 第 2 版: H-01〜H-07・M-01〜M-13・L-01〜L-11・L-13 を反映（L-12 は Q1 が WS083 の側で直した）。対応は design §16。review の
  「確かめていないこと」のうち host で確かめられた物は design §15（E1〜E9）、確かめられない物は design §14（U1〜U15）。Phase を p015 で 1 つ足し、
  見積もりは 62 → 75 LW。

## 残り

1. H1〜H6・J1〜J6 のユーザーの回答を ws.md と design.md に記録し、推しと違う回答の Phase の変更（例 H1 (b) なら CPU の decoder の Phase を足す、J1 で
   container を絞るなら §3.1 と p010 の試験を縮める）。
2. 変更が大きければ 2 回目の review（Q1 の判断）。
3. WS を planned に、この Phase を cleared に（Q1 の判定）。
