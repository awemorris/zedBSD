# WS031 p016 計画: executor の未実装機能

Phase disposition: canceled（2026-09-23、`plan/ws031/ws.md`「残課題のブレークダウン」の子Phaseへ分割。確認項目・範囲の正本はこの文書に残す）

ユーザー決定（2026-09-23）: p014 の後回し一覧のうち executor（`src/drivers/gpu/i915/render/`）の未実装機能を本 Phase にまとめ、
計画だけ作って実行は後回しにする。

## 範囲
- 描画先: mip level 0 以外・array layer への描画と attachment clear（今は level 0 のみ、XXX で拒否）。
- 複数の colour attachment（MRT）: render target の binding、`BLEND_STATE` の attachment ごとの entry、fragment の複数出力。
- blend: logic op、dual source（`SRC1_*`、今は blend off に落とす）。
- image view: format の読替え（`MUTABLE_FORMAT`）、component swizzle。image create flags と input/transient attachment の usage を
  format 特性の回答で照合。
- sampler: anisotropy、depth compare、border colour、unnormalized 座標。mirrored blit（今は ENOTSUP）。
- descriptor: 配列（`descriptorCount > 1`）、`vkUpdateDescriptorSets` の copy、vertex stage の sampled image（binding table を VS にも）。
- uniform buffer: push data 経由の 1 stage 1 KiB・8 block の上限を超える場合の dataport（constant cache / stateless）読み出し、
  同一 command buffer 内で GPU が書いた UBO を draw 間で読む順序（依存の検証）。
- tiling: 今は全 image が linear。Y-tile（または Tile4 相当）の optimal image と、その copy/blit/sampling の対応（性能にも効く）。

## 方針
各機能を正常系の実機試験 1 つで確認してから次へ（p014 と同じ進め方）。Mesa 25.0.7 genxml/anv を出典にして `intel/` へ転記。
変更で既存の出力（vkdemo・mview・`vkx`/`vkc`/`vke1`/`vke2`）が変わる場合は理由を記録する。

## 受け入れ
範囲の各機能がそれぞれ実機 scenario で PASS、host 試験 PASS、統合回帰 1 回。

## 見積
5〜7 日（tiling が最大）。
