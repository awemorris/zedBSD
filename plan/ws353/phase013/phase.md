<!-- awesome-plan project=zedbsd record=ws353-p013 -->

# ws353-p013: client の buffer の明示の同期（release fence）

Status: planned（未決 3 の決定の後。2026-11-13 q2056-i02 で推しを「完了を見てから返す（REAPED の serial）を当面の正の形、この Phase は要る理由が出てから」に変えた。設計 §8 の未決 3）
Parent: [WS353](../ws.md)。設計 [design.md](../design.md) §2.3・§2.4・§8 の未決 3。

## 目的

compositor が client の buffer の読みの終わりを待たずに返す形にする: 置き換わった buffer にすぐ `wl_buffer.release` と release fence（最後に読んだ job の fence の export）を送り、client（WSI）は fence を待ってから描く。p010 の返し待ちの list（REAPED の serial）は、release fence を出せない時の道としてだけ残る（全 render の backend で出せれば消す）。得は release が読みの GPU の時間（一つの frame の合成）だけ早く出る事。前提の kernel の作業（5 つの backend の `GPU_CAP_FENCE`）が大きいので、測った理由が出るまで着手しない事を推す。

## 手順

1. 調べ: i915・amdrdna・powervr・mali・videocore の render が `GPU_CAP_FENCE` を出さない理由（gpu.c の job の完了の bind は共通。backend の job・notification の経路で足りない物）。各 backend の変更の量と危険を Q1 に返す（ここで一度返す）。
2. kernel: 各 backend の `GPU_CAP_FENCE`（job の完了で fence を signal する bind が働く事を試験で確かめた物から）。
3. protocol: `kl_gpu_buffer_v1` rev 4（release の fence の event）。`libkeiland-backend-zedbsd/gpu-buffer-zedbsd.c`・libwayland の生成の header。
4. compositor: render が job の fence を export（OPAQUE_FD）し、置き換わった buffer に release と fence。
5. WSI（`libvulkan/wsi-wayland.c`）: release の fence を受け、acquire の時に待つ（または wait の semaphore として渡す）。rev 3 以前の compositor では今のまま。

## 残す振る舞い

- rev 3 の client（fence を受けない）には p010 の返し方。shm は今のまま。

## 確かめ

- build warning 0。QEMU（Venus）と、手順 2 で直した backend の実機（Q1 の割当、5330 なら i915）: FIFO・MAILBOX の client が止まらない、release の後に client が書いても表示が壊れない（読みの途中の書きが無い）、release の時刻が置き換えの直後。

## 受け入れ

- 手順 1 の調べを Q1 に返した上で、Q1 が決めた範囲の判定が PASS。結果・SHA・未実施を書く。

## 依存

- p010。未決 3 の決定。
