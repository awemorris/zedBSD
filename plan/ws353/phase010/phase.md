<!-- awesome-plan project=zedbsd record=ws353-p010 -->

# ws353-p010: 表示の完了で callback・animation・client の buffer の返し（slot から外す）

Status: planned
Parent: [WS353](../ws.md)。設計 [design.md](../design.md) §5.8・§5.9、対応表の 2-12・2-13。

## 目的

T4（present の thread）が window mode の frame の表示の完了（`vkWaitForPresentKHR`）を待ち、main に SHOWN を知らせる。frame の callback・animation の一歩・client の GPU の buffer の返しをそこから行い、合成の slot の `held[]`・`callbacks` を消す（ユーザーの決定 5: client の buffer と合成の buffer を分ける）。

## 変える file

- `scanout.c`（今の present の lane の持ち主）: window mode の仕事「frame n、output と head の swapchain と present id」を受け、順に `vkWaitForPresentKHR(UINT64_MAX)`、SHOWN(n, 結果) を出口へ。game mode の present と同じ FIFO。
- `render-thread.c`・`compose.c`: present に `VkPresentIdKHR`、present の後に T4 へ積む。slot から `held`・`callbacks` を消す。
- `objects.c`・`compose.c`・`kwl.h`: buffer の `read_serial`、main の `done_serial` と返し待ちの list、frame ごとの callback の記録。`kwl_buffer_put` は protocol と current の hold だけ。返しは「置き換わった」かつ `read_serial ≤ done_serial`。`done_serial` は SHOWN（成功）と render の REAPED で上がる。去った client の buffer の解放も同じ条件（main.c:1115 の 1 pass に数個は残し、残りは自分の wake）。
- `shm.c`: 写しの条件を「その host image の staging を読む slot が飛んでいない」に狭め、待つ時は REAPED・SHOWN で続ける。
- animation（p008 で REAPED にした物）を SHOWN へ。head の missed は、その head の SHOWN で want し直す。

## 先に確かめる事

- 同じ queue の job が出した順に終わるか（libvulkan の queue と kernel の job の列）。崩れるなら止まって Q1 に報告（設計 §5.8 の前提）。
- present が失敗した frame の扱い（SHOWN は serial を上げない、callback は送る）。

## 残す振る舞い

- `wl_buffer.release` と frame の callback の意味（protocol は変えない）、shm の「写したらすぐ返す」。

## 確かめ

- build warning 0。host: 返し待ちの list と serial の試験（置き換え・同じ buffer の再 attach・client の切断・present の失敗）。
- QEMU: FIFO の Vulkan の client（vkcube の類）と shm の client が止まらず回る、`--log-frames` の RELEASE と callback の時刻が present の後、client の切断で buffer が残らない（ASan の build があれば）。idle で T4 は起きない。

## 受け入れ

- slot に client の buffer と callback が無い。上の判定が PASS。結果・SHA・未実施を書く。

## 依存

- p009、p008。
