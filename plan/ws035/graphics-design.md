<!-- awesome-plan project=zedbsd record=ws035-graphics-design -->

# `/dev/graphics` の共通層とGPU scanoutへの引き継ぎ（設計）

ws035-p020 の成果物。実装は p005（共通層、pcat・pc98をbackend化）と p024（GPU scanoutへの切替えと戻し）で行う。
**この設計ではソースを変更しない。**

対象読者は p005・p024 を実行する者。決めるのはkernel内部のAPIであって、`include/uapi/graphics.h` の
既存のUAPIは変えない（理由は §3）。

## 1. 現状の事実（2026-09-23 の実測）

### 1.1 `/dev/graphics` には共通層が無い

`drv_graphics_device_register()`（`include/kern/graphics-device.h`）はplatformごとに別々に実装されている。

| ファイル | 行数 | 役割 |
| --- | --- | --- |
| `src/drivers/platform/pcat/graphics/pcat-graphics.c` | 816 | cdev登録、11個のioctl全部、所有権、mode保持、palette・row bufferの一時領域 |
| `src/drivers/platform/pc98/graphics/pc98-graphics.c` | 814 | 同じ |
| `src/drivers/platform/pcat/graphics/backend.c` | 1166 | boot framebuffer、Cirrus GD5446、標準VGA |
| `src/drivers/platform/pc98/graphics/backend.c` | 479 | GDC、Cirrus、auto選択 |

`pcat-graphics.c` と `pc98-graphics.c` の差分は **88行**（`diff` の出力行数）で、残りは同じ内容である。
関数の並び（`graphics_open`・`graphics_close`・`require_entered`・`valid_rect`・`graphics_enter`・`graphics_get_modes`・
`graphics_fill`・`graphics_line`・`load_palette`・`graphics_blit`・`graphics_flush`・`graphics_glyph`・
`graphics_ioctl_locked`・`graphics_ioctl`）も、`KERN_GRAPHICS_*` の参照数（各27）も一致する。
**同じcdevを2回書いている**のが現状で、機種が増えるたびに3つ目が増える。

backendの関数表は宣言だけで、`struct` にまとまっていない。名前にplatformが入っている
（`drv_pcat_graphics_backend_enter` と `drv_pc98_graphics_backend_enter`）ため、共通層から呼べない。

rpi4・sun4u・x68k には `/dev/graphics` が無い。

### 1.2 画面の所有者が2系統あり、互いを知らない

| 系統 | 所有権 | textの扱い | 誰が使うか |
| --- | --- | --- | --- |
| `/dev/graphics` | `graphics_owner`（`struct file *`）と `graphics_entered`。openは1つだけ | `KERN_GRAPHICS_ENTER` で `kern_text_suspend()`、closeまたは `drv_graphics_device_restore_text()` で `kern_text_resume()` | `userland/X11/{zwm,xzed,zterm}`、noctのbeui |
| `/dev/gpu0` の display | `GPU_DISPLAY_CLAIM` が返す `lease`（`include/uapi/gpu-display.h`）。openをまたげない | suspendしない。`kern_text_observe()`・`kern_text_snapshot()` でtextを読み、GPUのscanoutへ描く（`src/drivers/gpu/venus/display.c`） | `libvulkan` の直接表示、`zwl` |

**この2つの間に調停が無い。** `/dev/graphics` が `kern_text_suspend()` した状態でGPUが
`kern_text_snapshot()` を読むと、止めたはずのtextが画面に出る。逆にGPUがscanoutを持っている間に
`/dev/graphics` がbackendのmode設定を行うと、同じ出力を2つのドライバが設定する。
今この衝突が起きていないのは、`/dev/graphics` を使うX11系とGPUを使うzwlを同時に動かしていないからにすぎない。

### 1.3 textの復帰の仕組みが2種類ある

- `/dev/graphics`: backendが `leave()` でmodeを戻し、`kern_text_resume()` でtext layerが再描画する。
- GPU: GPUドライバがtextのsnapshotを自分のscanoutへ描き続ける。所有者が死んでも、GPUがscanoutを
  持ったままconsoleを描く（q310以降の実測: 強制終了直後の640×480 console復帰）。

