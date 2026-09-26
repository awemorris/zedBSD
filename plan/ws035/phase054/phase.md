<!-- awesome-plan project=zedbsd record=ws035p054 -->

# ws035-p054: acquire fence（`zed_gpu_buffer_v1` の拡張）

Phase ID: `ws035-p054`
Parent: [WS035](../ws.md)
Status: uncleared（q459-i01、2026-09-26。受け入れ 1・2・4 と 3 の前半は満たした。3 の「present が速くなる」は測って速くならず、判断待ち）
Phase disposition: normal
Queue: q459-i01
設計: [compositing-design.md](../compositing-design.md)（2026-09-25 承認）の D3（client の描画の完了）

## 目的

GPU の画像の client が、描画が終わる前に commit できるようにする。`zed_gpu_buffer_v1` に、commit と一緒に acquire fence（`KERNEL_HANDLE_FENCE` の fd）を渡す request を足す（Linux の `linux-explicit-synchronization` 相当を最小限で）。zdesktop はその fd を poll に入れ、signal されてからその commit を使う（CPU で待たない）。fence を渡さない client は今まで通り（commit の時点で描画が終わっている前提）。

## 受け入れ

1. 描画中に commit しても、描き終わった画像だけが出る（画面の読み取り）。fence が signal されるまで、その commit は出ない。
2. zdesktop は fence を CPU で待たない（poll）。signal されない fence の client は、その commit が出ないだけで、他の窓の描画は止まらない。
3. libvulkan の Wayland の WSI は、present で描画の完了を CPU で待たず、fence を渡す（client の present が速くなる: 測定）。
4. 新しい C は `plan/coding-style.md` の全文に従い、style-check の指摘 0。amd64 の build は warning 0。boot test。p052・p053 の試験が通る。

## 設計（実装の前に決める点）

- protocol: `zed_gpu_buffer_v1` の version 2 に `set_acquire_fence(surface, fd)`（次の commit に付く）を足すか、surface ごとの object にするか。今の protocol の定義（libwayland の `zed-gpu-buffer-v1`）と WSI の present の順序を読んで決める。
- zwl: commit の queued に fence の fd を付け、fd を poll に入れる。signal で `ready`。surface に待っている commit があるうちに次の commit が来たら、前の commit は捨てて（release）新しい方を待つ。
- WSI: present の信号の semaphore を fence に変え（または fence を別に作り）、`vkGetFenceFdKHR` の fd を渡す。

## 決めた設計

- protocol: `zed_gpu_buffer_v1` の version 2 に `set_acquire_fence(surface: object, fd, generation_hi: uint, generation_lo: uint)`（opcode 2、署名 `2ohuu`）を足した。surface ごとの object にはしない（WSI の present の順序は fence → attach → damage → commit で、factory の request 1 つで足りる）。fence は次の commit に付く。**1 つの commit に 4 つまで**付けられ、zdesktop は全部を待つ（WSI の fence と、別の producer の fence を両方付ける場合。試験の client もこれを使う）。fd は `KERNEL_HANDLE_FENCE`、generation はその fence の payload の世代（`GPU_FENCE_QUERY` の generation 0 で読める）。
- zwl（`userland/base/zwl`）: request で fd を zdesktop の GPU の open に `GPU_FENCE_QUERY` で確かめ（違う GPU・fence でない・generation 0 は protocol error）、surface の次の commit の fence に積む。commit で queued の画像の fence になる（buffer を attach した commit は前の fence を閉じて置き換え、attach しない commit は再利用する画像の fence を保つ）。`zwl_schedule`（と Vulkan の無い直接表示）は、fence が終わった（世代が進んだか、同じ世代が signal／error）commit だけを採る。待っている fence の fd は main の poll に入れ、CPU では待たない。surface の破棄で閉じる。`--log-frames` で待った commit は `ZWL ACQUIRED surface= waited_ms=` を出す。
- WSI（`userland/base/libvulkan`）: factory を min(広告, 2) で bind。present の worker は、全 target が GPU の画像で compositor が version 2 なら、job の fence（`KERNEL_HANDLE_FENCE` の fd と generation）を付けて**先に commit し**、その後で fence を待って job を片付ける（`present_early`、platform の op `commit_early`、`wayland_present_sync`）。version 1 の compositor には今まで通り完了を待ってから commit する。
- libwayland: `zed_gpu_buffer_v1_interface` を version 2 に、`zed_gpu_buffer_v1_set_acquire_fence`。

