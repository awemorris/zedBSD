# WS014 p005: 標準Vulkan APIへの訂正結果（q308）

2026-09-13 JST。`userland/base/vkdemo/` は標準Vulkan1.0とdirect-display WSIだけを使い、`/lib/libvulkan.so` へ動的linkするアプリに訂正した。公開headerは `libc/include/vulkan/`、libraryの所有WSはWS030。アプリにGPU ioctl/device-node/Venus codecを置かない。EGLは今回cancel。

最終 `q308-lifecycle-003` で6枚のテクスチャ付き回転直方体を実VNC/GPU readback/独立oracleで照合し、不一致0。通常終了・再起動、SIGINT後の再起動、実console復帰、別processの表示競合拒否も通過した。source/対象build/155公開API、shader/CLI/portable offscreen試験を確認した。

詳細は [WS030 q308結果](../../ws030/results-q308.md)、[最終検証](../../ws030/phase004/final-evidence/verification.json)、[実画面](../../ws030/phase004/final-evidence/vkdemo.png) を参照。q307の旧Venus-client結果は `results.md` / `evidence/` とQueue履歴に保持し、今回の標準API結果へ書き換えない。

p005の訂正をclearし、p004へ最終API・U/K・MMIO/WSI/console契約と結果を引き渡す。p004はplanning/未queue、WS014はincomplete、native i915は別WS029。git add/commit/pushはユーザー所有。