どちらも「textが戻る」が、経路も責任者も違う。共通層はこの2つを1つの契約にまとめる必要がある。

## 2. 目標と非目標

### 目標

1. `/dev/graphics` のcdev（ioctlの解釈、引数検査、所有権、mode保持）を1か所にし、機種の仕事を
   backendの関数表だけにする。pcat・pc98をそのbackendへ移す。
2. 画面の所有者を1つの調停器に集め、`/dev/graphics` とGPU scanoutのどちらが出しているかを
   kernelが知っている状態にする。所有者が消えたらtextへ戻す責任を1か所に置く。
3. `/dev/graphics` の描画先が、機種のframebufferからGPUのscanoutへ切り替わっても、
   userlandのUAPIが変わらないようにする。

### 非目標

- `include/uapi/graphics.h` の変更（§3）。
- GPUの `GPU_DISPLAY_*` UAPIの変更。p024はkernel内部の接続だけで足りる。
- rpi4・sun4u・x68k への `/dev/graphics` の追加（WS036の範囲）。
- 複数画面。今の `/dev/graphics` は1画面で、GPU側は `display_id` を持つ。共通層は
  1画面を前提にし、`display_id` を持つbackendでは「最初の有効な出力」を使う（§7に制限として記録）。

## 3. UAPIを変えない理由

`/dev/graphics` の11個のioctlは、すでに `userland/X11/{zwm,xzed,zterm}` とnoctのbeui backendが使っている。
描画先がGPUに変わっても、これらのアプリが求めるのは「矩形塗り・線・blit・flush・glyph」であって、
その実現手段ではない。UAPIを変えれば既存のアプリを全部直すことになり、p005・p024の範囲を超える。

ただし**GPU scanoutへ切り替えたときに意味が変わるものが2つある**ので、p024で次のように扱う。

- `KERN_GRAPHICS_GET_CAPS` の `capabilities`: backendが実際にできることを返す
  （`GRAPHICS_CAPABILITIES` は今はコンパイル時定数だが、共通層ではbackendから取る）。
  ただし `CAP_GLYPH` は共通層が常に立てる（§4.2、R1）。`userland/X11/xzed/main.c:472` が
  `CAP_FLUSH`・`CAP_GLYPH`・`CAP_BLIT_RGB24` を必須として検査するためである。
- `KERN_GRAPHICS_FLUSH`: 機種のframebufferでは「VRAMへの反映」、GPUでは「描いた画像をscanoutへ出す」。
  意味は「ここまでの描画を画面に出せ」で一貫するので、名前も番号も変えない。

## 4. 共通層の設計

### 4.1 置き場所

| ファイル | 内容 |
| --- | --- |
| `src/drivers/generic/graphics.c` | 共通層。cdev、ioctl、所有権、引数検査、backendの呼び分け |
| `include/kern/graphics.h` | backendの関数表 `struct drv_graphics_ops` と登録・解除のAPI |
| `include/kern/graphics-device.h` | 既存。`drv_graphics_device_register()` と `drv_graphics_device_restore_text()` は残し、中身を共通層が持つ |

`src/drivers/generic/` は既に `console.c`・`input.c`・`system-device.c`・`memory-device.c`・`loop.c`・`dma.c`
が置かれている、機種によらないドライバの場所である。

### 4.2 backendの関数表

