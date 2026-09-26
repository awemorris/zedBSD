<!-- awesome-plan project=zedbsd record=ws068p006 -->

# ws068-p006: i915 実機での GL（GLX の zgears、App Home の X11、仮想デスクトップ）

Phase ID: `ws068-p006`
Parent: [WS068](../ws.md)
Status: cleared（q484-i01、2026-09-27）
Phase disposition: normal
Queue: q484-i01
承認: 2026-09-26 ユーザーの自律実行の指示（EGL/GLES・X11 を含むデスクトップ関連を優先）
設計: design.md §7（i915 では F-023 の不足が GLES の app に効く）

## 範囲

1. 事前の対策: gl_Position の書き換えの OpCompositeInsert（F-023 で i915 の compiler に無い）を OpCompositeConstruct に（Venus で確認済み）。
2. i915 実機の capture の scenario `zdesktop-x11`（plan/ws031/tests/i915-capture.py）: App Home から Gears（Xzed --rootless と GLX、
   固定機能の GL が i915 の Vulkan 実行器で動く）と X terminal、Ctrl+Alt+→・← でデスクトップ 2 と 1。`zdesktop-home` の icon の位置を
   6 つの app に。Xzed の log を disk へ。
3. 実機で起きた不足は、この Phase の範囲で直せるもの（変換層・shader の形）は直し、実行器・compiler に要るものは F-023 と Bug に記録する。

## 受け入れ

1. 実機（5330、i915、capture）で `zdesktop-x11` の各 check の結果と画像を記録する（通らなければ原因を記録）。
2. 直したものは Venus の回帰（egl-p008・x11-p005・zdesktop-p070）が通る。
3. 実機の LCD での目視は未実施でよい（capture の画像）。

## 結果（2026-09-27、q484-i01）

実機（5330 の i915 を QEMU へ VFIO で渡した GPU、capture 表示）で `zdesktop-x11` の 6 検査（desktop_drawn・gears_shows・gears_turns・
xterm_starts・desktop2_differs・desktop1_back）が **全部 PASS した run がある**（run6、`build/ws068-p006-hw-run6/`: sheet.png・result.json・
guest-logs.txt）。i915 の Vulkan 実行器で固定機能の GL（GLX の zgears）が正しい絵を描き（`ZGEARS CHECK ... failures=0 glerror=0x0`、
red 65803・green 16646・blue 13397）、約 27 fps。App Home から X terminal、仮想デスクトップ 2 と 1 も実機で動いた。
ただし gears_turns は間欠で落ちる（最後の run も落ちた）。原因は zgears の側の止まりで、[BUG-057](../../bugs/BUG-057.md) に記録した。

### 実機で見つけて直した不足（この Phase の範囲）

1. 固定機能の shader が i915 の compiler に拒まれた（struct の member 16 まで、Flat の decoration 無し、PointSize 無し、vertex kernel 16 KB まで）。
   uniform block を 9 member に、光源の数で 1/2/4/8 の変種、Flat の代わりに glEnd で flat の primitive を頂点ごとに展開（`immediate_flat`）。
   host の試験 `plan/ws068/tests/i915-shader-check/`（i915 の compiler を host で走らせ、全変種の可否を見る）。
2. pbuffer の最初の frame の `vkCmdClearDepthStencilImage` が実行器に無い → recording 全体が拒まれ device lost。libEGL は新しい pbuffer を
   最初の render pass の clear で消す（`vulkan_pbuffer_layouts`）。可搬で、clear-image の命令を使わない。
3. 実行器が `vkCmdSetLineWidth`・`vkCmdSetDepthBias`・stencil の 3 つ（opcode 96・97・100〜102）を知らず、recording を拒んでいた
   （GLES の pipeline は毎回これらを動的状態にする）。実行器（`src/drivers/gpu/i915/render/command.c`）はこれらを decode して捨てる
   （描画に stencil・depth bias は元から無い。幅 1 以外の線は一度 XXX を出す）。
4. 実行器に `vkResetDescriptorPool`（opcode 76）が無く EOPNOTSUPP → frame 2 以降が device lost。実行器に実装（pool の set を全部解放、
   pool は残す。`descriptor.c`・`objects.c`）。
5. 空の pbuffer の frame（readback の後に何も描かない swap）で recording を開いたままにしない（`vkEndCommandBuffer` で閉じる）。

### 調べるための出力（残す）

- libEGL: Vulkan の呼び出しの失敗を一度ずつ stderr へ（`EGL: vkEndCommandBuffer failed (-4)` のように）。
- 実行器: 拒んだ命令の opcode と errno を kernel の log へ（最初の 32 個、`dispatch.c`）。
- zgears: 最初の 10 frame の draw と swap の時間（`ZGEARS FRAME`）、frame 1・2・50 の readback の和（`ZGEARS SUM`）。
  回転は時間で（毎秒 70 度、frame 1 は検査のため 2 度）。frame ごとの角度だと歯の対称で 9 frame 周期になり、2 枚の撮影が同じ絵になりうる。
- zwl（`--log-frames` のとき）: `ZWL COMMIT`、`SHM_COPY` の行の和。silent に `server->failed` にしていた 5 箇所に `ZWL FAILED site=...`。
- Xzed: rootless の present の数（commit と、両 buffer が compositor にあった回数）を 200 回ごとに stderr へ。
- 実機の run の `run-home.sh` は `i915: vk` と `gpu: ioctl` の kernel の行を `/var/log/vk.log` に貯める（dmesg の ring は最新しか残らない）。
- x11-p005（Venus）: 2 秒後の `gears-later.png` が違う絵であること（回ることの検査）を足した。

### 確かめなかった仮説（記録）

- MOCS: 実行器はすべての surface を uncached の MOCS（3）で読む。CPU が write-back で書いた画像を GPU が DRAM から読む非一貫の疑いで、
  sampled の画像を index 5（LLC）にして 3 run 試したが、止まりは同じ頻度で残り、GPU 描画の client の画像を LLC 経由で読む危険もあるので戻した。
  kernel の debug（sampled の画像の memory の和）では、止まったとき zwl の画像の中身そのものが変わっていなかった（止まりは上流）。
- fork の COW: 止まりの時点に fork が無いことを kernel の log で確かめた（否定）。

## 検証

実機（i915、capture）: run6 で 6 検査 PASS（画像 `build/ws068-p006-hw-run6/sheet.png`）。それ以外の run: BUG-056（zwl が client の
後片付けで `ZWL EXIT error=5 cleanup_failed=1`）が 17 run 中 3 回、gears_turns の FAIL（BUG-057）が多数。実機の LCD の目視は未実施。

Venus（QEMU、最後の code で）: egl-p008 PASS、x11-p005 PASS（gears-later が違う）、zdesktop-p070 PASS。
build（`plan/ws035/tests/build-zdesktop-image.sh`）warning 0。boot test PASS（`build/ws068-p006-boot/login.png`）。
host: `plan/ws068/tests/i915-shader-check/run.sh`（smooth の 1/2/4 光源と fixed_frag が通る。flat の変種は i915 で拒まれるが、
client の配列の flat の描画にだけ使う）。

## 残り

- [BUG-057](../../bugs/BUG-057.md): 実機で zgears が数百 frame の後に止まることがある（X/GLX の道）。
- [BUG-056](../../bugs/BUG-056.md): 追記あり（zwl は signal でなく `server->failed` で終わっている）。
- flat の変種の shader（8 光源、Flat 無しの形）は i915 で未だ拒まれる（client の配列の flat の描画だけ）。F-023 に含む。
