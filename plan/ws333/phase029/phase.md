<!-- awesome-plan project=zedbsd record=ws333-p029 -->

# ws333-p029: i915 の GT を RC6 に入れる（HW_CONTROL で Vulkan が DEVICE_LOST になる原因と、forcewake の参照の数の性能）

Status: in-progress（2026-11-13 Q1、q2037-i01 P3。考える Phase、実機は最後に。結果は下、cleared の判定は Q1）
Parent: [WS333](../ws.md)

## 目的

[p027](../phase027/phase.md) の続き。5330 の package の C-state（PC2）には、GT が RC6 に入る事と tick を下げる事の両方が要る（tick は 2026-11-13 に amd64 を 100 Hz にした）。p027 の結果:

- forcewake を参照の数で握る形（agent/p3-ws333b 6e208bfd3b）と RC_CONTROL の HW_CONTROL|EI_MODE|RC6_ENABLE で、GT RC6 99.9 %・PC2 85〜88 %・4.34 W。
- ただし HW_CONTROL を入れると qs40 で Vulkan が DEVICE_LOST。p027 は RC6_ENABLE だけに戻した（GT は RC6 に入らない）。
- forcewake の直しだけで qs40 c1 が 338 → 163 fps（1 回ずつの数、未確かめ）、c600 は 39.30 → 37.35 fps。

p027 の i915 の source の直しは、性能の回帰の疑いがあるので main に merge していない。

## 解析する事（code と PRM で根拠を書く）

1. HW_CONTROL で RC6 に入った後に GT の仕事が失われる理由: RC6 の出入りで要る物（render の power context の置き場、RC6 から戻る時の待ち、RC の各 threshold・EI・idle の値、context の save/restore、forcewake を握らずに触っている register、GuC の無い時の ring の扱い）。PRM の RC6 の節と、値の出典（guardrail の GPL の値の規則）。
2. forcewake の参照の数の形の性能: worker が 20 ms で返す・RPS の hook・telemetry の一瞬の wake が描画の流れで何回 wake を起こすか。c1 の fps の差が本当か（複数回の測り）と原因。本来の形（Linux の i915 のように仕事の有る間は握り、idle で離す、等）を考えて直す。
3. 直した後に 5330 で: qs40 の c1・c600 が直す前と同等（DEVICE_LOST 無し）、普段の起動の idle で GT RC6・PC2 が出る。全部一回だけの起動と watchdog の下。

## 受け入れ

- DEVICE_LOST の原因が根拠付きで分かり直っている。qs40 の c1・c600 が直す前の ±5 % の中（数回の測りの平均）。idle の GT RC6 と PC2 が p027f と同等。build warning 0、host 試験 PASS。

## 結果（2026-11-13 P3、q2037-i01）

### 1. DEVICE_LOST の原因 → HW_CONTROL ではない。起動直後の display の takeover が worker を約 14 s 塞ぎ、GPU の job の予約の期限 10 s を越える