```c
struct drv_graphics_ops {
	/* この出力が使える状態かを報告する。0なら登録しても公開されない。 */
	int (*ready)(void *private_data);

	/* 対応するmodeを書き出し、書いた数を返す。capacityが0なら数だけ数える。 */
	size_t (*get_modes)(void *private_data,
			    struct graphics_mode_info *modes, size_t capacity);

	/* 希望のmodeへ入る。実際に取れた値をmodeへ書き戻す。 */
	int (*enter)(void *private_data, struct graphics_mode *mode);

	/* modeを出て、入る前の状態へ戻す。失敗できない。 */
	void (*leave)(void *private_data);

	/* 描画。できないものはNULLでよく、共通層がcapabilityから落とす。 */
	int (*fill)(void *private_data, const struct graphics_rect *rect,
		    uint32_t color);
	int (*line)(void *private_data, unsigned x0, unsigned y0,
		    unsigned x1, unsigned y1, uint32_t color);
	int (*pattern_fill)(void *private_data,
			    const struct graphics_rect *rect,
			    uint32_t color, uint64_t pattern);
	int (*blit)(void *private_data, unsigned x, unsigned y,
		    const struct drv_graphics_image *image,
		    uint64_t pattern, int patterned);
	int (*flush)(void *private_data,
		     const struct graphics_rect *rects, size_t count);

	/*
	 * 機種が自前のフォントROMを持つ場合だけ。NULLなら共通層のフォントを使う。
	 * glyphの取り出しは出力と関係がないので、既定は共通層が持つ（R1）。
	 */
	int (*get_glyph)(void *private_data, uint32_t codepoint,
			 uint8_t bitmap[32], unsigned *width, unsigned *height);

	/* この出力ができることの集合（KERN_GRAPHICS_CAP_*）。 */
	uint32_t capabilities;

	/* 最大の幅と高さ。GET_CAPSが返す。 */
	uint32_t maximum_width;
	uint32_t maximum_height;
};

int drv_graphics_register(const struct drv_graphics_ops *ops,
			  void *private_data, unsigned priority,
			  struct drv_graphics_output **output);
int drv_graphics_unregister(struct drv_graphics_output *output);
```

- `struct drv_graphics_image` は今の `struct pcat_graphics_image` と同じ内容を機種によらない名前にしたもの
  （`include/kern/graphics.h` へ置く）。今は `pcat` と `pc98` で別々に同じ構造体を定義している。
- 関数表は `drv_gpu_ops`（`src/drivers/gpu/`）と同じ形にする。`private_data` を取り、登録が
  不透明なhandleを返し、解除がhandleを消費する。**GPUで確立した形を踏襲する**ので、
  新しい流儀を作らない。
- `capabilities` を関数表に持たせるのは、今の `GRAPHICS_CAPABILITIES` がコンパイル時定数で、
  機種が実際にできることと一致する保証が無いため。共通層は「関数ポインタがNULLでない」ことと
  `capabilities` のbitが一致するかを登録時に検査し、食い違えば `EINVAL` で拒否する。
- **`KERN_GRAPHICS_CAP_GLYPH` だけは共通層が常に立てる**（R1）。`src/drivers/platform/pcat/graphics/`
  の `vgafont.c`・`font.c` を `src/drivers/generic/` へ移し、共通層のフォントとする。
  backendが `get_glyph` を持つ場合（pc98のGDCフォントROM）はそちらを優先する。
  この規則が要るのは、`userland/X11/xzed/main.c:472` が `CAP_FLUSH`・`CAP_GLYPH`・`CAP_BLIT_RGB24` の
  3つを**必須**として検査し、欠ければ起動しないためである。backendによってxzedが動かなくなってはいけない。

### 4.3 優先度と差し替え

`priority` は、同じ画面に複数のbackendが出てくる場合の選択に使う。具体例:

- pcatでは boot framebuffer（UEFI GOP）と Cirrus GD5446 が両方あることがある。今は
  `backend.c` の中で選んでいる（1166行のうち大半がこの選択）。
- p024では、同じ画面に対してGPU backendが後から登録される。GPUの方が高い優先度を持つ。

`priority` は2段階でよい。**p005では 1 platform = 1 登録**とし、機種の中の選択（pcatの
boot framebuffer・Cirrus・標準VGA）は今どおり `backend.c` の中に残す。`backend.c` の分割は
p005の範囲外である（R7）。機種backendは固定の低い値、p024のGPU backendは高い値を使う。

共通層は**登録されたbackendのうち優先度が最も高く `ready()` が真のものを現在のbackendとする**。
現在のbackendが変わるのは、次のどちらかのときだけである。

