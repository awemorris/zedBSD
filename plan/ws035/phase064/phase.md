<!-- awesome-plan project=zedbsd record=ws035p064 -->

# ws035-p064: 物理的に追従するドッキングの解除

Phase ID: `ws035-p064`
Parent: [WS035](../ws.md)
Status: cleared（q482-i01、2026-09-26）
Phase disposition: normal
Queue: q482-i01
承認: 2026-09-26 ユーザーの自律実行の指示（デスクトップ関連を優先）。ws.md の planned の Phase。
設計: [wiseman-design.md](../wiseman-design.md)「上部の合体タイトルバー（ドッキング中）を下へ: 最大化の解除。10〜20 px 引くと窓の上端が
画面から離れ、さらに引くと左右にも余白、角丸と影が戻る。閾値を越えたら通常の大きさへ snap、届かなければバネで最大化へ戻る。途中で上へ
戻したら最大化に戻る」

## 範囲

1. バーの題名を押して下へ引く間、docked の窓が指に追従して縮む: 引いた距離 d の割合 p = d / 140 px で、docked の矩形から「元の大きさで
   指の下」の矩形へ（上端が離れ、左右に余白、角丸と影、浮いた題名の bar が fade in、バーの題名は隠れる）。
2. d が 140 px を越えたら通常の大きさへ（今までの undock と同じ、そのまま移動が続く）。
3. 閾値の手前で離すと、今の矩形から docked へ 220 ms で戻る（dock の animation）。上へ戻せば docked のまま。
4. 試験: 引いて途中の形（画面）、離して戻る（log）、引き切って解除（p062 の試験）。

## 受け入れ

1. Venus で引いた途中の窓が縮んで題名の bar が出ている（画面）、手前で離すと docked に戻る、越えれば元の大きさで指に付いて動く。
2. p062・p065 の試験が通る。build warning 0、style-check 0。

## 結果（2026-09-26、q482-i01）

cleared。受け入れ 1・2 を満たした（実機は未実施）。

### 実装（`userland/base/zwl/shell.c`、zwl.h）

- バーの題名の press で `pull_start_y`、motion で `pull_distance`（上へ戻せば 0 = docked のまま）。`PULL_DISTANCE` は 16 → 140 px。
- 引いている間の矩形 `pulled_rect`: docked の矩形から「元の大きさ、題名の同じ所が指の下」の矩形へ、p = d/140 を ease-out
  （1-(1-p)²）で。body は角丸と影（docked でない描き方）、浮いた題名の bar が p で fade in、バーの題名は隠れる（`docked_window` が NULL）。
- 140 px を越えたら今までどおり undock して移動が続く（矩形は連続）。
- 手前で離すと `pull_back`: 今の矩形から docked へ dock の animation（220 ms、題名がバーへ滑る）、`ZWL GLASS pull back`。

### 検証（QEMU・Venus）

- `plan/ws035/tests/zdesktop-p064.sh` PASS（build/ws035-p064.log）: 70 px 引いた途中で窓が縮み角丸と題名の bar（build/ws035-p064/pulling.png、
  窓の中が赤）、離すと docked に戻る（back.png、画面の端まで赤、`pull back`）、引き切ると `undock via=pull` と `moved`。
- 回帰: zdesktop-p062（引く距離 243 px で undock）・p065 PASS。build warning 0、shell.c の style-check 0。
- boot test PASS（build/ws035-p064-boot/login.png）。i915 実機は未実施。
