<!-- awesome-plan project=zedbsd record=ws217-p009a -->

# ws217-p009a: UAPI の 180° の定数・ops の印・核の検査

Parent: [WS217](../ws.md)
Status: cleared（2026-11-15 Q1、merge 557ea0ffbf）
Disposition: normal
Primary Milestone: MG006（WS から継承）
Queue / attempts:
- q1680-i01（2026-10-29 P2、branch `agent/p2-ws217` の `ff15539e66`、基点 main `d23ab88387`）: 実装と host 試験・build。QEMU は範囲外。main に merge されないまま残った。2026-10-29 にユーザーから、手の空いた担当は試験なしの実装で残りの Phase を進めてよいという趣旨の指示があり、その範囲で作られた。
- q2110-i01（2026-11-15 P5、worktree `/home/awe/zedBSD-worktrees/p5-ws217`、branch `agent/p5-ws217`、基点 main `44bb7cdaac`）: p009a だけを今の main に載せ直し、試験を流す。p009b（i915）・p009c（libvulkan）は載せない。
Purpose / goal: [design-v5.md](../design-v5.md) A2 の p009a。UAPI の 3 つの定数と ops の印 `DRV_GPU_DISPLAY_ROTATE_180`、核の検査（登録・present・commit）を純粋な関数にして host 試験。
印を立てる driver はまだ無いので、既存の driver の挙動は変わらない（どの ops にも印が無く、`ROTATE_180` の present・commit は核が EOPNOTSUPP で断る）。
Prerequisites: p009-1（PRM の確認、2026-10-13 済み、[phase009](../phase009/phase.md) §1〜§4）。

## 1. 実装

| file | 内容 |
| --- | --- |
| `include/uapi/gpu-display.h` | `GPU_DISPLAY_ROTATE_180 4096U`（info.flags）、`GPU_DISPLAY_PRESENT_ROTATE_180 4U`（present.flags）、`GPU_DISPLAY_PLANE_ROTATE_180 4U`（plane_state.flags、primary だけ・SHOWN と一緒）。意味は p001 第 4 版 §5.1 のまま。`GPU_ABI_VERSION`・struct は不変 |
| `include/drivers/gpu/gpu-display.h` | `DRV_GPU_DISPLAY_ROTATE_180 2U`（kernel の内部の面、HAL ではない）。印のある backend は query が `GPU_DISPLAY_ROTATE_180` を出さない出力で EOPNOTSUPP を答える、と契約に書いた |
| `src/drivers/gpu/gpu-display-flags.h`（新） | 核の純粋な検査（`static inline`）: `gpu_display_ops_flags_check`（既知の印は UNGATED・ROTATE_180）、`gpu_display_present_flags_check`（FIFO・FIFO\|BLOB、それぞれ \|ROTATE_180、印の無い ops は EOPNOTSUPP）、`gpu_display_plane_flags_check`（既知の bit、overlay の ROTATE と隠れた primary の ROTATE は EINVAL）、`gpu_display_plane_rotation_check`（印の無い ops は EOPNOTSUPP） |
| `src/drivers/gpu/gpu.c` | `gpu_ops_validate`（登録）・`GPU_DISPLAY_PRESENT`（PRESENT_SYNC も同じ所を通る）・`gpu_commit_plane_check`・`gpu_commit_ioctl`（`gpu_commit_check` の後、ops を見て）で上の関数を呼ぶ |
| `src/drivers/gpu/tests/host-display-flags-test.c`（新）、`plan/ws217/tests/host-gpu-display-flags.sh`（新） | 上の 4 つの関数の host 試験（34 項） |
| `docs/reference/gpu-display.md`・`gpu-display-control.md` | 2026-11-15 に足した写し: present の flags に `ROTATE_180` を許す文、`GPU_DISPLAY_ROTATE_180`・`GPU_DISPLAY_PRESENT_ROTATE_180`・`GPU_DISPLAY_PLANE_ROTATE_180` の表の行、EINVAL・EOPNOTSUPP の条件、commit の「Turning the primary」の段落。branch の版は docs を直していなかった（docs が「flags は FIFO か FIFO\|BLOB だけ」のまま UAPI と食い違う所だった） |

判断（2026-10-29 P2）:
- 検査は新しい `.c` ではなく header の `static inline` にした。`.c` を足すと Makefile・`mk/` の build の list を変える要る（Makefile・`mk/` は Claude が変えない）。header なら list は変わらない。
- EINVAL（形の誤り）を EOPNOTSUPP（印が無い）より先に判定する。commit は全部の plane の形の検査（`gpu_commit_check`）の後で印を見る。

## 2. 載せ直し（2026-11-15 P5）

- `git cherry-pick --no-commit ff15539e66` は衝突なしで当たった（`gpu.c` は基点から約 2200 行増えたが、present の flags の検査・commit の検査・登録の検査の所は形が同じ）。
- 今の main の UAPI の bit と重ならない事を確かめた: info.flags は 2048（HAS_OVERLAYS）まで、present.flags は 1・2、plane.flags は 1・2。`GPU_CAP_DISPLAY_EVENTS 4096U` は capabilities の bit で info.flags とは別の field。
- 今の main で present の flags を見る所: 核（`gpu.c` の `GPU_DISPLAY_PRESENT`、`GPU_DISPLAY_PRESENT_SYNC` も同じ case）と driver（venus・bcm2711・ghci の `request->flags != FIFO` など）。driver の検査は変えていない（核が印の無い ops の ROTATE を先に断るので届かない）。
- commit: `ca9f3d491e`（p009a の source・docs と、§3 の古い試験の直し 2 本）。