1. 誰も `ENTER` していないとき。そのまま差し替える。
2. `ENTER` 中に、より高い優先度のbackendが登録されたとき。**すぐには差し替えない。**
   現在の所有者が `close` するまで待ち、そこで差し替える。描画の途中で描画先が変わると、
   アプリが持っている座標やmodeの前提が崩れるためである。

この規則により、GPUドライバのattachが `/dev/graphics` を使っている最中のアプリを壊さない。

### 4.4 所有権

今の `graphics_owner`（`struct file *`）と `graphics_entered` をそのまま共通層へ移す。加えて:

- `open` は所有者がいれば `EBUSY`。今と同じ。
- `close` で `entered` なら `leave()` と `kern_text_resume()`。今と同じ。
- `drv_graphics_device_restore_text()`（shutdown経路が呼ぶ）も共通層が持つ。
  **所有者が誰であれtextへ戻す**（R5）。今の実装は `/dev/graphics` の `entered` しか戻さないので、
  所有者がGPUのときは何もしない。共通層は調停器の状態を見て、`GPU` なら
  GPUドライバが登録した**強制解放のcallback**を呼ぶ。このcallbackは `struct drv_graphics_ops` とは
  別に、調停器が持つ1つの関数ポインタとする（backendの登録とGPUのscanout所有は別の話であるため）。

## 5. 画面の調停（`/dev/graphics` とGPU）

### 5.1 誰が画面を持っているか

共通層に**画面の所有者**を1つ置く。状態は3つだけである。

| 状態 | 画面を出しているもの | textの扱い |
| --- | --- | --- |
| `TEXT` | text layer | 描いている |
| `GRAPHICS` | `/dev/graphics` のbackend | `kern_text_suspend()` 済み |
| `GPU` | GPUのscanout（`GPU_DISPLAY_CLAIM` のlease） | GPUドライバが `kern_text_snapshot()` から描く |

`include/kern/graphics.h` に次を置く。GPUドライバはこれを呼ぶ。

```c
/* GPUがscanoutを持つ。/dev/graphics が使っていれば EBUSY。 */
int drv_graphics_display_acquire(enum drv_graphics_owner owner);

/* 手放す。textへ戻す責任は共通層が持つ。 */
void drv_graphics_display_release(enum drv_graphics_owner owner);
```

- `/dev/graphics` の `ENTER` は `drv_graphics_display_acquire(GRAPHICS)` を通る。
  GPUが持っていれば `EBUSY` を返す（今は黙って衝突する）。
- GPUの `GPU_DISPLAY_CLAIM` は `drv_graphics_display_acquire(GPU)` を通る。
  `/dev/graphics` が持っていれば `EBUSY`。GPU側のlease（`gpu_display_claim.lease`）はそのまま残し、
  **GPU内部の所有権はGPUが持ち続ける**。共通層が持つのは「画面を出しているのはどちらか」だけである。
- どちらの `release` でも、共通層が `TEXT` へ戻し `kern_text_resume()` を呼ぶ。
  textへ戻す責任が1か所になる。

**調停器を触るのはUAPIの入口だけである**（R2）。`GPU_DISPLAY_CLAIM` ioctl と `/dev/graphics` の
`ENTER` の2か所からしか `acquire` を呼ばない。`/dev/graphics` のGPU backend（§5.3）が
内部でscanoutを取る経路は、GPUドライバ内部のlease APIを直接使い、調停器を通らない。
通してしまうと、共通層が既に所有者を `GRAPHICS` にした後でbackendが `acquire(GPU)` を呼び、
**自分で自分を `EBUSY` で弾く**。

**lockの順序は「共通層のmutex → GPUのlock」の一方向だけを許す**（R4）。
`GPU_DISPLAY_CLAIM` は GPU のlockを取る**前に** `acquire()` を呼ぶ。
`/dev/graphics` の `ENTER` は共通層のmutexを取った中でbackendの `enter` を呼び、
その先でGPUのlockへ降りる。逆順の経路を作らない。

**`acquire`/`release` はsleepできる文脈からだけ呼ぶ**（R3）。割り込み、watchdog、
spinlock保持中からは呼ばない。`release` は `kern_text_resume()` を通じてtext layerと
display driverの描画へ入るためである。

