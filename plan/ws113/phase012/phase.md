<!-- awesome-plan project=zedbsd record=ws113-p012 -->

# ws113-p012: native の power と refresh の境界（VK_EXT_display_control の下層）

Parent: [WS113](../ws.md)
Status: in-progress（2026-10-08 q902 P1 の照合: Venus の分は T1-135 PASS（Q1 2026-10-05）。i915 の refresh 約 60 Hz・power off の 5330 の確認は未）（旧: in-progress（2026-10-05、P2。C2 はユーザーが許可済み（2026-10-05 未明）。UAPI の差分を下に示してから実装する））
Disposition: normal
Primary Milestone: MG006（WS から継承）
Queue: none
目安: 3h

## 目的

`VK_EXT_display_control` を全部実装するのに要る native の操作を、GPU の display の UAPI に 2 つ足す（[契約の確定](../phase001/contracts-beta2.md) D-EXT・D-UAPI、[native capability](../phase001/native-contract.md) §2・§3）。HAL は変えない。

## 範囲

- `include/uapi/gpu-display.h`: `GPU_DISPLAY_POWER`（version・size・display_id・generation・state ON/OFF/SUSPEND・reserved。lease の持ち主の衝突・旧 generation・切断を副作用の前に拒む）と `GPU_DISPLAY_REFRESH`（version・size・display_id・generation・cursor・timeout_ns → refresh の連番と時刻。lease 不要、pipe が止まっていれば境界を作らず timeout、旧 generation は ESTALE、GPU の offline は ENODEV）。GPU の core（`src/drivers/gpu/gpu.c`）の dispatch と driver の ops の口。
- i915: pipe の vblank の割り込み（`vblank.c`）で出力ごとの refresh の連番を短い lock で publish し waiter を起こす。power は出力の worker で serialize（OFF/SUSPEND は present を止め buffer を退役してから pipe を止める、ON は検証してから戻す）。power の変更で topology の sequence を進めない。
- Venus: refresh は今の仮想の clock の境界（仮想と log・capability で明示）、power は scanout の停止と再開。
- 0 の capability を値 0 と取り違えない。fake の 60Hz を作らない。

## 受け入れ

host の試験（GPU の core の dispatch・検査、i915 の vblank の連番の publish と wait の race、power の状態の遷移）、QEMU（T1、Venus）で refresh の wait が進み power OFF・ON で scanout が止まり戻る（native の probe）、実機（5330）で eDP の refresh の wait が 60Hz 程度で進む（p008 にまとめてよい）。build warning 0（vmunix の kernel include check まで）、規約。

## 依存と衝突

依存: p002、C2 の許可。p003 がこれを使う。衝突: GPU の core（`gpu.c`）を変える他の WS の Phase。

## 設計（2026-10-05、P2）

### UAPI の差分（`include/uapi/gpu-display.h`、C2 の許可の範囲、HAL は不変）

```c
#define GPU_CAP_DISPLAY_CONTROL		32768U	/* GPU_DISPLAY_POWER and GPU_DISPLAY_REFRESH */

/* gpu_display_info.flags (added) */
#define GPU_DISPLAY_POWER_CONTROL	64U	/* GPU_DISPLAY_POWER works on this output */
#define GPU_DISPLAY_POWERED_OFF		128U	/* the lease holder powered it off (OFF or SUSPEND) */
#define GPU_DISPLAY_REFRESH_COUNTER	256U	/* GPU_DISPLAY_REFRESH reports this output's refresh boundaries */
#define GPU_DISPLAY_LIMITED		512U	/* connected, but cannot be lit now: the limit of outputs shown at once (D-LIMIT) */

#define GPU_DISPLAY_POWER_ON		0U
#define GPU_DISPLAY_POWER_OFF		1U
#define GPU_DISPLAY_POWER_SUSPEND	2U

#define GPU_DISPLAY_POWER		_IOW('G', 39, struct gpu_display_power)
#define GPU_DISPLAY_REFRESH		_IOWR('G', 40, struct gpu_display_refresh)

/*
 * Powers the display this open leases on or off (VK_EXT_display_control).  Inputs only;
 * reserved is zero.  Needs a writable open and this open's lease on the display: EBUSY
 * otherwise (another open's lease, or no lease).  ESTALE for an old generation, ENXIO
 * when disconnected, EOPNOTSUPP when the output has no power control.  The lease's end
 * powers the output on again.  A power change is not a topology change.
 */
struct gpu_display_power {
	uint32_t version;
	uint32_t size;
	uint32_t display_id;
	uint32_t state;
	uint64_t generation;
	uint64_t reserved;
};

/*
 * Waits for the output's next refresh boundary after cursor (no lease needed, read access).
 * cursor 0 reports the current count at once.  Inputs: display_id, generation, cursor,
 * timeout_ns (the core bounds one call to 1 s); outputs: sequence (> cursor, a count that
 * never goes back), time_ns (the boundary's monotonic time, of the 1 ms scheduler clock)
 * and flags (GPU_DISPLAY_VIRTUAL_CLOCK when the boundary is a virtual clock's, not a
 * scanout's).  An output that does not scan out makes no boundary: ETIMEDOUT.  ESTALE
 * for an old generation, ENODEV when the GPU is offline.
 */
struct gpu_display_refresh {
	uint32_t version;
	uint32_t size;
	uint32_t display_id;
	uint32_t flags;
	uint64_t generation;
	uint64_t cursor;
	uint64_t timeout_ns;
	uint64_t sequence;
	uint64_t time_ns;
};
```