## 結果（q459-i01、2026-09-26、QEMU の Venus guest。実機は未実施）

試験: [zdesktop-p054.sh](../tests/zdesktop-p054.sh)。client a は新しい試験の client `acquire-fence-test`（`userland/base/tests/acquire-fence`。wltest の window・renderer を使い、10 frame 描いた後、自分で作った kernel の fence（`GPU_FENCE_CREATE`・`BIND`、後で `SIGNAL`）を次の commit に足して青の frame を present する）。client b は緑の wltest の窓で、描き続ける。

| 受け入れ | 結果 |
| --- | --- |
| 1. 描き終わった画像だけが出る | fence が signal されるまで a は赤のまま（画面の読み取り）、signal の後で青。zwl はその commit を 5991〜6034 ms 待った（hold 6000 ms）。3 回とも PASS |
| 2. CPU で待たない、他の窓は止まらない | hold の間も b の frame（1 秒で 10〜11）と zdesktop の合成（1 秒で 10〜12 frame）が進む。signal されない fence では a の commit は出ないまま、b は描き続ける |
| 3. WSI は完了を待たずに fence を渡す | WSI は version 2 の compositor へ fence を付けて先に commit する（上の 1 の試験で、WSI の commit は試験の fence より先に届き、両方を待った）。**present は速くならなかった**: `vkQueuePresentKHR` は変更前の libvulkan と同じ（400x300 の窓 200 frame、FIFO・MAILBOX で 1 回あたり 31〜35 ms、最大 60〜71 ms、どちらも約 9 frame/s）。present はもともと worker に job を渡して戻り、完了を待っていたのは worker の側だった。Venus/Lavapipe では worker が commit する時点で描画が終わっており、WSI の fence は signal 済みで届く。変わるのは「描画の完了 → commit」の worker の wait の分の遅延だけで、この環境では測れない |
| 4. 規約・build・boot・回帰 | style-check の指摘は変えた行と新しい file で 0。amd64 の build は warning 0（外部 package・llvm-source・noct の既存の warning を除く）。boot test PASS（`build/ws035-p054-boot/login.png`）。p052 PASS、p053 PASS |

作業中に見つけて直したもの:
- libc の `setvbuf(stream, NULL, _IOLBF, 0)` が EINVAL で失敗し、zwl の log が 4 KiB ごとにしか file へ出ていなかった（p052・p053 の試験で log を数える所が古い値を見ていた可能性がある）。buffer が NULL で大きさ 0 なら `BUFSIZ` で確保する（glibc・BSD と同じ）。[BUG-055](../../bugs/BUG-055.md)（resolved）。
- 最初の試験の方法（wltest の描画を host の `vkSetEvent` まで `vkCmdWaitEvents` で止める）は、この Venus の renderer では transport が止まって 10 秒の watchdog で GPU が reset された。試験は kernel の fence を使う方法に変えた（wltest には present の時間の計測 `WLTEST PRESENT` だけを残した）。
- 試験の client の競合: WSI は別の thread で commit するので、直前の frame の commit の前に試験の fence を送ると前の frame に付いた。`vkQueueWaitIdle`（present の job も片付ける）の後で送る。

## 要る判断

受け入れ 3 の「client の present が速くなる」は、present が元から描画の完了を待っていなかったので満たせない。受け入れ 3 を「WSI は完了を待たずに fence を付けて commit する（present の時間は測って記録する）」と読み替えてこの Phase を cleared にするか、ユーザーの判断を待つ。
