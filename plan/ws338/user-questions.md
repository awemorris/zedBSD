<!-- awesome-plan project=zedbsd record=ws338-questions -->

# WS338 ユーザーへの問い（p001、2026-11-15 P1）

WS338: libmedia の H.264 の software の decoder を速くする（Pi 5 で 1080p30 の High を 4 thread で 40 fps 以上）。状態: p001（設計）の最中。設計は [design.md](design.md)。
決まった事（問いから外した）: userland の program と shared library を -O2 -Wall -Wextra -Werror で build する（2026-11-15 のユーザーの決定、design.md §1）。

## Q338-1 decoder の SIMD の扱い

- 背景: WS329（encoder）では 2026-11-14 に SIMD を範囲の外にした。WS338 の ws.md には 11-01 の答え（compiler の報告を見て C を整える → short vector 型）に合わせるという古い記録が残っている。
- 用語: SIMD は 1 命令で複数の画素を処理する CPU の機能（arm64 の NEON、x86 の SSE2）。「自動の vector 化」は普通の C を compiler が SIMD の命令にすること。「short vector 型」は C の拡張で SIMD の値を型として書くこと（機種ごとの命令の名前は書かない）。
- 案:
  - **A（推し）**: 普通の C のまま、compiler の自動の vector 化が効く書き方にする（各 Phase で compiler の報告と block の幅を見て確かめる）。short vector 型・intrinsics は使わない。Pi 5 の実測（p010）で 40 fps に届かない時だけ、short vector 型の Phase を足すかを改めて聞く。
  - B: 11-01 の形の 2 段（自動の vector 化 → short vector 型）を最初から計画に入れる。
  - C: SIMD を一切考えない（vector 化の確かめもしない。普通の C の書き直しと thread だけ）。
- 推しの理由: 見積もり（-O2・C の書き直し・4 thread）で 40 fps に届く見込みが大きい（推定 Pi 5 で 42〜53 fps、実測は p003）。vector 型は機種ごとの試験と Windows の build を増やす。A は ANSI C のままで 3 機種に効く。
- A の弱み: compiler の自動の vector 化は block の幅が小さい（4・8 画素）所で効かないことがあり、手で書く SIMD より効き目が小さい。

## Q338-2 5330 の数の扱い

- 背景: 5330（Latitude 5330、Alder Lake）は GPU の Vulkan Video で H.264 を decode できるので、この software の decoder は予備の道。WS326 p024 の案は「1080p60 の High を 2 thread 以内」。
- 案:
  - **A（推し）**: 5330 は 1・2・4 thread の数を測って記録するだけで、WS の完了の条件にしない。
  - B: 1080p60 の High を 2 thread 以内で 60 fps 以上を完了の条件にする。
- 推しの理由: 5330 の再生は hardware の decode が主。Pi 5 の 40 fps を満たす作りなら、core の速い 5330 は 1080p30 の予備として足りる見込み。

## Q338-3 Pi 5 の 40 fps を判定する動画

- 背景: 「1080p30 の High」でも bit rate で重さが変わる（entropy の decode の量が bit rate にほぼ比例）。WS326 p024 は「8 Mbps 級（x264 の標準の設定、B frame・CABAC・8x8 変換）」と定めた。手元の実 stream B は 13 Mbps で、host で 1.3〜1.5 倍重い。
- 案:
  - **A（推し）**: 判定は試験の道具が作る 8 Mbps の High（high-1080-grain、x264 medium、粒の有る絵）で 40 fps 以上。13 Mbps の実 stream B も測って記録し、目安にする（判定にしない）。
  - B: 両方で 40 fps 以上を判定にする（より厳しい。届かなければ picture の重なりか SIMD の Phase が要る見込み）。
- 推しの理由: 配信・録画の 1080p30 の多くは 8 Mbps 以下。B は高い bit rate の録画も落とさない保証になるが、作業が増える。
