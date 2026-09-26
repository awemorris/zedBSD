<!-- awesome-plan project=zedbsd record=ws035p060 -->

# ws035-p060: Vulkan で描く Wayland の client（mview）を glass の窓で

Phase ID: `ws035-p060`
Parent: [WS035](../ws.md)
Status: cleared（q461-i01、2026-09-26）
Phase disposition: normal
Queue: q461-i01
承認: 2026-09-26 ユーザー「3Dモデルを表示するサンプルアプリがbaseにあるので、WaylandクライアントがVulkan描画できるか試してみてください。」

## 目的

base の 3D model viewer（`userland/base/mview`、標準の Vulkan と Wayland の WSI で描く）が、`zwl --glass` の窓として Vulkan で描けることを確かめる。mview は全画面しか求めないので、窓で開く option を足す。

## 範囲

- mview に `--windowed`（fullscreen を求めない）と `--size=WxH` を足す。
- `zwl --glass` の上で mview を窓で開き、model が窓に描かれ、title bar が付くことを画面で確かめる。drag（mview の orbit）と最大化（resize）、`--spin` の frame rate も見る。

## 受け入れ

1. mview の窓に model が描かれる（画面を撮ってユーザーに見せる）。
2. 最大化で mview が新しい大きさで描き直す。窓の中の drag で model が回る（title bar ではなく client に届く）。
3. build は warning 0、変えた C は style-check の指摘 0、p059 の試験が通る。

## 結果（q461-i01、2026-09-26、QEMU の Venus guest（host の Lavapipe）。実機は未実施）

- mview に `--windowed`・`--size=WxH`（64〜4096、既定 640x480）を足した（`main.c`、`window.c`、`mview.h`、README）。
- `zwl --glass` で `mview --windowed --size=720x520`: 窓（312,215）に model（qs40、37000 三角形、texture 13）が Vulkan で描かれ、浮いたタイトルバー「Model viewer」が付いた（`build/ws035-p060/mview.png`）。
- 窓の中の左 drag（QMP）で yaw 80°・pitch -15° に回った（mview の log。client に届いた）。最大化で configure 1256x690 → mview が描き直し、縦横比も正しい（`build/ws035-p060/maximized.png`）。
- `--spin=15` の frame rate: 窓 720x520 で 8.80 fps（平均 112.7 ms）、全画面 1280x800 で 8.59 fps（115.8 ms）。Lavapipe（host の CPU）の描画が律速で、窓の合成による差は見えない。
- 変えた行の style-check の指摘 0、build は warning 0。p059 の試験 PASS。boot test は未実施（mview だけの変更で、起動に関わらない）。
