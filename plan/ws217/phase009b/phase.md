<!-- awesome-plan project=zedbsd record=ws217-p009b -->

# ws217-p009b: i915 の 180° の scanout（xd と xd9）

Parent: [WS217](../ws.md)
Status: in-progress（実装は branch `agent/p2-ws217` の `8d51619d93` だけ、main に無い。試験は host と build だけ。2026-11-15 P5 q2110-i01: p009a だけを main に載せ直し、この段は載せていない。今の main への載せ直しの見込みは §5。guardrail「i915 の display の書き換えは 5330 で動く段ごとに」により、5330・SV7 の実機で通るまで main に入れない。cleared にしない）
Disposition: normal
Primary Milestone: MG006（WS から継承）
Queue / attempts: q1680-i01（P2、worktree `/home/awe/zedBSD-worktrees/p2-ws217`、branch `agent/p2-ws217`、基点 main `d23ab8838`、p009a の上、commit `8d51619d93`）。q2110-i01（2026-11-15 P5）は見込みの記録だけ（§5）
Purpose / goal: [design-v5.md](../design-v5.md) の r2 の A1・A3〜A6・A8・A11 の i915 の部分。display 11〜13 の xd（resident と head）と display 9 の xd9（firmware の plane の flip）で、`GPU_DISPLAY_PRESENT_ROTATE_180` の frame を plane の hardware の回転で出す。
Prerequisites: p009a（UAPI と核の検査）。

## 1. 実装

### 1.1 xd（display 11〜13）

| 所 | 内容 |
| --- | --- |
| `intel/display-regs-xd.h` | `I915_XD_PLANE_CTL_ROTATE 0x3`・`I915_XD_PLANE_CTL_ROTATE_180 0x2`。出典は TGL・ACM の PRM Vol 2c の PLANE_CTL（Plane Rotation）を Doc Ref つきの複数行の block で（m10） |
| `xd-pipe.h/.c` | `struct i915_xd_plane` に `rotation`（度数）。`drv_i915_xd_plane_words` は 180 の時だけ rotation の field に 10b、他の値（90 など）は回さない（m14） |
| `xd-screen.h/.c` | `struct i915_xd_flip` に `current_rotation`・`pending_rotation`。`drv_i915_xd_flip_arm(flip, surface, rotation)`、judge・idle・settled が latch の時に回転も写す（stuck では写さない、A4）。純粋な関数 `drv_i915_xd_flip_target(idle, armed_rotation, rotation)` → `I915_XD_TARGET_BACK`・`_ARMED`・`_SETTLE`（A5） |
| `xd-picture.h/.c` | config に `fb_rotation`（prepare が `flip.current_rotation` と plane に入れる、A1）、status に `cur_rotation`・`pend_rotation`、`drv_i915_xd_picture_flip`／`_flip_nowait` に `rotation`、新しい `drv_i915_xd_picture_rotation(pictures, screen)`（armed なら pending の、でなければ表示中の回転）。flip の arm で回転が表示中と違う時だけ `i915: display screen N: plane rotate=R` を log（m13） |
| `worker.h` | `struct i915_worker_present` に `rotation` |
| `present.h/.c` | `drv_i915_present(..., rotation)`（呼び手 1 つ）、`i915_present_shared`・`i915_present_head` の copied の経路で `request->flags` から item に（A3）。`i915_present_target(display, rotation, &flip)` は `drv_i915_xd_flip_target` で選び、SETTLE なら `drv_i915_xd_picture_flip_settle` で待ってから back に（A5、`I915_PRESENT_NO_VSYNC=1` の build だけで届く経路）。`i915_present_flip(display, publish, rotation)`。`drv_i915_lcd_resident_flip`（window の終わりの buffer A への flip back）は表示中の回転で flip（A3・M2） |
| `resident.c`・`display-internal.h` | window の終わりに relight なら `display->window.kept_rotation` に表示中の回転を残し、次の点灯の `env->cfg.fb_rotation` に。buffer を作り直す点灯は 0（A1） |
| `head.c`・`internal.h` | head の item の回転で target・flip（A5 も同じ）。`drv_i915_head_stop(keep=1)` は表示中の回転を `head->kept_rotation` に残し、`i915_head_light_once` が dormant の buffer で点ける時に `head->cfg.fb_rotation` に。新しい buffer は 0（A1） |
| `display.c` | `i915_display_ops` に `DRV_GPU_DISPLAY_ROTATE_180`、`i915_display_query`（resident）と `i915_display_other_query`（head の候補）の両方に `GPU_DISPLAY_ROTATE_180`（m4）。i915_display_ops の出力は全部 xd の universal plane（Gen9 は bootfb の ops） |
| `display-internal.h` | scanout の buffer の注記の「no rotation」を直した |

