# WS031 p018 計画: 性能の構造改善

Phase disposition: canceled（2026-09-23、`plan/ws031/ws.md`「残課題のブレークダウン」の子Phaseへ分割。確認項目・範囲の正本はこの文書に残す）

ユーザー決定（2026-09-23）: p014 の性能第 2 回（`plan/ws031/phase014/phase.md` §性能 第 2 回）で見つかった構造上の制約を本 Phase に
まとめ、計画だけ作って実行は後回しにする。現状: mview `--spin=30`（640×480、37k 三角形）で no-vsync 190 fps・5.25 ms/frame、
vsync 60 fps。GPU 自体の描画は 1.3 ms/frame。

## 範囲
1. **非同期 executor**: vkQueueSubmit を queue して即座に返し、fence・semaphore は GPU 完了時に signal する。state/batch heap の多重化、
   command buffer と資源の lifetime 管理。1 frame に直列の GPU round trip 3 回（render・libvulkan の present copy・zwl の拡大 copy）を
   重ねられるようにする。見込み frame ≈ max(CPU, GPU) ≈ 3 ms 台。
2. **worker の完了待ち**: CSB の udelay(50 µs) busy-poll をやめ、GT の user interrupt と waitq で待つ（その CPU を他の thread に渡す）。
3. **scheduler の wakeup**: CPU 固定の thread で、同じ CPU への wakeup が次の `kern_preempt_enable` まで待つ性質（1〜5 ms）への対処。
   kernel 全体の方針に関わるため、WS031 の単一目標（i915 native Vulkan 実行器）に収まるかを計画時に判断し、収まらなければ新 WS へ
   分ける（`plan/AGENTS.md` の単一目標の規則）。
4. **frame の copy**: libvulkan WSI の swapchain image → export 用 shared image の毎 frame copy（WS014 の設計）を、i915 では aliasing で
   省く（Venus と共通の libvulkan のため driver 能力で分岐）。zwl の 640×480 → 1920×1080 拡大 copy を plane scaler か zero-copy flip で省く。
5. **present mode**: vsync の有無を present mode で選ぶ（p015 の小修正と重なる。p015 で済めば本 Phase では扱わない）。

## 方針
各段階で `MVIEW_ARGS=--spin=30` の fps と `MVIEW STAGES`・`i915: perf:` の内訳を記録し、正しさは capture display の 6 検査・vkdemo
offscreen hash・`vkx`/`vkc`/`vke1`/`vke2` で確認する（最後に統合回帰 1 回）。

## 受け入れ
非同期 executor と割込み待ちで no-vsync の frame 時間が短縮され（目標 3〜4 ms、300 fps 前後）、vsync 60 fps を維持し、
正しさの確認が PASS。scheduler の扱いの判断（本 Phase か新 WS か）が記録される。

## 見積
5〜8 日（非同期 executor が中心）。
