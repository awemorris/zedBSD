<!-- awesome-plan project=zedbsd record=ws083-p003a -->

# ws083-p003a: i915 の VCS の土台（engine record VCS0・worker の video の context・hang の封じ込め）

Status: cleared（2026-10-10 Q1 判定: 5330 の実機の p005・p006b と照合）（旧: cleared 候補（2026-10-10 P2 の照合: design §9 の受け入れ（§8.1 の 6 行目 PASS、vmunix の build warning 0）は 2026-10-07 に満たし、VCS0 の context の実機の bring-up も 5330 で確かめた。判定は Q1）（旧: in-progress（q833、P1。2026-10-07 実装と host 試験、実機は p005））
Disposition: normal
Parent: [WS083](../ws.md)

## 範囲

[design.md](../design.md) 第 3.1 版の §6.1 のうち wire・UAPI の要らない部分: engine record VCS0、worker の engine ごとの context の表と遅延の attach、`i915_worker_run` の engine の選択、video の hang の封じ込め（worker の側）。timeline は今のまま全部 RCS0（§4.3）。display/ と render/ は触らない（実行器の video の object・video 専用の batch・engine を引数に取る flush・session の quarantine の呼び出しは p003b）。

## 実装（2026-10-07 夕、P1）

- `request-queue.h`: `I915_ENGINE_VCS0 2`・`I915_ENGINE_COUNT 3`・`I915_CLASS_VIDEO`。`i915.c` の publish の loop は index 2 を VIDEO の class に（今までは「0 以外は COPY」）。`reset.c`・`session.c` の loop は 3 つを回る（VCS0 の context は open では record だけ、`session.h` の `contexts[]` も 3 に）。
- `worker.c`: `video_index`（GT の engine の VIDEO_DECODE の instance 0、無ければ −1）、`video_contexts[8]`、`video_dead`。record に `retained`。context の作成は `i915_worker_context_fill`（render と video の共通、engine の class が LRC の形を選ぶ）。新しい `drv_i915_worker_context_attach`（device の mutex の下、既にあれば 0、VCS0 の context だけ、GT に VCS0 が無ければ ENODEV、video が死んでいれば EIO、表が尽きれば ENOMEM）。`i915_worker_find` は engine で表を選ぶ。free の探索は retained を除く。`i915_worker_run` は RCS0 → render の GT engine、VCS0 → video の GT engine（無ければ ENODEV、死んでいれば EIO）、copy は ENOTSUP のまま。submit と `i915_worker_wait` はその engine の `ge`・`el`。VCS0 の request が wait で失敗（ETIMEDOUT・EIO）したら `i915_worker_video_hung`: `video_dead`、record を owner から切り離し retained（解放・再利用しない、live の数は残り worker は残る）、log `i915: video: request failed …`。context の作成の log は今の行の末に ` engine=<名>` を足した（前の行の形は保つ）。
- `worker.h`: `drv_i915_worker_context_attach` の宣言。

## 確認

| コマンド | 結果 |
| --- | --- |
| `sh plan/ws083/tests/run-host-vcs-worker.sh`（新、worker.c を組み込み GT を stand-in に） | plain・ASan/UBSan とも `ws083 vcs worker host test PASS`: engine の探し方、render の context は即 render の engine に image、video の context は attach まで record だけ（attach 前の run は EINVAL）、attach は VCS0 に 1 回だけ、render の context の attach は EINVAL、video の request は VCS0 の execlists へ・render の request は render へ、destroy で両方の image を解放、hang で video_dead・retained・owner 切り離し・destroy は解放しない・後の attach と run は EIO、VCS0 の無い GT で attach は ENODEV |
| `make -j16 BUILD=build/p1-k ZEDBSD_CONFIG=config/ci/config-amd64.mk build/p1-k/vmunix` | 成功、warning 0 |

未実施: 実機（VCS0 の context の LRC・空の batch の bring-up は p005）、session の quarantine の呼び出しと video の batch の保持（p003b、実行器の video submit の中）。

## 2026-10-10 の照合（P2、実機の結果で）

- 受け入れ（design §9 の p003a）: §8.1 の 6 行目（VCS の立ち上げの host、`run-host-vcs-worker.sh`）PASS、vmunix の build warning 0 → 2026-10-07 に満たした（上の表）。
- 実機: 5330 の実機（2026-10-10 Q1、image 588c5cd、boot に `i915.debug=video`、SSH。ws.md の p005・p006b の行）で dmesg に `engine[2] vcs0: … reset_domain=0x20` と `Vulkan video decode is offered on a GT with VCS0`、VCS0 の context の遅延の attach・LRC・execlists への submit が 6 本の stream の decode（I 3 本・P/B 3 本、全 frame 一致）で通り、hang・reset の行は無い。未実施に書いていた「VCS0 の context の LRC・空の batch の bring-up」は p005 の受け入れとして実機で満たした（design §8.3 の 1 行目の kernel の scenario の代わりに decode の batch で確かめた、Q1 の p005 の判定）。hang の封じ込めの実機は p007（人工の hang の F1・F2）。
- 588c5cd と照合の時の main（1b08d90b9）の間に `src/drivers/gpu/i915`・`userland/desktop/libvulkan`・`userland/tests/vkvideo-probe`・`src/kern/boot.c`・`include/uapi/gpu-op.h` の差は無い（`git diff --stat` が空）ので、実機の結果は今の code に当たる。host 試験（`plan/ws083/tests/run-host-*.sh` の 8 本: boot-video・libvulkan-native・libvulkan-status・libvulkan-video・mfx-avc・vcs-worker・video-roundtrip・vkvideo-probe）は 1b08d90b9 で全部 PASS（2026-10-10 P2）。
- 残り: 無し（この Phase の受け入れの条件は全部満たす。hang の実機は p007 の受け入れ）。
