# WS031 p015 計画: 準正常系・異常系の確認と小さな欠落の修正

Phase disposition: canceled（2026-09-23、`plan/ws031/ws.md`「残課題のブレークダウン」の子Phaseへ分割。確認項目・範囲の正本はこの文書に残す）

ユーザー決定（2026-09-23）: p014 の「後回しの確認・強化」を Phase に分けて計画だけ作り、実行は後回しにする。本 Phase は
その確認と小修正の部分。大きな未実装機能は p016（executor）・p017（compiler）、性能の構造改善は p018。

## 前提
- p014 の A0〜E3 と性能第 2 回が完了していること（`plan/ws031/phase014/phase.md`）。
- 試験は正常系で使ってきた経路で行う: `vkloop-hw.sh test <scenario>`（`vkx`/`vkc`/`vke1`/`vke2` の拡張か新 scenario）、
  capture display（`CAPTURE=...`）、host 試験（`run-vk-host-tests.sh`）。

## 確認（実機で 1 回ずつ、期待値は CPU 参照）
- 画像・sampler（p014 B の残り）: 幅 32 texel 未満の mip 付き image を描画先・blit 先にする（level 2 以降が 64 byte 境界でない）、
  非正方・奇数寸法（5×3 など）の sampling、16384 級の大 image の 15 level、`maxLod < minLod` の sampler、端数の自然 LOD。
- blend・UBO（E1 の残り）: `SRC_ALPHA_SATURATE`・`CONSTANT_ALPHA` 系、float target の blend、`vkCmdSetBlendConstants` 前の
  動的定数 draw（0 と log）、dynamic offset の範囲外（拒否）、block より短い UBO range（0 埋め）。
- compiler（E2/E3 の残り）: 0 除算・`INT_MIN / -1` の hardware 値を記録し仕様上の扱いを決める、商が整数に近い mod・FRem、
  ループ内の discard・texture sample、32 以上の shift、入れ子 8 超のループ（拒否）、入力を読まない fragment shader（SBE 属性 0）、
  SPILL と VIO16 を同じ command buffer で交互に描く、discard と spill の同居、per-thread 4 KiB 超の spill、spill した VS と PS の同時高負荷、
  sample の reply の一部が spill される形、scratch buffer 作成失敗時の draw の失敗経路。
- 異常系: 範囲外の index/offset、command buffer 65536 操作超（`VK_ERROR_OUT_OF_HOST_MEMORY`）、descriptor の上限超え、
  終わらないループ（GPU hang の扱いの記録。回復経路は WS031 の範囲外なら理由を書く）。
- mview: `R` 以外の視点の再現性（同じ入力列で同じ画像）、blend material を持つ model（test model で確認済みのものを qs40 系でも）、
  `--shading=pixel` の LCD 写真。

## 小さな欠落の修正
- uint8 index（`VK_INDEX_TYPE_UINT8_EXT` 相当の拒否か対応かを決める）、4 byte 非整列の `vkCmdCopyBuffer`、
  viewport/scissor の index > 0、負の viewport 高さ（`VK_KHR_maintenance1` の flip）。
- shader compile 失敗時に未公開 pipeline が漏れる件（既知 XXX）。
- discard の早期終了（HALT）: 全 channel が消えた thread を早く終える。
- vsync の有無を build option `I915_PRESENT_NO_VSYNC` ではなく present mode（FIFO / MAILBOX / IMMEDIATE）で選ぶ。libvulkan・zwl・
  UAPI の present flags で運べるかを確認し、運べない場合は UAPI 変更として事前に提示する。
- PS/2 keyboard の key が zwl に届かない件（p013、QMP 経由。USB keyboard は届く）: 原因を調べる。
- QMP `input-send-event` に `device` を付けると QEMU 10.0.11 が egl-headless で abort する件: 回避策の記録のみ（QEMU 側の問題）。

## 受け入れ
上の確認項目がそれぞれ PASS（または仕様として決めた結果が記録される）、小修正が実機で 1 回ずつ PASS、発見した不具合の修正、
最後に WS031 の統合回帰（p014 の回帰一覧）を 1 回。host 試験・build warning 0・`git diff --check`。

## 見積
2 日前後（確認 1 日、修正 1 日）。
