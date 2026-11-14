<!-- awesome-plan project=zedbsd record=ws302-p001 -->

# ws302-p001: DTB の検討と設計

Status: cleared（2026-11-14 Q1。第 3 版、merge 39b8c7e7d0）
Parent: [WS302](../ws.md)
Queue: q1775-i01（P5）

## 範囲

ws.md の p001 の行のとおり。どの統計からどの性質を推すか、sched への hint の口（kernel の中、HAL を変えない）、5330（P 2・E 8）の割り付けの方針、計測と受け入れの試験、Phase の表。header の案は `proposed/` に。design-reviewer を 1 回通す。ユーザーの判断が要る点は [../user-questions.md](../user-questions.md) に。source は変えない。

## 進み（2026-10-31 P5）

- 設計 [../design.md](../design.md) 第 1 版（main 3d820d9ca の code を読んで書いた）、header の案（`proposed/include/kern/sched-hint.h`・`dtb.h`。本物の [include/kern/sched-hint.h](../../../include/kern/sched-hint.h)・[include/kern/desktop-balancer.h](../../../include/kern/desktop-balancer.h) が p002 で出来て、案は p003 で消した）（host の cc で `-fsyntax-only -Wall -Wextra`、`CONFIG_KERN_DESKTOP_BALANCER=0/1` の両方で通る事だけを確かめた）。
- 問い Q302-1〜7（[../user-questions.md](../user-questions.md)）。Q1 への点は design.md §10.2。
- design-reviewer（1 回、blocker 2・major 11・minor 13）の結果は [../design-review.md](../design-review.md)。全部を第 2 版に反映した（design.md §14）。主な変更: user の thread を kernel の worker の段（8）より上に置かない、`sched_set_cpu` の状態の確かめ直し、手渡しの席を上書きしない、MOVE は希望の種類の idle の CPU だけ、S2 を 4 本に、試験の stress を p003 に。
- 観察の UAPI の形の案 [../proposed/include/uapi/desktop-balancer-sysctl.h](../proposed/include/uapi/desktop-balancer-sysctl.h)（-m64・-m32 で 720 byte）。

- 2026-11-14 P6（q2093-i01）: ユーザーが Q302-1〜7 を全部推しで決めた（2026-11-14）。設計を第 3 版に改訂した（今の main fd18bd61ee の code・新しい build system と照らし、冒頭に改訂の要点と食い違いの表 D-1〜D-15、決定を §10.1 に）。design-reviewer の 2 回目（blocker 0・major 6・minor 9）を反映した（design.md §15、design-review.md の「第 3 版の review」）。

## 受け入れ

- design-reviewer の指摘の反映（済み、design.md §14）。
- ユーザーの review（Q302-1〜7 の答え）。2026-11-14 に全部推しで決まった。cleared は Q1 の判定（第 3 版の改訂の後）。

## 後の Phase への持ち越し

- p002: `CONFIG_KERN_DESKTOP_BALANCER=n` が今と同じ事は最終の `vmunix` の逆 assemble で比べる（LTO の `.o` は bitcode）。
- p003: 手渡しの後継の 4 か所と tick の頭の `complete_handoff`、席を上書きしない規則、`move_pending` の後始末、`sched_set_cpu` の確かめ直し（design.md §4.6.5・§4.6.7）。
- p006: 5330 の CPU の番号の並びと SMT の兄弟の対を最初に記録（design.md §12）。
