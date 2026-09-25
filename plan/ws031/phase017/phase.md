# WS031 p017 計画: compiler の未実装機能

Phase disposition: canceled（2026-09-23、`plan/ws031/ws.md`「残課題のブレークダウン」の子Phaseへ分割。確認項目・範囲の正本はこの文書に残す）

ユーザー決定（2026-09-23）: p014 の後回し一覧のうち SPIR-V compiler（`src/drivers/gpu/i915/compiler/`）の未実装機能を本 Phase に
まとめ、計画だけ作って実行は後回しにする。

## 範囲
- 型: 整数の varying（`Flat`）と整数の頂点属性、16 bit・64 bit の整数と浮動小数（`Int16`/`Float16`/`Int64`/`Float64` capability）。
- 値の形: local の配列・構造体、local vector の動的 index（`OpVectorExtractDynamic`、動的 access chain）、行列の `OpPhi`、
  ループ内で初めて store してループ外で読む local（今は escape として拒否）。
- 制御: `OpSwitch`、関数呼出し（inline 化で可）、ループ内の return。trip count 0 の channel が body を 1 回（無効化されて）通る形の解消。
- 命令の並べ方: SWSB（software scoreboard）を全命令直列から依存に基づく指定へ。命令の並べ替え（latency hiding）。
- spill の改善: 定数の再生成（rematerialization）、cost 重み付き victim 選択、spill 1 つごとの再 lower をやめる。
- 終わらないループの扱い: compiler は検出できないため、GPU hang からの回復（WS031 の範囲外なら理由と転送先を記録）。

## 方針
各機能を GLSL の試験 shader（glslc で offline に SPIR-V、`regenerate.py` で CPU 期待値）と実機 scenario（`vke2` 系の拡張か新設）で
確認し、host の IR interpreter・EU model・gentool（Mesa 25.0.7 の assembler/disassembler）でも照合する。既存 kernel の出力が変わる
場合（SWSB など）は vkdemo の offscreen hash を含めて理由を記録する。

## 受け入れ
範囲の各機能がそれぞれ実機 scenario で PASS、host 試験・gentool PASS、統合回帰 1 回。

## 見積
6〜8 日（SWSB と 64 bit が大きい）。
