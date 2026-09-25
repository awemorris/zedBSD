<!-- awesome-plan project=zedbsd record=ws035p020-review -->

# ws035-p020 敵対的レビュー: `/dev/graphics` 共通層の設計

対象は [graphics-design.md](../graphics-design.md) の初版。
探したのは、設計の誤り・欠落・矛盾と、実装に入ってから初めて破綻する箇所である。
**8件を指摘し、7件を設計へ反映、1件を制限として記録した。**

## R1: GPU backendが `CAP_GLYPH` を落とすと xzed が起動しなくなる（重大）

初版の §5.3 は、GPU backendが `get_glyph` を持たず `CAP_GLYPH` を落とすとしていた。
一方 `userland/X11/xzed/main.c:468-475` は、

```c
if (ioctl(s->graphics, KERN_GRAPHICS_GET_CAPS, &caps))
	...
if (!(caps.capabilities & KERN_GRAPHICS_CAP_FLUSH) ||
    !(caps.capabilities & KERN_GRAPHICS_CAP_GLYPH) ||
    !(caps.capabilities & KERN_GRAPHICS_CAP_BLIT_RGB24)) {
```

と、3つのcapabilityを**必須**として検査し、欠けていれば起動しない。
GPU backendが優先度で勝つ機種では、p024の適用と同時にxzedが動かなくなる。
WS035 p012（zdesktopへのX11サーバ移植）はxzedのコードを前提にしているので、これは実害である。

**対応**: `get_glyph` を backend の仕事から外し、**共通層の仕事**にする。
glyphの取り出しはフォントの表を引くだけで、機種の出力とは関係がない
（現に `src/drivers/platform/pcat/graphics/vgafont.c` は機種に依存しない8x16のビットマップである）。
共通層が常に `CAP_GLYPH` を提供すれば、どのbackendでもxzedが動く。
p005で `vgafont.c`・`font.c` を `src/drivers/generic/` へ移す。pc98の `display-glyph.c` は
GDCのフォントROMを読む別物なので、backendの任意機能として残し、あれば優先する。

## R2: `/dev/graphics` のGPU backendが自分自身を `EBUSY` で弾く（重大）

§5.1 で `GPU_DISPLAY_CLAIM` が `drv_graphics_display_acquire(GPU)` を通るとした。
§5.3 では `/dev/graphics` のGPU backendが `enter` でscanoutを確保するとした。
このbackendは内部で `GPU_DISPLAY_CLAIM` 相当の処理を行う。
しかし `enter` の時点で共通層は既に所有者を `GRAPHICS` にしているので、
backendの内部claimが `acquire(GPU)` を通ると `EBUSY` になり、**自分で自分を弾く**。

**対応**: `acquire`/`release` は**cdevのUAPI入口だけ**で呼ぶと定める。
`GPU_DISPLAY_CLAIM` ioctl と `/dev/graphics` の `ENTER` の2か所である。
backendの内部でscanoutを取る経路は、GPUドライバ内部のlease APIを直接使い、調停器を通らない。
設計に「調停器はUAPIの入口だけが触る」と明記する。

## R3: GPUのfault・session終了から `release` を呼ぶと、text layerを危険な文脈で触る（重大）

§5.2 は「session終了・device faultの回収で `drv_graphics_display_release(GPU)`」とした。
GPUのfault処理は watchdog と monitor から走り、GPUの内部lockを保持している
（`src/drivers/gpu/gpu.c` の `stop_begin`/`isolate`/`fault` 経路）。
`release` は `kern_text_resume()` を呼ぶので、text layerのlockとdisplay driverの描画へ入る。
**GPUのlockを持ったままtext layerへ入る**形になり、順序が定まらない。

**対応**: 設計に次を加える。

- `drv_graphics_display_acquire`/`release` は**sleepできる文脈からのみ呼ぶ**。
  割り込み・watchdog・spinlock保持中からは呼ばない。
- GPUのfault経路は `release` を直接呼ばず、所有していたsessionの `close`（file解放）で呼ぶ。
  GPUのdevice lossは既に「新しい投入を拒む」形で表現されており、画面の所有は
  sessionが閉じるまでGPUが持ったままでよい。実際、q313までの実測でも
  consoleが戻るのはprocessが終了して fd が閉じたときである。

## R4: 共通層のmutexとGPUのlockでABBAになりうる（重大）

`/dev/graphics` の `ENTER` は共通層のmutexを取ってからbackendの `enter` を呼ぶ。
GPU backendの `enter` はGPUのlockを取る。
一方 `GPU_DISPLAY_CLAIM` はGPUのlockを取ってから `acquire()` で共通層のmutexを取る。
**逆順**であり、ABBAが成立する。