- 仕組み（code）: libvulkan は vkQueueSubmit を監督つきの job で送る（`userland/desktop/libvulkan/queue.c`、`context.c` の GPU_JOB_SUBMIT、i915 は jobs の command を持たないので GPU_JOB_RESERVE → GPU_COMMAND → commit）。`src/drivers/gpu/gpu.c` は予約から `CONFIG_GPU_JOB_RESERVATION_MS`（`mk/config.mk` 10000）を過ぎた job を ETIMEDOUT にする（監視の thread の `gpu_session_fail_locked(session, ETIMEDOUT, 1U)`、stream の後の `gpu_job_submit_finish()`）。libvulkan の「decoder failed status=42」はこの ETIMEDOUT。i915 の request worker は一度に 1 つの仕事しか走らせず、最初の presentation で display の window に入り、その中で firmware の表示の takeover・点灯・survey をする（`worker.c` `i915_worker_loop()` の ENTER_DISPLAY → `display/present.c` `drv_i915_present_window()`）。その間、後ろの batch は待つ。
- 実測（試験だけの診断 `plan/ws333/tests/hw5330/latency-diag.py`、worker の各仕事の待ちと実行の時間）: p029hw（HW_CONTROL あり、f958637790369f0b）の r1 で compositor の最初の PRESENT_BLOB が 14540 ms 待ち 629 ms で実行、その後ろの batch が 15776 ms・8589 ms 待ち → compositor の session が落ち（lease 1 は 1 frame で解放）、mview c1 は止まり c600 は socket 無し。**HW_CONTROL 無し**の p029fix（46eb1f8c8739f8b2）の r1 でも PRESENT_BLOB 14036 ms 待ち、batch 13381 ms・11612 ms 待ちで、mview c1 が `vkQueueSubmit` DEVICE_LOST（errno 42）を**再現**した。直す前の i915 そのもの（p027q4 と同じ kernel bbdfdd69a3b8667f、p029base2）でも最初の present が 13710 ms、compositor の lease 1 が 1 frame で解放され、最初の mview は結果を出さなかった。p027 の q4・q6（HW_CONTROL 無し）も最初の present に 13〜16 s かかっており（perf の行）、通ったのは batch の並び方による偶然。p027 の「HW_CONTROL で DEVICE_LOST」は誤り。
- window の入りが 14 s かかる理由: kernel の text console は最初の flip が受け付けられる（ioctl が返る）まで描き続け（`gpu.c` `gpu_console_end()` は ioctl の後）、boot framebuffer は uncached で map されている（`src/drivers/platform/pcat/graphics/backend.c` の `kern_device_map(..., KERN_DEVICE_UNCACHED, ...)`）。下の行に 1 行足すたびに scroll で 1920x1080 のほぼ全部の cell を描き直す。qs40 の image の zedbsd.cfg には `kmsg=quiet` が無い（普段の image にはある）ので、takeover・点灯・survey が出す約 50 行が全部描かれる。survey の約 30 行（register を読んで log するだけ）に約 8 s（1 行 約 270 ms、診断の時刻）。普段の image（p027f）の最初の present は平均 95 ms。
- RC6 で失われる状態の確かめ（PRM）: DG1 PRM Vol. 13（IHD-OS-DG1-Vol 13-2.21、Gen12 Xe-LP）の「Global State」「Power Context Image」: engine の global state（GFX_MODE、HWSTAM、IMR、EXECLIST_SQ_CONTENTS、CTXT_ST_PTR・CTXT_ST_BUF、RING の各 register）は power-off/on で power context に save/restore される。同 Vol. 13 の Force Wake の表: render の CS（0x2000〜0x26FF）は RENDER、RC の register（0xA000〜0xAFFF）は GT の domain で、寝ている domain への access は終端される。driver は GT の register に worker・RPS・telemetry の保持の中でだけ触り（保持の外の held accessor の拒みは 0 件）、request は submit から CSB の完了まで worker の保持の中で走る（Linux v7.2 `intel_execlists_submission.c` の `execlists_schedule_in/out` が context の active の間 engine の forcewake を握るのと同じ範囲）。HW_CONTROL のまま c1・c600 を 4 round（p029final）と c1 1 回（p029hw r2）走らせ、失われた submit は無い。RC6 で失われる状態は見つからない（無いと言える根拠は上の save/restore の表と実測）。
- 直した事（62d20b5099）:
  - `display/present.c` `drv_i915_present_window()`: resident の run（takeover）の前に `kern_text_suspend()`。takeover の後は console の絵は表示されず、描くのは uncached の全画面の描き直しの無駄。console は最初の flip で `gpu_console_end()` が終わらせる。firmware の出力の run が失敗した時だけ `kern_text_resume()`（終わった console には何もしない）。出典「Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/i915_driver.c (i915_driver_hw_probe …)」（driver が display に触る前に firmware の framebuffer の console を外す、`drm_aperture_remove_conflicting_pci_framebuffers()`）。
  - `gt-power.c`: RC_CONTROL を HW_CONTROL | EVALUATION_MODE_1 | RC6_ENABLE に戻した（出典の版を v6.8.12 に、`intel/gt-power.h` も）。
- 直した後（p029final、e53d7a3608969911、4 round、kmsg=quiet 無しの同じ image）: DEVICE_LOST 無し、最初の lease の present は 8 回で平均 189 ms（直す前 13〜16 s）、flip の失敗・「did not complete」無し。

### 2. forcewake の参照の数の性能 → 回帰は無い。p027q6 の c1 163 fps は mview が直接の scanout から外れて合成された run