### 1.2 xd9（display 9、A6）

| 所 | 内容 |
| --- | --- |
| `intel/display-regs-xd9.h` | `I915_XD9_PLANE_CTL_ROTATE_180 0x2`（既存の KBL PRM の block の中、`ROTATE_MASK` の隣） |
| `xd-pipe.c`・`xd-pipe.h` | `drv_i915_xd9_plane_flip(io, pipe, surf, stride, rotation)`: firmware の PLANE_CTL を読み、rotation の field だけを替えて、変わる時だけ書く → stride → SURF（SURF の書き込みが両方を arm する） |
| `xd9-flip.c` | present が `request->flags` から回転を arm に渡す。restore（lease の終わり・flip の停止）は 0 で arm し firmware の値（readout は回った plane を引き取らないので firmware の rotation は 0）に戻す |
| `bootfb.c` | ops に `DRV_GPU_DISPLAY_ROTATE_180`。query は flip を使う時（`bootfb->flip`）だけ `GPU_DISPLAY_ROTATE_180`。copy の経路・xd9 の外部 display・flip が途中で諦められた frame の `ROTATE_180` は EOPNOTSUPP（copy は回せない） |

### 1.3 推しで決めた所（Q1 へ報告）

- A5 の「回転が違えば settle して flip」の判定を `xd-screen.c` の純粋な関数にし、resident と head の両方で使った。
- window の終わりの flip back（relight でない時）は buffer A の絵の回転ではなく表示中（B）の回転で出す。A は同じ lease の 1 つ前の絵で、直後に出力は止まる（最大 1 frame）。
- xd9 で flip が途中で失敗し copy に戻った後の 180 の frame は EOPNOTSUPP（CPU・GPU の copy で回して出す案は範囲を広げるので採らない）。その後の query は `GPU_DISPLAY_ROTATE_180` を出さない。
- 同じ buffer への回転だけの変化は今まで通り SAME で断る（present は常に表示していない buffer に描くので当たらない）。

## 2. 確かめた事（2026-10-29、host と build だけ）

| 確かめ | コマンド | 結果 |
| --- | --- | --- |
| xd の host 試験の全部（ASan/UBSan、source の監査） | `sh plan/tools/i915-display/run-xd-host-tests.sh build/p009b-host3` | 下の §2.1 |
| Gen9 の host 試験 | `sh plan/ws237/tests/run-gen9-{flip,backlight,mode,modeset,outputs}-host-test.sh` | flip 48・backlight 21・mode 61・modeset 77・outputs 29 checks、全部 0 failures |
| amd64 vmunix | `make ZEDBSD_CONFIG=config/ci/config-amd64.mk BUILD=build/p009a build/p009a/vmunix`（2026-10-29 の旧い build system の形。今は §5 の形） | exit 0、warning 0、`amd64 vmunix check: PASS` |

### 2.1 足した host 試験の項

- `host-xd-pipe-test.c`: display 11・12・13 で 180 の PLANE_CTL が 10b（他の bit は今と同じ: 0x84000002・0x94000002）、display 13 の 90 は回さない、xd9 の flip が firmware の PLANE_CTL の rotation の field だけを替え、0 で戻す。
- `host-xd-screen-test.c`: arm の pending の回転、idle・judge・settled の latch で current に写る、stuck・not settled では残る、`drv_i915_xd_flip_target` の 6 通り。既存の `drv_i915_xd_flip_arm` の呼びの追従。
- `host-xd-picture-test.c`: 180 の flip_nowait が PLANE_CTL に 10b を書き `drv_i915_xd_picture_rotation` が 180、latch の後 `cur_rotation` 180、0 の waited flip で 0 に戻る、dark の後に `fb_rotation = 180` の config で prepare・light すると plane が 180 で arm される（A1 の keep の後の点灯）。既存の 3 つの呼びの追従。

