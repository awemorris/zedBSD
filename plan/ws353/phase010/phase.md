<!-- awesome-plan project=zedbsd record=ws353-p010 -->

# ws353-p010: shown の thread（present の完了で callback・animation）と、client の buffer を slot から外す

Status: planned
Parent: [WS353](../ws.md)。設計 [design.md](../design.md) §5.1・§5.8・§5.9、対応表の 2-12・2-13。

## 目的

1. 新しい thread shown（T17）が window mode の frame の present の fence（p009）を待ち、main に SHOWN を知らせる。frame の callback・animation の一歩・head の missed の want し直しをそこから行う（合意した形の「frame の callback は present の完了で」）。
2. client の GPU の buffer の返しを、合成の slot から外し、render の REAPED の serial で決める（ユーザーの決定 5）。slot の `held[]`・`callbacks` を消す。

T17 を game mode の present の lane（T4、scanout.c）に載せない: display の thread が game chain を壊す前の `kwl_scanout_present_wait` と lane の FIFO が window mode の待ちに引きずられるため（設計 §5.1、レビュー S2）。

## 変える file

- `render-thread.c`・`render-thread.h`: T17（`KWL THREADS` に `shown`）。present の fence の輪（空きの list、lock。空きが無ければ `vkCreateFence` で足し待たない）。render は `vkQueuePresentKHR` に `VkSwapchainPresentFenceInfoEXT`（output と載せた head ごと）を付け、present の後に「frame n、(swapchain, fence) の列」を T17 に渡す。swapchain ごとの結果が OOM なら fence を空きに戻し、SHOWN(n, swapchain, 失敗) を render が自分で積む。OUT_OF_DATE・SURFACE_LOST は T17 に渡して結果を添える（レビュー L2）。T17 は飛んでいる fence があれば swapchain ごとの最古を any で `vkWaitForFences(UINT64_MAX)`（遅い head と output が互いを待たない、レビュー L4）、返った物ごとに SHOWN を出口に積み wake、fence を reset して空きに戻す。無ければ自分の cond。停止は render の後で、残りの fence を待ち終えてから終わる（打ち切り無し）。
- `compose.c`・`compose.h`・`kwl.h`: frame ごとの記録（番号、callback の列、表示した head）。output の SHOWN(n) で callback を送る（present の失敗でも送る）。head の SHOWN で、その head の missed を want し直す。slot から `held[]`・`callbacks` を消す。
- `objects.c`・`compose.c`・`kwl.h`: buffer の `read_serial`（frame と layer の焼きで sample した時に上げる）、main の `done_serial` と返し待ちの list。`holds` は protocol・current・scanout（game mode の lend、scanout.c:420〜425）の分だけ（レビュー L9）。返しは「置き換わった」かつ `read_serial ≤ done_serial`。`done_serial` は REAPED だけで上がる（render が submit の順に待つので、GPU の job の順の前提は要らない）。去った client の buffer の解放も同じ条件に serial を足す（歩調は p005 の形）。
- `shm.c`: 写しの条件を「その host image の staging を読む slot が飛んでいない」に狭め、待つ時は REAPED で続ける。
- animation（p008 で REAPED にした物）を output の SHOWN へ。

## 先に確かめる事

- p011 が入っている事（REAPED が GPU の完了の直後に来る。来ないと返し待ちの list が idle で残る）。

## 残す振る舞い

- `wl_buffer.release` と frame の callback の意味（protocol は変えない）、shm の「写したらすぐ返す」、game mode の present の lane（T4）と `kwl_scanout_present_wait`。

## 確かめ

- build warning 0。host: 返し待ちの list と serial の試験（置き換え・同じ buffer の再 attach・client の切断・present の失敗・layer の焼きが読んだ buffer）、T17 の試験（偽の fence で、output と遅い head、OOM の present、停止の時に残りの fence）。
- QEMU: FIFO の Vulkan の client（vkcube の類）と shm の client が止まらず回る、`--log-frames` の RELEASE が読みの完了（REAPED）の後、callback が present の後、client の切断で buffer が残らない（ASan の build があれば）、game mode の出入りと出力の移動・回転で T17 が止まらない。idle で T17 は起きない。

## 受け入れ

- slot に client の buffer と callback が無い。上の判定が PASS。結果・SHA・未実施を書く。

## 依存

- p009、p008、p011。