- driver の口（`include/drivers/gpu/gpu-display.h`）: `drv_gpu_display_ops` の末尾に `power(device, session, const struct gpu_display_power *)` と `refresh(device, session, struct gpu_display_refresh *)`。`GPU_CAP_DISPLAY_CONTROL` の時は両方が要り、無い時は両方とも NULL（登録の検査）。
- GPU の core: POWER は普通の session の admission の中（書き）。REFRESH は待つので EVENTS・COMMAND_WAIT と同じく admission の外で（device の offline と session の誤りを先に見て ENODEV）、timeout を 1 s に切る（libvulkan が繰り返す）。入力の検査（version・size・reserved・state・出力の欄が 0）は core。
- D-LIMIT: QUERY の `GPU_DISPLAY_LIMITED` を足す（claim の ENOSPC を先に知れる）。i915 は resident でない接続済みの出力に立てる（p011 の後は資源が足りない時だけ）。

### driver

- **Venus**: REFRESH は仮想の clock の境界（`GPU_DISPLAY_VIRTUAL_CLOCK`）: 出力が scanout している間（lease の画が出ている、power が ON）、boot からの tick と出力の refresh（millihz）で数を出す（単調）。scanout していない時は境界を作らず timeout。POWER は lease の持ち主だけ: OFF・SUSPEND は scanout を外し（SET_SCANOUT resource 0）、画（front・shared_front）は持ったまま、OFF の間の present は画を入れ替え sequence を進めるが scanout の選択と flush を host に送らない。ON は今の画で scanout を選び直す。lease の終わりで ON に戻す。
- **i915**: REFRESH は pipe の hardware の frame counter（`PIPE_FRMCOUNT`）を 1 ms ごとに読んで 64 bit に伸ばす（減った時は pipe の作り直しとして進めない）。vblank の割り込みは resident の run が unmask しない設計なので使わない。読むのは display の window が上がっている間だけ（短い lock で `display_up` と一緒に見る。worker は同じ lock の下で `display_up` を下ろすので、pipe を止めた後に読まない）。resident でない出力は点いていないので境界無し（timeout）。POWER は eDP の panel の backlight（`drv_i915_lcd_modeset_backlight`）を worker で切る・入れる（pipe は動いたまま。SUSPEND は OFF と同じ）。lease の画がまだ出ていない時は EBUSY、HDMI は `EOPNOTSUPP`（QUERY に `POWER_CONTROL` を立てない）。lease の終わりで ON に戻す。

### 試験

- host: `plan/ws113/tests/host-display-control.sh`（GPU の core を ws014 の fixture の stub と組み、偽の display backend で POWER・REFRESH の dispatch・検査・admission の外の REFRESH・capability の登録の検査）。
- QEMU（T1、Venus）: native の probe で REFRESH が仮想の 50 Hz 程度で進み、POWER OFF で scanout が外れ（PNG が黒）、ON で戻る。実機（5330）: eDP の REFRESH が 60 Hz 程度で進む、POWER OFF で panel が暗く ON で戻る（p008 にまとめてよい）。

## 実装（2026-10-05、P2）

