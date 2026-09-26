<!-- awesome-plan project=zedbsd record=ws035p054 -->

# ws035-p054: acquire fence（`zed_gpu_buffer_v1` の拡張）

Phase ID: `ws035-p054`
Parent: [WS035](../ws.md)
Status: in-progress（q459-i01）
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