- p027 の c1 338 → 163 fps は 1 回ずつの数。同じ image で 1 回の起動に複数 round を測った（`5330-p029-qs40.sh`、mview 12 s、mailbox 4 images）。基準は直す前の i915（p027q4 と同じ kernel bbdfdd69a3b8667f、と同じ i915＋診断 68ce8313387c19d4）、比べるのは直した tree（e53d7a3608969911: 参照の数＋HW_CONTROL＋console）:

| run | kernel | c1 fps | c600 fps |
| --- | --- | --- | --- |
| p029nofix | 直す前＋診断 | 292.58 / 230.17 / 316.75 / 280.00 | 39.63 / 42.33 / 41.42 / 42.01 |
| p029base2 | 直す前（bbdfdd69a3b8667f） | （最初の c1 は起動の race で compositor が落ちて無し）258.00 / 266.67 / 217.67 / 293.33 / 265.92 | 42.88 / 42.42 / 42.30 |
| p029final | tree | 275.33 / 241.67 / 251.50 / 280.08 | 43.05 / 42.71 / 42.92 / 42.38 |
| p029tree2 | tree | （148.33、下の合成）270.00 / 278.50 / 289.25 / 273.58 / 273.92 | 42.18 / 42.83 / 42.38 |
| p029fix | 参照の数＋診断、HW_CONTROL 無し | （r1 は DEVICE_LOST）239.00 / 282.75 | 43.05 / 42.80 |
| p029hw | 参照の数＋HW_CONTROL＋診断 | （r1 は DEVICE_LOST）295.08 | （r2 は §4 の flip の失敗） |

- 平均（直接の scanout の run）: **c1 直す前 269.01（9 回、標準偏差 31.3）→ tree 270.43（9 回、14.8）、+0.5 %。c600 直す前 41.86（7 回）→ tree 42.64（7 回）、+1.9 %。** ±5 % の中。
- c1 の外れ値の原因（Keiland の session.log）: p027q6 の c1 は mview（client 4）が一度 `KWL SCANOUT direct=1` になった後に `direct=0 reason=not-fullscreen` で合成に戻り、compositor が 5 s に 300 frame を合成していた（`KWL PERF compose frames=300`）。p029tree2 の最初の c1（148.33）は mview が desktop の起動の最中に繋がり（session.log の 73 行目、他の run は 230〜253 行目）、`KWL SCANOUT` が一度も無く合成された（mview の present 2.72 ms、q6 は 2.92 ms）。直接の scanout の run（p027q4 を含む）の present は 0.04〜0.09 ms。compositor の側の条件で、i915 の forcewake の形とは関係しない。
- c1 の round ごとの差は GT の周波数（RPS の software の busy の割合が 85〜95 % の帯で round ごとに違う所に落ち着く）に沿う: 要求の周波数の平均と fps は、直す前 742→292.6、680→316.8、608→280.0、459→230.2、tree 588→275.3、578→280.1、505→251.5、464→241.7、参照の数 600→282.8、465→239.0（実の周波数 CAGF は診断の kernel で要求とほぼ同じ）。同じ周波数なら kernel によらず同じ fps。
- 参照の数の形は「仕事の有る間は握り、idle で離す」になっている: worker は仕事を走らせる前に保持し、仕事が無くなって 20 ms で返す（Linux が context の active の間 engine の forcewake を握り、仕事が無くなって GT を park するのと同じ考え）。c1・c600 の間は rest 0〜2 回・wake 0〜2 回／5 s（診断）で、GT は保持されたまま。直す必要は無い。

### 3. 普段の起動の idle（p029idle、5330-p027.sh NOPARK=1 QUIET=1 PARTS=held、tree の kernel e53d7a3608969911、普段の NVMe の zedBSD）

| 窓（60 s） | package | PC2 | GT RC6 | forcewake の保持 |
| --- | --- | --- | --- | --- |
| held | 4.395 W | 86.0 % | 99.9 % | 0 |
| 2 回目（同じ状態） | 4.388 W | 87.3 % | 100.0 % | 0 |
| quiet（Wi-Fi 落とし・audiod 停止） | 4.339 W | 87.9 % | 99.9 % | 0 |

p027f（4.32〜4.36 W、PC2 85.3〜88.5 %、RC6 99.9 %）と同等。