### 5.2 GPUドライバ側の変更（p024）

- `GPU_DISPLAY_CLAIM` の成功前に `drv_graphics_display_acquire(GPU)`、`GPU_DISPLAY_RELEASE` と
  **sessionの `close`** で `drv_graphics_display_release(GPU)`。
- **device faultやwatchdogの経路からは `release` を呼ばない**（R3）。これらはGPUの内部lockを
  保持したまま走り、`release` はtext layerへ入るので、そこから呼ぶと順序が定まらない。
  device lossは既に「新しい投入を拒む」形で表現されており、画面の所有はsessionが閉じるまで
  GPUが持ったままでよい。q309〜q313の実測でも、consoleが戻るのはprocessが終了してfdが閉じたときである。
- 今のGPUドライバはtextを `kern_text_observe`/`snapshot` で読んで自分で描いている。**これは残す。**
  GPUがscanoutを持っている間のconsoleの見え方は、実測（q309〜q313の10 VM）で確かめた振る舞いであり、
  ここを変えると回帰する。共通層が増やすのは調停だけである。
- GPUが組み込まれていないbuild（`CONFIG_DRIVER_PCI_VENUS=0`・`_I915=0`）では
  `drv_graphics_display_acquire` の呼び出し元が無いだけで、共通層は変わらない。

### 5.3 GPU backendとしての `/dev/graphics`（p024）

p024は、GPUを持つ機種で `/dev/graphics` の描画先をGPUのscanoutにする。共通層のbackendとして
GPUドライバが `drv_graphics_register()` する形にし、次のように対応させる。

| `/dev/graphics` | GPU backendの実装 |
| --- | --- |
| `enter` | scanout用のlinear画像を確保し、modeを設定する |
| `fill`・`line`・`pattern_fill`・`blit` | その画像へCPUで描く。GPUの描画命令は使わない（§7の制限） |
| `flush` | `SET_SCANOUT_BLOB` 相当で画像を画面へ出す |
| `leave` | scanoutを解放し、画像を捨てる |
| `get_glyph` | 持たない（`CAP_GLYPH` を落とす） |

この段階でGPUの描画エンジンを使わないのは、`/dev/graphics` の描画が矩形塗り・線・blitという
CPUで完結する操作で、GPUのcommand streamに載せる利点がflushの回数より小さいからである。
**GPUで描く価値があるのはzdesktopの合成であって、`/dev/graphics` の単発の矩形ではない。**
この判断はp024の実測で覆ってよい。覆った場合は結果に記録する。

## 6. 実装の分け方と受け入れ

### p005: 共通層（このWSの次のPhase）

1. `include/kern/graphics.h` に `struct drv_graphics_ops`・`struct drv_graphics_image`・
   登録/解除・`drv_graphics_display_acquire`/`release` を置く。
2. `src/drivers/generic/graphics.c` に、今の `pcat-graphics.c` の内容を機種によらない形で1つだけ書く。
3. `src/drivers/platform/{pcat,pc98}/graphics/backend.c` を `struct drv_graphics_ops` で登録する形に直し、
   `pcat-graphics.c`・`pc98-graphics.c`（計1630行）を消す。
4. `platform/{pcat,pc98,amd64}/vmunix.mk` の対象を差し替える。

受け入れ:

- amd64・pcat・pc98 の kernel が warning 0。`disk-image` が通る。
- `pcat-graphics.c`・`pc98-graphics.c` が無い。`drv_pcat_graphics_backend_*`・`drv_pc98_graphics_backend_*` の
  公開名が `struct drv_graphics_ops` の中へ入っている。
- **host fixture**（新規、`plan/ws035/tests/`）: 偽backendに対して、ioctl 11個の引数検査、
  所有権（2つ目のopenが `EBUSY`）、`ENTER` 前の描画ioctlの拒否、`close` での `leave`＋`resume`、
  capability不一致の登録拒否、backend差し替えの延期、調停（`GRAPHICS` と `GPU` の相互排除）。