| 部分 | file |
| --- | --- |
| UAPI | `include/uapi/gpu-display.h`（上の差分のとおり: `GPU_CAP_DISPLAY_CONTROL`、QUERY の flag 4 つ、`GPU_DISPLAY_POWER`・`GPU_DISPLAY_REFRESH` と構造体） |
| driver の口 | `include/drivers/gpu/gpu-display.h`（`power`・`refresh`、表の末尾） |
| GPU の core | `src/drivers/gpu/gpu.c`: 既知の capability、登録の検査（CONTROL と 2 つの op は揃って）、POWER は `gpu_display_ioctl` の中（書き・入力の検査・返事無し）、REFRESH は admission の外の `gpu_refresh_ioctl`（読み・出力欄が 0・timeout を 1 s に・offline は ENODEV・cursor 以下の答えは EIO） |
| Venus | `src/drivers/gpu/venus/display.c`（`display_power`・`display_refresh_boundary`・`display_scanout_disable`・`display_scanout_restore`、出力の `powered_off`、OFF の間の present は scanout の選択と flush を送らない、release で ON に戻し画を hold しない、console の画だけの primary も外し・戻す、QUERY の flag）、`venus.c`（capability） |
| i915 | `display/control.c`・`.h`（新規: frame counter の 64 bit への伸ばし、`display_up` と同じ lock、1 ms ごとの wait、power の lease の検査と worker、lease の終わりの戻し）、`display.c`（op の表、id・generation の検査、QUERY の flag、他の接続済みの出力に `LIMITED`、refresh の初期化）、`present.c`（`display_up` を refresh の lock で、release の前に light を戻す）、`backlight.c`・`.h`・`worker.c`・`worker.h`（worker の backlight の item を op 付きに: GET・SET・POWER）、`vblank.c`・`.h`（中立の `drv_i915_pipe_frame_read`）、`internal.h`（`rd->power_off`・`display->refresh`） |
| 試験 | `plan/ws113/tests/host-display-control.sh`・`.c`（host、ws014 の fixture を include）、`plan/ws113/tests/display-control-p012.sh`（T1）、`plan/ws113/tests/config-amd64-p012.mk`、`userland/tests/display-control/`（native の probe） |
| 他 | `plan/ws014/tests/gpu-test-fd.c` に `kern_text_progress_end` の stub（ws014 の GPU の core の host 試験が main で link できなくなっていた。直す前から壊れていた） |

## 確認（2026-10-05）

- `sh plan/ws113/tests/host-display-control.sh` PASS（ordinary と ASan・UBSan: 登録の検査（capability と op の不一致を拒む）、CONTROL の無い display は両方 EOPNOTSUPP、POWER の read-only は EACCES・state 3・reserved・id 0・size 違いは EINVAL で backend に届かない、SUSPEND が backend に届き EBUSY が返る、REFRESH の出力欄・generation 0 は EINVAL、timeout 5 s は 1 s に切られる、cursor 以下の答えは EIO、cursor 0 は今の数）。
- ws014 の GPU の core の host 試験 10 本（framework・scanout・sharing・placement・fence・job・supervision・fence-close・fence-reuse・fence-payload）PASS（`gpu-test-fd.c` の stub の後）。
- build（warning 0）: amd64 vmunix を release の config（i915）と p012/p013 の config（Venus）で（kernel include check PASS）、`display-control`。style-check: 新規 file は違反 0、既存 file（gpu.c・venus/display.c・i915 の display.c ほか）は新しい違反 0。
- 未実施: QEMU（T1: `config-amd64-p012.mk` の image で `display-control-p012.sh`）、実機（5330: `display-control` で eDP の REFRESH が 60 Hz 程度、POWER OFF で panel が暗く ON で戻る。p008 または ws159 の UAT に）。libvulkan の `VK_EXT_display_control` は p003。

## T1-133（2026-10-05）

- `display-control-p012.sh` は 1 行だけ FAIL ×2: claim の前の測り `refresh now boundaries=0 ... error=42`（ETIMEDOUT）。その時の QUERY の flags は 0x16f で ACTIVE（0x10）が無い: greeter と compositor を止めた直後で、Venus の出力は何も scanout していなかった（console の画もまだ無い）。ON の後は boundaries=76 virtual=1 で正しい。
- 判定: **driver が正しい**（scanout していない出力は境界を作らない、i915 の止まった pipe と同じ契約。VK_EXT_display_control でも消えている display に vblank は来ない）。試験の順が誤り: probe は present の後にも 1 秒測る（`refresh shown`）ようにし、試験は `shown` に 40〜80 を求め、`now` は「ETIMEDOUT か、画があれば 40〜80」を受ける。

Q1（2026-10-05）: QEMU（Venus）の分は T1-135 で PASS。i915 の分（refresh が約 60 Hz、power OFF で panel が暗くなり ON で戻る）は 5330 の UAT の後に判定。