## 3. 未実施

- 5330（display 13）・SV7（display 9.5）・5320（display 12）の実機（A8 の vkdemo `--transform=180` の間の PLANE_CTL の bit 1:0・SURFLIVE、終わりの後の 00b、DC の idle の後、head の 180、hold の経路）。T1 か担当の script で、Q1 の機械の割当の後（Q217-3）。
- QEMU（i915 は QEMU で動かない）。
- display 11（Ice Lake）の PRM の確認（m7）: host 試験は TGL と同じ field を仮定。
- display 14 以上が xd に入らない門の所在（m8）: 調べていない。
- DC5/DC6 の後に PLANE_CTL の回転が保たれるか（PRM に記述なし）。
- m17（prepare を経ずに light を繰り返す経路）: head は light_once が毎回 prepare し、失敗の後は buffer を返して新しく作る（回転 0）ことを読んだ。resident の run の再試行は読んでいない。

## 4. 残り

p009c（libvulkan・vkdemo）。実機の回帰の後に Q1 が merge。

## 5. 今の main への載せ直しの見込み（2026-11-15 P5、q2110-i01）

p009a は `ca9f3d491e`（agent/p5-ws217、基点 main `44bb7cdaac`）で載せ直した。この段（`8d51619d93`）は p009a の上に積む。

衝突の見込み（`git merge-tree --write-tree --merge-base=8d51619d93^ <p009a を載せた HEAD> 8d51619d93` で確かめた）:
- 文の衝突は `plan/ws217/ws.md` だけ（表と Resume point。今の ws.md を正として、行を足すだけ）。source の 23 file は自動で当たる。
- 基点から main で変わった i915 の file は 6 つ（`bootfb.c`・`display.c`・`present.c`・`xd-screen.c`・`xd-screen.h`・`xd9-flip.c`、計 10 commit）。中身は時間の単位（`KERN_CLOCK_HZ` の tick → `kern_ms_to_ticks()`・`kern_ticks_to_ns()`）、`hal_space_map_device` → `kern_device_map`、`drv_gpu_display_ops` の末尾の `refresh_arm`（初期化子の `NULL` が 1 つ増えた）、hotplug の `drv_i915_display_changed()`、present の window の `kern_text_suspend()`／`kern_text_resume()`。どれも回転の所（`flags` の位置・present の item・flip の arm・PLANE_CTL）とは重ならない。`head.c`・`resident.c`・`xd-picture.c`・`xd-pipe.c`・`worker.h` は基点から変わっていない。
- 意味の上で見る所: (1) `i915_display_ops`・`i915_bootfb_ops` の位置の初期化子で `flags` に `DRV_GPU_DISPLAY_ROTATE_180` を立てる所が、`refresh_arm` を足した後も `flags` の位置か（`gpu-display.h` の struct の順と数えて確かめる）。(2) WS353 の display の fence（`GPU_DISPLAY_FENCE` の REFRESH）は回転と無関係だが、settle（A5）の待ちが refresh の境を数える所と干渉しないか読む。(3) host 試験の追従（`host-xd-screen-test.c` の `drv_i915_xd_flip_arm` の引数、`host-xd-picture-test.c`）が今の試験の fixture で通るか。

手順:
1. p009a が main に入った後、新しい worktree（main から）で `git cherry-pick 8d51619d93`、`plan/ws217/ws.md` の衝突は今の ws.md を残して手で直す。
2. 上の (1)〜(3) を読み、`plan/tools/i915-display/run-xd-host-tests.sh` と `plan/ws237/tests/run-gen9-*-host-test.sh`、`make -j4 CONFIGMK=config/config.x86_64.mk build/x86_64/vmunix`（warning 0）。
3. 5330（display 13）・SV7（display 9.5）の実機の回帰（起動・内蔵 LCD の表示・desktop、vkdemo `--transform=180` は p009c の後）。実機は Q1 の割当。通るまで main に入れない。