**対応**: lockの順序を設計に書く。**共通層のmutex → GPUのlock** の一方向だけを許す。
`GPU_DISPLAY_CLAIM` は GPU のlockを取る**前に** `acquire()` を呼ぶ。R2の「UAPIの入口だけ」と
合わせると、入口で調停器を取り、その後で下位のlockへ降りる形に揃う。

## R5: shutdownがGPU所有の画面をtextへ戻せない（欠落）

`drv_graphics_device_restore_text()` は shutdown 経路（`src/kern/shutdown.c`）が呼ぶ。
初版の共通層は `/dev/graphics` の `entered` しか戻さない。所有者がGPUのときは何もしない。

**対応**: `restore_text()` を「所有者が誰であれtextへ戻す」に定義し直す。
GPU所有のときは、GPUドライバへ登録された**強制解放のcallback**を呼ぶ。
`struct drv_graphics_ops` とは別に、調停器が持つ1つの関数ポインタとする。

## R6: p005のboot-testは共通層の回帰確認にならない（受け入れ条件の誤り）

初版は「`/dev/graphics` はconsoleの下にあるので、login promptが出ることが共通層の回帰確認になる」
と書いた。**これは誤りである。** consoleは `kern_text_ops`（`src/drivers/generic/console.c` と
各機種のtext.c）を通り、`/dev/graphics` を経由しない。`/dev/graphics` は `ENTER` されたときに
`kern_text_suspend()` を呼ぶだけで、login promptの表示はこのcdevを一切使わない。
login promptが出ても、共通層が壊れていないことの証拠にはならない。

**対応**: 受け入れ条件を次の2つに分ける。

1. **host fixture**: 偽backendに対して、ioctl 11個の引数検査、所有権（2つ目のopenが `EBUSY`）、
   `ENTER` 前の描画ioctlの拒否、`close` での `leave`＋`resume`、capability不一致の登録拒否、
   backend差し替えの延期、調停（`GRAPHICS` と `GPU` の相互排除）を確かめる。
2. **実QEMU**: `/dev/graphics` を実際に使う小さな試験プログラムを rootfs に入れ、
   `ENTER` → `FILL_RECT` → `FLUSH` → `close` を行わせ、**その画面をscreendumpで撮って**
   塗った矩形が出ていることと、closeの後にlogin promptへ戻ることを確かめる。
   boot-testはその後段のconsole復帰の確認として使う。

## R7: `priority` の導入はp005の範囲を超える（矛盾）

§4.3 は登録時の `priority` で backend を選ぶとした。しかし §6 の p005 の手順3は
「`backend.c` を `struct drv_graphics_ops` で登録する形に直す」だけである。
pcatの `backend.c`（1166行）は boot framebuffer・Cirrus・標準VGA の選択を**自分の中で**行っており、
priorityで選ばせるにはこれを3つの登録に割る必要がある。手順3の記述と実際の作業量が合わない。

**対応**: p005 では **1 platform = 1 登録**とし、機種内の選択は今どおり `backend.c` の中に残す。
`priority` は「後から来るGPU backendが機種backendより強い」ことだけを表す2段階でよいので、
`priority` は残すが、p005では機種backendが固定値、p024でGPU backendが高い値を使う、と明記する。
`backend.c` を分割するかどうかはp005の範囲外とする。

## R8: `.vfs_bss` セクションの扱い（欠落、制限として記録）

今の `pcat-graphics.c` は、`graphics_owner`・`graphics_mode`・`row_buffer`（4096 B）・
`palette_buffer`（1 KiB）・`graphics_lock` を `__attribute__((section(".vfs_bss")))` に置いている。
`/dev/graphics` の登録が `src/kern/vfs.c:349` から起きるためで、
`platform/amd64/vmunix.ld:49` がこのセクションを通常の `.bss` と同じ場所へ集めている。
共通層へ移すときにこの属性を落とすと、リンク配置が変わる。

**対応**: 設計に「共通層の静的変数は `.vfs_bss` のままにする」と書く。
なぜこのセクションが要るのかは今の記述からは読み取れないので、p005で確かめ、
不要と分かれば落とす（分からなければ残す）。**これは制限として残し、設計では変えない。**

## 反映しなかった指摘

無し。R8のみ「設計では変えず、p005で確かめる」形にした。

## レビュー後の設計の状態

R1〜R7 を [graphics-design.md](../graphics-design.md) へ反映した（§4.2・§4.3・§5.1・§5.2・§5.3・§6・§7）。
R8 は §7 の制限へ加えた。