### 4. 見つけた事（この Phase の範囲の外、Q1 へ）

- **flip の失敗が 1 回**: p029hw r2 の c600（HW_CONTROL＋診断）で「display run: error (cleanup): the update crossed a vertical blank」→「flip to 0xbc0c0000 failed: rc=-2 result=4」→ lease が解放され mview が EPIPE（398 frame）。`display/xd-screen.c` `drv_i915_xd_screen_update()` の割り込み off の arm の書きが vblank を跨いだ。p029final の 4 round と他の run では出ていない。HW_CONTROL との関係は未確認（1 回だけ）。
- **worker の待ちの 1 tick**: `worker.c` の `I915_WORKER_WAIT_MS`（1 ms、comment「a lost interrupt から 1 ms 以上を取らない」）は `kern_ms_to_ticks()` で 1 tick、100 Hz の tick では 10 ms。c600 で 5 s に 190〜460 回の待ちが割り込み無しに期限切れ（診断の tmo、直す前の i915 でも同じ）。c600 の frame（約 23 ms）に効いている見込み（未確かめ）。
- **起動の race は残る**: worker が一度に 1 つで、display の window の入りが長い間、他の session の job が 10 s の期限に近づく構造は変えていない。console を止めて入りは 1 s 未満になった。

### 確かめ

- build: tree の kernel（x86_64、THERMAL・X86_POWER_CONTROL、100 Hz）rc 0・warning 0、e53d7a3608969911（`kbuild.sh`）。診断の kernel 5 本（46eb1f8c8739f8b2・f958637790369f0b・68ce8313387c19d4・e784bb51e0376e79・defe8f9bd52c533a）と比べの kernel（直す前 bbdfdd69a3b8667f、p029 の前の tree 97c8f6f5d4be750c）も rc 0・warning 0。
- host 試験: `plan/ws333/tests/rc6-host/run.sh` PASS（81 checks、ordinary・ASan/UBSan）。`plan/tools/i915-display/run-xd-host-tests.sh` の display host 試験 31 本 全 PASS、`check-display-steps` PASS、`check-sources` は最初 present.c の出典の版（v7.2）を拒み、v6.8.12 に直して PASS。
- 未実施: i915 の `tests/contracts/run.sh`（一時 directory を rm する script）。

### 実機の状態

- 5330 は毎回 Debian（Linux 6.19.13+deb13-amd64）で始まり Debian に戻った。zedBSD の起動は全部一回だけ（UEFI BootNext）で `watchdog=600`。各 run の最後に p4 の vmunix 4c98d617279bf96b・zedbsd.cfg 1b33a395b0ff4bd7 に戻して sha256 を照合した（p029hw・fix・nofix・final・idle・base2・tree2）。
- qs40 の run は NVMe の p4・p5・p6・ESP の loader を書くので、写しから書き戻して sum を比べた（p4 の log/ は image の起動の log が増えるので比べない）: p029hw・fix・nofix・final は p027 の写し `~/ws245/p027-nvme-p027q6` と一致。p029idle（普段の NVMe の zedBSD を起動）で p5 が変わった（sha256 7eee56c4f80e0527 → 0f4de9e4a34ac1a1、普段の zedBSD の起動の書き込み）ので、p029base・p029tree は書く前に比べで止まり（何も書いていない）、script を「写しが無ければ作る」に直して `~/ws245/p029-nvme-p029` に写しを取り直した。p029base2・tree2 はこの写しと一致。
- 最後の状態: Debian、p4 は vmunix 4c98d617279bf96b・zedbsd.cfg 1b33a395b0ff4bd7（p029tree2 の後に照合）、NVMe の p5 は 0f4de9e4a34ac1a1（p029idle の後の普段の zedBSD）。

### 消してよい物（Q1 へ）

- worktree: `build/p029/qs40.img`（image の写し 2.5 GB）、`build/p029/revert-fix.patch`（使わなかった）、`build/p029/k-*`（試験の kernel）。
- Debian: `~/ws245/vmunix-p029-*`（試験の kernel）、`~/ws245/p029-nvme-p029`（NVMe の写し 3.2 GB、書き戻しは済み）、`/tmp/p029-logs.tar`、`~/p1-p022/image.img`（書いた image の写し）。