## 3. 確かめた事（2026-11-15 P5）

build は今の build system の順（`make CONFIGMK=config/config.<platform>.mk llvm-toolchain-cached` → `sysroot` → `build/<platform>/vmunix`、`-j4`）。`config/ci/*.mk` は旧い形式（`ZEDBSD_PLATFORM`）で、今の Makefile では PLATFORM が空になり使えない。

| 確かめ | コマンド | 結果 |
| --- | --- | --- |
| 核の host 試験（ASan/UBSan、`-Wall -Wextra -Werror`） | `sh plan/ws217/tests/host-gpu-display-flags.sh build/p009a-host` | `host-display-flags-test: 34 checks, 0 failures` |
| x86_64 の sysroot と vmunix | `make -j4 CONFIGMK=config/config.x86_64.mk sysroot`、`make -j4 CONFIGMK=config/config.x86_64.mk build/x86_64/vmunix` | 両方 exit 0。compiler の warning 0（log の `warning` は make の jobserver の注記 1 行だけ）。`gpu.c` を compile した |
| rpi5（arm64）の sysroot と vmunix | 同じ形で `config/config.rpi5.mk`、`build/rpi5/vmunix` | exit 0、warning 0、`arm64 vmunix check: PASS` |
| vf2（riscv64）の sysroot と vmunix | 同じ形で `config/config.vf2.mk`、`build/vf2/vmunix` | exit 0、warning 0（link まで） |
| WS217 の既存の host 試験（回転の回帰） | `sh plan/ws217/tests/host-{absolute,displays-conf,hid-pen-props,rotate,rotation-settings,scanout-rules,tablet-direct}.sh` | 7 本とも PASS（直しの後。下） |
| `git diff --check HEAD` | 変えた file | 出力なし |

古くなっていた試験の直し（同じ commit）:
- `host-rotation-settings.c`: `KL_VERSION == 93` の検査が今の 109 で FAIL。確かめたいのは「回転の API が入った版 93 以上」なので `>= 93` に。
- `host-tablet-direct.c`: `tablet.c` が今は `kwl_client_fail()`（`main.c`）を呼び、link で失敗。fake を足した（どの case も object を使い切らないので、呼ばれたら FAIL と数える）。

QEMU の回帰: §3.1。

### 3.1 QEMU（x86_64、Venus）

既定の x86_64 の image は firmware の package（i915・AX211 ほか）を含み、その license の同意が要るので作れない（担当の同意は権限の判定で拒まれる）。firmware を外した WS353 の試験の image（`plan/ws353/tests/config-amd64-render.mk`、text の login、compositor と試験の client）を使った。kernel はこの worktree の p009a の tree（`ca9f3d491e`）から作った物。Venus の renderer は `sh plan/ws353/tests/build-venus-renderer.sh`（この worktree の `build/ws035-sq-venus/install`、`build-venus-renderer: PASS`）。

| 確かめ | コマンド | 結果（summary の行） |
| --- | --- | --- |
| 回転を含む compositor の回帰（WS353 の後の main の形） | `timeout 7000 sh plan/ws353/tests/qemu-render-wait.sh build/p5-ws217-render` | `build: warnings outside noct 0`、start・draw-shm（600 frame）・draw-vk（300 frame）・`rotate-90: PASS turned`・`rotate-0: PASS turned`・bake・`game: PASS the game mode asked and left`・shot・stop・errors の全部 PASS、最後の行 `qemu-render-wait: PASS` |
| 起動 | `OUTPUT=build/p5-boot-test BOOT_TEST_WORK=$PWD/build/tmp/p5-boot-work BOOT_TIMEOUT=240 timeout 600 plan/tools/boot-test.sh build/x86_64/hdd-image.img`（上と同じ image、uefi-nvme） | `boot-test: PASS build/p5-boot-test/login.png`（画面の login prompt を読んだ） |

判定は試験の script の要約（guest の file の行を serial の command で数えた物）と画面の読み取りで、console・serial の log は読んでいない。venus の driver は `DRV_GPU_DISPLAY_ROTATE_180` を立てないので、この回帰は「印の無い driver で present・commit・回転（compositor の turn pass）の挙動が変わらない」事の確かめ。180° の scanout そのものは p009b（i915）の後の実機。

## 4. 未実施

- 実機（範囲外。p009a は印を立てる driver が無いので、実機で変わる所は無い）。
- i386・pc9821 の build（参考の platform、変更ごとの確かめは要らない）。

## 5. 残り

p009b（i915 xd・xd9 が印を立てる）、p009c（libvulkan）。載せ直しの見込みは [phase009b](../phase009b/phase.md)・[phase009c](../phase009c/phase.md) の §5。
