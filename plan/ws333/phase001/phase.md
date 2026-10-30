<!-- awesome-plan project=zedbsd record=ws333-p001 -->

# ws333-p001: /dev/thermal で統一する電力・熱の設計

Status: in-progress
Parent: [WS333](../ws.md)
Queue: q1734（P5）

## 範囲

ws.md の p001 の行のとおり。WS301・WS303 の design.md・user-questions.md・review を入力にし、CPU の idle（C-state）・P-state（HWP と driver の P-state）・P/E core の相対性能・熱の zone・fan・strategy・非常の停止を 1 つの /dev/thermal の UAPI と kernel の枠組みにまとめる。p002 の amd64 の idle（MWAIT）は先に実装するので、その形と矛盾しない設計にする。ユーザーの判断が要る点は、背景と案と用語の説明を付けて一覧にする。design-reviewer を通す。

## 進み（2026-10-31 P5）

- 設計の第 1 版（500061ba1）→ design-reviewer（[../design-review.md](../design-review.md)、blocker 0・major 8・minor 24）→ 第 2 版 [../design.md](../design.md)（反映の表は §14、p002 の main f740839e5 の結果を入れた）。問いは [../user-questions.md](../user-questions.md)（Q333-1〜7）、Q1 の判断は design.md §11.2（10 点）。
- p002 に求める条件 1〜7 は Q1 経由で P1 に送った（design.md §4.3.1）。

## 後の Phase への持ち越し（phase.md を立てる時に写す）

- p017・p018: WS303 design §2.1 の「EC は ECM1 の下で読む」は method 0x20 には当たらない（0x20 以外は）と読む（WS303 v5 m5）。
- p018: WS303 design §2.3 の「ACPI の thermal zone」は Linux の thermal_zone（DPTF の participant）と読む（v5 m24）。5330 の熱の試験は合否の手順と記録だけの手順を分ける（v5 m18）。試験の前に Debian で mode の set → 再起動 → 読みで mode が残る事と profile の daemon が無い事を確かめる（v5 M8）。
- p012: 今の形に戻す build の macro、lock の取り合いの確かめ、`lapic.c` の開発の管理情報の comment（あれば）を消す。
- p005・p006・p016・p022: 手を入れる file の開発の管理情報の comment を消す（design.md §4.16）。
- p003: `kern_cpu_count()` を kernel に足す（HAL は変えない）。
