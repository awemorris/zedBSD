<!-- awesome-plan project=zedbsd record=ws202-p001 -->

# ws202-p001: 設計（design.md）と review

Status: in-progress（2026-10-10 第 1 版を書いた。design-reviewer の review と H1〜H6 の回答の後に cleared）
Disposition: normal
Parent: [WS202](../ws.md)
見積もり: 3 LW
依存: —

## 範囲

[design.md](../design.md) を書く。実装・build はしない（2026-10-11 ユーザー「設計だけ書いてください。実装は別なセッションで行います。」）。

## 成果

- `plan/ws202/design.md` 第 1 版（2026-10-10）: 今の形の事実（§1）、mp4 の demuxer（§4、既存の `mediafile/mp4.c` を使う）、Vulkan Video の
  H.264 の back end（§5）、AAC-LC の自前の decoder（§6）、共通の部品（§7）、A/V（§8）、API と app（§9）、試験（§10）、license（§11）、
  人の判断 H1〜H6（§13）、未確認 U1〜U8（§14）。
- `plan/ws202/ws.md` と p002〜p014 の phase.md。

## 残り

1. design-reviewer の review（設計ごとに必ず）。指摘は design.md の改版（§ 末尾に review と反映先の節を足す、WS083 の design の形）。
2. H1〜H6 のユーザーの回答を ws.md と design.md に記録し、選ばれなかった選択肢の Phase の変更（例 H1 の (b) なら CPU の decoder の Phase を足す）。
3. 回答の後、WS を planned に、この Phase を cleared に（Q1 の判定）。
