<!-- awesome-plan project=zedbsd record=ws338 -->

# WS338: libmedia の H.264 の software の decoder の速さの続き

- Master: [plan/master.md](../master.md)（2026-11-01 作成）。Primary Milestone: MG006。
- Status: planning（2026-11-15 p001 の設計を P1 が q2121 で書いた。問い Q338-1〜3 の答えと p001 の cleared は Q1）
- 目標: [WS326](../ws326/ws.md) で正しさを確かめた H.264 の software の decoder を速くする。Pi 5 で 1080p30 の High を decoder 4 thread で 40 fps 以上（WS326 p024 の設計の目標）を受け入れの目安にし、5330 でも測る。
- 範囲（WS326 の「制限・移管」の案 a〜e）: a deblocking の filter と weight、b neighbour・transform・syntax の消し・CABAC の file の中の fast path、c thread（deblock の行の pipeline、parse と再構成の分離）、d SIMD（NEON・SSE2。扱いは Q338-1）、e Pi 5・5330 の実測と目標の判定。
- 出発点: WS326 p024 の設計と計測。2026-11-15 の計測は [design.md](design.md) §3。
- 設計: [design.md](design.md)（決まった事・乖離・計測・手段・受け入れの数・Phase の表・敵対的レビュー）。問い: [user-questions.md](user-questions.md)。計測の要約: [measure/](measure/)。
- Resume point: p001 の設計と review の反映は済み（2026-11-15）。Q1 が p001 を merge し、Q338-1〜3 をユーザーに聞く。次は p002（host の計測の道具）、その後に並べて p004〜p006（Sonnet）・p007。p003 は build の -O2 が main に入り実機が割り当てられた後。

## ユーザーの決定（要点）

- 2026-11-01 09:05 ユーザーから、WS326 の速さの続きは WS326 に Phase を足さず新しい WS にする、という趣旨の答えを受けた（Q1 の推しの案）。
- 2026-11-01 09:30 Q1: SIMD（範囲の d）は WS329 のユーザーの答えと同じ形で行う（gcc・clang の vectorizer の report を見て C を整える Phase → 表せない所は short vector 型の intrinsics）。
  → 2026-11-15 p001: 元にした WS329 の 11-01 の答えは 2026-11-14 に取り消された（WS329 は SIMD を範囲の外）。decoder の SIMD は Q338-1 でユーザーに聞く（推し: 自動の vector 化が効く C だけ、short vector 型は Pi 5 の実測で届かない時に改めて聞く）。
- 2026-11-15 ユーザーから（Q1 経由）、userland の program と shared library の両方を -O2 -Wall -Wextra -Werror で build する、という趣旨の決定を受けた（p001 で libmedia.so が -O0 で build されていると分かった後）。Q1 が全 userland の build で warning 0 を確かめて当てる。WS338 の設計と受け入れの数は -O2 が前提。

## Phase

型: O = Opus、S = Sonnet 5.5 high、D = conformance-reviewer。

| ID | 目的 | 型 | 状態 | 依存 |
| --- | --- | --- | --- | --- |
| p001 | 設計（WS326 p024 の案 a〜e を Phase に分ける、受け入れの数） | O | in-progress（2026-11-15 Q1、b82d0fb060 を main に merge 6aeb76a864。Q1 の独立の設計レビューの結果待ち、問い Q338-1〜3 はユーザーへ） | WS326 |
| [p002](phase002/phase.md) | host の計測の道具（compiler の指定・module の時間・出力の MD5 の一覧・zedBSD 用の bench の build・実機の 1 本の script）と host の基準 | O | planned | p001 |
| [p003](phase003/phase.md) | Pi 5・5330 の基準の実測と換算の置き換え | O | planned | p002、-O2 が main に、実機 |
| [p004](phase004/phase.md) | deblock の書き直し | S | planned | p002 |
| [p005](phase005/phase.md) | interp・weight の書き直し | S | planned | p002 |
| [p006](phase006/phase.md) | transform・mb の書き直し | S | planned | p002 |
| [p007](phase007/phase.md) | entropy の速い道（header の inline） | O | planned | p002、WS329 と header の順 |
| [p008](phase008/phase.md) | 段の分離（parse・再構成・deblock、1 thread） | O | planned | p004〜p007 |
| [p009](phase009/phase.md) | thread（picture の中の pipeline） | O | planned | p008 |
| [p010](phase010/phase.md) | Pi 5・5330 の実測と目標の判定、Pi 5 の再生 | O | planned | p009、p003、実機 |
| [p011](phase011/phase.md) | （条件付き）picture の重なり | O | planning | p010 |
| [p012](phase012/phase.md) | （条件付き、Q338-1）short vector 型の SIMD | O | planning | p010（p011） |
| [p013](phase013/phase.md) | 規約の全文の見直し | D | planned | p010（p011・p012） |

## p001 の結果（2026-11-15 P1、q2121-i01）

- 乖離（design §2）: libmedia.so が -O0 で build されていた（その後の決定で -O2 に）。p024 の数は gcc で zedBSD は clang。SIMD の記録が WS329 の取り消された答えに拠っていた。試験の path と BASE（b753848bf2）。適合の stream の cache が main から消えていた。WS318 の thread.c は無い。
- host の profile（1 thread、gcc -O2、grain・実 B）: deblock 21〜24%、interp 17〜19%、transform 11〜15%、CABAC の engine 9〜10%、recon 7〜9%、weight 6〜8%、mb 7%、cabac-se 6%、macroblock 4%。clang 19 -O2 は gcc の 1.10〜1.14 倍、-O0 は -O2 の 1/4.2〜1/4.4、-Os は 0.79 倍。
- design-reviewer: 29 件（blocker 3・major 11・minor 15）を design §11 のとおり反映。
- 未実施: 実機（Pi 5・5330）の計測（p003）、適合の試験は p001 では流していない（source を変えていない）。
