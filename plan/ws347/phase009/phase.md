<!-- awesome-plan project=zedbsd record=ws347-p009 -->

# ws347-p009: host が local memory に GOP の VRAM 以外を書かない形に

Status: in-progress（2026-11-13 Q1、q2043-i01 P4）
Parent: [WS347](../ws.md)

## 目的

2026-11-13 ユーザーの訂正: host が窓（BAR の local memory の窓）から GPU の local memory に書いてよいのは GOP の VRAM（firmware が残した boot の framebuffer）だけ。ring・vertex・fence など host と GPU が分け合う物は全部 host memory に置き、GPU が DMA で読み書きする（台帳 B35）。

## 範囲

1. `src/drivers/gpu/ghci/display.c` の `copy_by_processor`（blitter も copy の program も使えない時に CPU が local の窓を通して frame に書く予備の道）を削除する。GOP を残して使う事はまず無い（ユーザー）。予備の道が無くなった時の振る舞い（executor の無い GPU・program の拒否の時に copy の present を断る等）を本来の形で決める。
2. driver の全体で、host が local の窓を通して local memory を読み書きする所を点検し、GOP の VRAM の引き継ぎ以外を無くす（無くせない物は根拠を書いて Q1 へ）。fence・ring・descriptor・PSO など host が読む・書く物が host memory にある事を確かめる。
3. host 試験（WS290 の道具）を追従させ全 PASS、vf2・x86_64 の kernel warning 0。