- **実QEMU**: `/dev/graphics` を実際に使う小さな試験プログラムをrootfsへ入れ、
  `ENTER` → `FILL_RECT` → `FLUSH` → `close` を行わせ、**screendumpで塗った矩形が出ていること**と、
  closeの後にlogin promptへ戻ることを確かめる。
- 3 platform の boot-test が PASS（amd64はUEFI、pcatは `BOOT_MODE=bios-ide`、pc98はPC-98 QEMU）。
  **ただしboot-test単体は共通層の回帰確認にならない**（R6）。consoleは `kern_text_ops` を通り、
  `/dev/graphics` を経由しないので、login promptが出てもこのcdevは一度も使われていない。
  boot-testは上の試験プログラムの後段（console復帰）の確認として使う。

### p024: GPU scanoutへの切替えと戻し

1. GPUドライバに `drv_graphics_display_acquire`/`release` を入れる（§5.2）。
2. GPU backendを `drv_graphics_register()` する（§5.3）。優先度はframebuffer backendより高い。
3. 切替えの規則（§4.3）どおり、`ENTER` 中のbackend差し替えを延期する。

受け入れ:

- `/dev/graphics` とGPUの同時使用が `EBUSY` で拒否される（host fixture）。
- GPUが持っている間に `/dev/graphics` の `ENTER` が失敗し、GPUのreleaseの後に成功する（host fixture）。
- 実QEMU: zwl（またはvkdemo）の直接表示が従来どおり通り、終了後にconsoleが戻る。
  既存のq313の10 VMのうち `direct`・`wayland`・`producer-exit` を回す。
- amd64 boot-test PASS。

## 7. 制限（設計時点で分かっているもの）

1. **1画面だけ。** `/dev/graphics` のUAPIに画面の指定が無いため、共通層も1画面である。
   GPUが複数の出力を持つ場合、`/dev/graphics` は最初の有効な出力を使う。複数画面は別WSの話。
2. **`/dev/graphics` のGPU backendはCPUで描く**（§5.3）。GPUの描画エンジンは使わない。
3. **`ENTER` 中のbackend差し替えは起きない**（§4.3）。GPUのattachが遅れて起きた場合、
   次の `open` まで機種のframebufferを使い続ける。
4. **GPUがscanoutを持つ間のconsoleは、GPUドライバがtext snapshotから描く。** 共通層は
   その描画には関与しない。この経路の振る舞いはq309〜q313の実測のままである。
5. rpi4・sun4u・x68k には `/dev/graphics` が無い。共通層を入れても増えない（WS036）。
6. **`.vfs_bss` セクションの理由が分かっていない**（R8）。今の `pcat-graphics.c` は
   `graphics_owner`・`graphics_mode`・`row_buffer`（4096 B）・`palette_buffer`（1 KiB）・`graphics_lock` を
   `__attribute__((section(".vfs_bss")))` に置いている（登録が `src/kern/vfs.c:349` から起きるため。
   `platform/amd64/vmunix.ld:49` がこのセクションを `.bss` と同じ場所へ集める）。
   **共通層へ移すときもこの属性は残す。** なぜ要るのかはp005で確かめ、不要と分かれば落とす。
7. `KERN_GRAPHICS_GET_CAPS` が返す `capabilities` は、共通層がbackendから取る値になる。
   今はコンパイル時定数なので、**backendによっては返る値が減る**。
   実際の利用者は2つで、どちらもcapabilityを見る（2026-09-23に確認）。
   `userland/X11/xzed/main.c:468` は `CAP_FLUSH`・`CAP_GLYPH`・`CAP_BLIT_RGB24` を必須として検査し、
   noctのbeui（`userland/base/noct/noct/src/api/api-beui-zedbsd.c:4591`）は取得した値を保持して
   機能ごとに判定する。`zwm`・`zterm` は `/dev/graphics` を直接使わず、xzedを経由する。
   `CAP_GLYPH` を共通層が常に立てる規則（§4.2）と合わせて、xzedが動かなくなる経路は塞がれる。
