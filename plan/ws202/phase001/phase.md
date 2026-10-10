<!-- awesome-plan project=zedbsd record=ws202-p001 -->

# ws202-p001: 設計（design.md）と review

Status: in-progress（2026-10-10 第 4 版＝最後の版。ユーザーの判断 H1〜H6・J1〜J10 の答え待ち）
Disposition: normal
Parent: [WS202](../ws.md)
見積もり: 5 LW（第 1 版 3、第 2 版 1、第 3 版・第 4 版 1）
依存: —

## 範囲

[design.md](../design.md) を書く。実装・build はしない（2026-10-11 ユーザー「設計だけ書いてください。実装は別なセッションで行います。」）。

## 経過

- 2026-10-10 第 1 版: 今の形の事実、mp4（既存の `mediafile/mp4.c`）、Vulkan Video の H.264、AAC-LC、共通の部品、A/V、API、試験、license、判断の点 H1〜H6。
- 2026-10-10 design-reviewer の [review-001](../review-001.md)（e8adcdf89）: H 7・M 13・L 13 と判断の点 J1〜J6。
- 2026-10-10 第 2 版: H-01〜H-07・M-01〜M-13・L-01〜L-11・L-13 を反映（L-12 は Q1 が WS083 の側で直した）。対応は design §16。review の
  「確かめていないこと」のうち host で確かめられた物は design §15（E1〜E9）、確かめられない物は design §14（U1〜U15）。Phase を p015 で 1 つ足し、
  見積もりは 62 → 75 LW。
- 2026-10-10 design-reviewer の [review-002](../review-002.md)（5e6dcce0e）: 新しい H 1・M 10・L 20、p015・p009 は NO-GO。
- 2026-10-10 第 3 版: H2-01（方式は推しの (a) を仮に採り D25・新 p016）、M2-01・M2-02・M2-04・M2-06・M2-08・M2-09・M2-10、L2-01〜L2-20 を反映。M2-03・M2-05・M2-07 と
  H2-01 の方式は判断の点 J7〜J10 にし、推しを「仮」として設計に入れた。対応は design §16.2。host で確かめた事実は design §15 E10〜E14。見積もりは 75 → 85 LW。

- 2026-10-10 design-reviewer の [review-003](../review-003.md)（ee7df65dc、範囲を絞った 3 回目）: p015・p016・p009 は条件付き GO、方式の選び直しは不要、4 回目の review は要らない。
- 2026-10-10 第 4 版（最後の版、Q1 の指示）: H3-01・M3-02〜M3-05・L3-01〜L3-07 を反映（U10 は閉じた）。J8 の材料に「保守的な判定で gap の後は最大 `max_num_ref_frames` 枚の間
  B が出ない」を足した。対応は design §16.3。見積もりは 85 → 87 LW（p015 4 → 5、p016 3 → 4）。p009 は p015 の後に p016 と並べる（L3-06）。

## 残り

**ユーザーの判断 H1〜H6・J1〜J10 の答え待ち**（一覧は [ws.md](../ws.md) の「人の判断の点」、詳しくは design §13。J7〜J10 は推しを「仮」として設計に入れてある）。

1. 答えを ws.md と design.md に記録し、推しと違う答えの Phase を直す（例 H1 (b) なら CPU の decoder の Phase、J8 (b)・(c) なら p016 の取り消しと p009 の参照の渡し方、
   J1 で container を絞るなら §3.1 と p010 の試験を縮める）。
2. WS を planned に、この Phase を cleared に（Q1 の判定）。設計の直しの差分は Q1 が照らす（4 回目の review は要らない、review-003 の推し）。
