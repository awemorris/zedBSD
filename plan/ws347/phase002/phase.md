<!-- awesome-plan project=zedbsd record=ws347-p002 -->

# ws347-p002 仕様の差の読みと driver の追従の設計

Status: cleared

- 親: [WS347](../ws.md)。状態: planned。

## 範囲

- amaterrace-gpu の `docs/ghci.md`・`include/` を `git show <SHA>:path` で読み、WS290 が実装した版（cb937e6〜d9252e4、2026-10-28）から
  今の main（ghci.md a7f5ca47、2026-11-06）までの差を一覧にする。SHA を記録する。
- 増えた・意味の変わった CAPS（DEVICE_CAPS の CAP_BLT・CAP_PROT・CAP_TEX_FFILT・CAP_ROP_PACK・CAP_HW_WALKER など、EXT_CAP_FEATURE の
  CAP_BLT_SCALE・CAP_CLIP_POLY、EXT_CAP_FMT0 の CAP_TU_BILINEAR・CAP_BLT_BLEND・CAP_BLT_TILE ほか）ごとに、bit が 0 の時の driver の扱い
  （shader での代替・Vulkan の機能や format の報告から外す・ENOTSUP）を決める。WS290 の design.md §24（capability → Vulkan）を土台にする。
- 仕様の誤り・未定義は推しで埋めず一覧にしてユーザーに示す。
- 設計は design-reviewer を通す。

## 受け入れ

- 差の一覧と CAPS ごとの扱いの表（`plan/ws347/design.md`）、p003 の作業の分け方。code は書かない。

## 記録

- 2026-11-11 Q1: ユーザーの指示で CAPS 化の差を先に調べた。結果は [spec-diff.md](../spec-diff.md)（仕様の差 11 項目、minimal の RTL の値と仕様の食い違い D1〜D8）。
