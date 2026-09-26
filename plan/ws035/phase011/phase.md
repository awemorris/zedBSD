<!-- awesome-plan project=zedbsd record=ws035p011 -->

# ws035-p011: zdesktop — `zwl` の改名と基本のウィンドウ管理

Phase ID: `ws035-p011`
Parent: [WS035](../ws.md)
Status: **uncleared**（q323-i04、2026-09-23。実行中に前提の欠落が判明したため）
Phase disposition: canceled（2026-09-27。改名はユーザー指示で [ws035-p073](../phase073/phase.md) が行う。基本の窓の管理（focus・移動・リサイズ・z-order・最小化・最大化）は p059・p062〜p072 で実装済み）
Queue: q323（q323-i04）
実行: メインセッション

## 範囲（計画時）

`userland/base/zwl` を `/bin/zdesktop` へ改名し、基本のウィンドウ管理
（focus、移動、リサイズ、z-order、最小化・最大化）を実装する。

## 判明した前提の欠落

着手して現状を調べた結果、**この Phase は合成（compositing）の仕組みを前提にしており、
それが存在しない**ことが分かった。計画の依存は `p004`（bootヘッダ移動）だけになっていたが、
実際にはその前に設計と実装が要る。

### 今の zwl は合成をしていない

`userland/base/zwl/display.c` の `zwl_present()` は、クライアントが渡した画像を
**そのまま scanout に出している**。

```c
present.handle = buffer->image.handle;   /* クライアントの画像そのもの */
present.flags  = GPU_DISPLAY_PRESENT_FIFO | GPU_DISPLAY_PRESENT_BLOB;
ioctl(server->gpu, GPU_DISPLAY_PRESENT, &present);
```

`struct zwl_server` が持つのは `front`（今出している buffer）と `front_surface` の2つだけで、
**コンポジタ自身の描画先が無い**。画面に出せるのは常に1つのクライアント画像の全画面である。
これは WS014 p006 以降「1パス1surface同期present のテストドライバ」として
ユーザーが承認した範囲であり、欠陥ではなく設計どおりである。

### ウィンドウ管理に何が要るか

focus・z-order・最小化・最大化は「どの surface を front にするか」の選択なので、
今の構造の延長でも作れる。しかし**移動とリサイズは作れない**。任意の位置・大きさの窓を
同時に画面へ出すには、コンポジタが自分の画像を持ち、各クライアントの画像をそこへ
**blit して合成**したうえで、その1枚を scanout する必要がある。

`/dev/gpu0` の UAPI にある描画手段は `GPU_COMMAND` / `GPU_COMMAND_SUBMIT` で、
中身は Venus のコマンド（Vulkan の構造体）である。つまり合成するなら zdesktop は
**Vulkan クライアントになる**（`/lib/libvulkan.so` を使い、クライアントの画像を
`VK_KHR_external_memory_fd` の OPAQUE_FD で import し、自前の swapchain 画像へ blit する）。
今の zwl は libvulkan を一切使っていない（7ファイルとも参照0）。

WS014 p007 が libvulkan に入れた「描画node＋表示nodeの組」と OPAQUE_FD の共有が
まさにこの土台だが、**zdesktop 側でそれを使う設計は書かれていない**。

### 決めるべきこと（設計の対象）

1. 合成の方法: Vulkan で blit するのか、GPU に 2D の合成経路を足すのか。
2. クライアント画像の import 経路と、その寿命・同期（今の `GPU_RESOURCE_IMPORT` と
   OPAQUE_FD のどちらを使うか）。
3. 直接 scanout の高速経路を残すか。全画面1枚のときは今の経路の方が copy が無い。
   WS014/WS031 の実機受入（`direct`・`wayland` の各VM）はこの経路を通っている。
4. damage の扱い。毎フレーム全画面を合成し直すのか、変わった矩形だけか。
5. 窓の装飾（p025 が担当）との境界。移動・リサイズの当たり判定を誰が持つか。
6. `wl_surface` の座標系と `xdg_toplevel` の configure。今は全画面固定である。
7. `/dev/graphics`（[ws035-p020 の設計](../graphics-design.md)）との関係。
   zdesktop が GPU を持つとき、`/dev/graphics` は同時に使えない（設計 §5.1 の調停）。

## この Phase で行ったこと

調査のみ。**ソースは変更していない。** 改名も行っていない。
部分的に改名だけ先に済ませると、設計がファイル構成を変える場合に二重の手間になるため。

## 引き継ぎ

Queue 完了後に WS035 の計画を直す。提案は次のとおり。

- **新規 Phase（設計）** を p011 の前に置く: 「zdesktop の合成と窓の座標系の設計」。
  上の1〜7を決め、敵対的レビューを付ける。成果物は文書のみ。
- p011 は改名と、その設計に沿ったウィンドウ管理の実装とする。依存に新しい設計 Phase を加える。
- p025（タイトル・フレーム描画）、p012（X11）、p013（タスクバー）、p014（タイル表示）は
  いずれも p011 の後のままでよい。
- WS031 の p027・p028 が `zwl` のパスを参照している点は変わらない。改名はその前に行う。

## 受け入れ（未達）

計画時の受け入れ条件には到達していない。改名もウィンドウ管理も行っていない。
