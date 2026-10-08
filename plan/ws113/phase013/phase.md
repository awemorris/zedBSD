<!-- awesome-plan project=zedbsd record=ws113-p013 -->

# ws113-p013: 内蔵の panel の明るさ（kernel の backlight の device・i915・backend）

Parent: [WS113](../ws.md)
Status: in-progress（2026-10-08 q902 P1 の照合: QEMU は T1-130 PASS。5330 の backlight-probe・30・100 の目視は未）（旧: in-progress（2026-10-05、P2。C2 はユーザーが許可済み（2026-10-05 未明）。UAPI の差分を下に示してから実装する））
Disposition: normal
Primary Milestone: MG006（WS から継承）
Queue: none
目安: 2〜3h

## 目的

2026-10-04 のユーザーの要望「SettingsのDisplayには、内蔵LCDの場合、明るさ調節がほしい」の下層: kernel の backlight の device と i915 の provider、libkeiland-backend の口（[契約の確定](../phase001/contracts-beta2.md) D-BRIGHT・D-BRIGHT-BOOT）。Settings の slider は p006、compositor の経路と Fn の key は p005。

## 範囲

- `include/uapi/backlight.h`（新規）: FreeBSD の backlight(9) と同じ形（`struct backlight_props`（brightness 0〜100、nlevels、levels[]）、`struct backlight_info`（name・type）、ioctl BACKLIGHTGETSTATUS・BACKLIGHTUPDATESTATUS・BACKLIGHTGETINFO）。kernel の `src/kern/` に backlight の class（provider の登録、`/dev/backlight/backlightN` の cdev）。
- i915: eDP の panel がある時に provider を登録（`panel-backlight.c` の `drv_i915_lcd_modeset_brightness` を 0〜100 で呼ぶ。panel が止まっている間は EBUSY）。起動の時の明るさは変えない。
- libkeiland-backend: `keiland-backend.h` に `kl_backend_backlight_open/get/set/close`、`libkeiland-backend-zedbsd/backlight-zedbsd.c`。Linux・FreeBSD は ENOTSUP の stub（p010 で sysfs・backlight(9)）。

## 受け入れ

host の試験（kernel の backlight の class の登録・ioctl の検査、backend の stub）、実機（5330）で `/dev/backlight/backlight0` の UPDATESTATUS で panel の明るさが変わる（小さい probe、ユーザーの目視）。QEMU（T1）は device が無いことと backend の ENOTSUP。build warning 0（vmunix の kernel include check まで、Linux の backend の build）、規約。

## 依存と衝突

依存: C2 の許可（p002 とは独立に始められる）。p005 がこれを使う。衝突: i915 の panel を触る Phase（WS075・WS084、BUG-159 の電源の調査）。

## 設計（2026-10-05、P2）

### UAPI の差分（新規 `include/uapi/backlight.h`、C2 の許可の範囲、HAL は不変）

FreeBSD の `sys/backlight.h` と同じ名前・同じ形の構造体と request。request の group は zedBSD の `'G'` が GPU で 0〜38 まで埋まっているので `'L'`（FreeBSD と番号の互換は求めない。名前と構造体で source が通る）。

```c
#define BACKLIGHTMAXLEVELS	100
#define BACKLIGHTMAXNAMELENGTH	64

/* brightness: 0..100 (percent).  nlevels: the levels the device has (0: any percent). */
struct backlight_props {
	uint32_t brightness;
	uint32_t nlevels;
	uint32_t levels[BACKLIGHTMAXLEVELS];
};

enum backlight_info_type {
	BACKLIGHT_TYPE_PANEL = 0,
	BACKLIGHT_TYPE_KEYBOARD
};

struct backlight_info {
	char name[BACKLIGHTMAXNAMELENGTH];	/* the provider: "i915" */
	enum backlight_info_type type;
};

#define BACKLIGHTGETSTATUS	_IOWR('L', 0, struct backlight_props)
#define BACKLIGHTUPDATESTATUS	_IOWR('L', 1, struct backlight_props)
#define BACKLIGHTGETINFO	_IOWR('L', 2, struct backlight_info)
```

- node: `/dev/backlight/backlightN`（FreeBSD と同じ）。devfs に固定の directory `backlight` を足し（`input` と同じ作り）、cdev の名前 `backlight<n>` はそこだけに出る。dev_t は major `0x000d`（空き）。mode は 0644（読むのは誰でも、UPDATESTATUS は書きで開いた時だけ。EBADF）。session の user には sessiond が gpu と同じく 0600 で渡す（`userland/desktop/sessiond/seat.c`）。
- UPDATESTATUS は `brightness` だけを読む（0〜100 の外は EINVAL）。0 は panel の最低（Linux の user level 0 と同じで、消灯ではない）。GETSTATUS は今の値と `nlevels = 0`。
- 誤り: provider が値を出せない時（panel が点いていない、HDMI が panel の代わり）EBUSY、provider が消えた後 ENXIO、driver の誤り EIO。

### kernel の backlight の class

- `include/kern/backlight.h`・`src/drivers/generic/backlight.c`: `kern_backlight_register(name, type, ops, ctx, &result)`・`kern_backlight_unregister(device)`。ops は `get(ctx, &percent)`・`set(ctx, percent)`（どちらも眠ってよい）。番号は空きの最小（4 まで）。ioctl は device の mutex の下で provider を呼び、unregister は mutex の下で provider を外してから cdev を unpublish する（その後の ioctl は ENXIO）。record は cdev の finalizer で放す。
- amd64 の kernel の source に足す（i915 が amd64 だけなので他の platform は今は足さない）。

### i915 の provider

- `src/drivers/gpu/i915/display/backlight.c`（新規）: node を publish した後、eDP の panel がある node（`rctx.lcd` があり、`output.none` でない）で `backlight0` を登録、unpublish の前に外す。登録の失敗は log だけ（node は続く）。
- get・set は worker に新しい同期の item `I915_WORKER_SYNC_BACKLIGHT` を渡して待つ（panel の modeset の状態は worker だけが触る）。worker は display の window の中で、HDMI が panel の代わりでない時に `drv_i915_lcd_modeset_brightness(display, percent, 100)`、読みは新しい中立の `drv_i915_lcd_modeset_brightness_get(display, 100, &percent)`（`panel-backlight.c`）。window の外（panel を driver がまだ点けていない、firmware の画面のまま）と HDMI の時は EBUSY。`I915_LCD_MS_NOT_PREPARED` → EBUSY、`I915_LCD_MS_ERRORS` → EIO。
- 起動の時の明るさは変えない（D-BRIGHT-BOOT）。

### libkeiland-backend

- `keiland-backend.h`: `kl_backend_backlight_open(&backlight)`（無い時 ENOENT、他は ENOTSUP）、`kl_backend_backlight_get(backlight, &percent)`、`kl_backend_backlight_set(backlight, percent)`、`kl_backend_backlight_close(backlight)`。
- zedBSD: `libkeiland-backend-zedbsd/backlight-zedbsd.c`（`/dev/backlight/backlight0` を O_RDWR、ioctl）。Linux・FreeBSD: `libkeiland-backend/unsupported/backlight-unsupported.c`（ENOTSUP、p010 で sysfs・backlight(9)）。

### 試験

- host: `plan/ws113/tests/host-backlight.sh`（kernel の class を host で build し、偽の provider で登録・番号・GETSTATUS・UPDATESTATUS・範囲の外・書きでない open・unregister の後の ENXIO を確かめる。backend の stub の ENOTSUP）。
- build: vmunix の link（kernel include check）、zedBSD の compositor の build、Linux の backend の build。
- QEMU（T1）: Venus の guest に `/dev/backlight` が空であること、backend の open が ENOENT。実機（5330）: 小さい probe で UPDATESTATUS が panel の明るさを変える（ユーザーの目視）。

## 実装（2026-10-05、P2）

| 部分 | file |
| --- | --- |
| UAPI | `include/uapi/backlight.h`（上の差分のとおり） |
| kernel の class | `include/kern/backlight.h`・`src/drivers/generic/backlight.c`（amd64 の kernel の source に追加） |
| devfs | `src/kern/devfs.c`: 固定の directory `backlight`（ino 6）、`backlight<n>` はそこだけ、mode 0644、root の一覧に `backlight` |
| i915 | `src/drivers/gpu/i915/display/backlight.c`・`.h`（provider、worker 側の `drv_i915_display_backlight_serve`）、`worker.c`・`worker.h`（`I915_WORKER_SYNC_BACKLIGHT`・`drv_i915_worker_sync_backlight`、登録は worker が node を publish して serve を始める時、取り下げは `drv_i915_worker_stop` の最初＝worker がまだ答える間。ioctl が worker を待ったまま取り下げが mutex を待つ deadlock を避ける）、`panel-backlight.c`・`.h`（`drv_i915_lcd_modeset_brightness_get`）、`internal.h`（`display->backlight`） |
| sessiond | `userland/desktop/sessiond/seat.c`: `/dev/backlight/backlight0..3` を seat の user に 0600、戻す時 root の 0644 |
| backend | `keiland-backend.h`（`kl_backend_backlight_open/get/set/close`）、`libkeiland-backend-zedbsd/backlight-zedbsd.c`（`sources.mk`）、`libkeiland-backend/unsupported/backlight-unsupported.c`（Linux・FreeBSD の Makefile） |
| 試験 | `plan/ws113/tests/host-backlight.sh`・`.c`（host）、`plan/ws113/tests/backlight-p013.sh`（guest）、`plan/ws113/tests/config-amd64-p013.mk`、`userland/tests/backlight-probe/`（実機の probe、試験の image だけ） |

## 確認（2026-10-05）

- `sh plan/ws113/tests/host-backlight.sh` PASS（ASan・UBSan、実の `cdev.c` と: 引数の拒否、番号 0〜3 と dev_t 0x000d000N、5 つ目は ENOSPC、空いた最小の番号の再利用、GETSTATUS（60、100 を超える答えは 100、nlevels 0）、UPDATESTATUS（read-only の open は EBADF、101 は EINVAL、40・0 を設定）、provider の EBUSY の伝達、GETINFO（i915・panel）、未知の request は ENOTTY、開いたままの取り下げで ENXIO・provider は呼ばれない・record は最後の参照で解放、allocation の漏れ無し。backend の stub は ENOTSUP）。
- build（warning 0）: amd64 vmunix（kernel include check PASS、amd64 vmunix check PASS）、zedBSD の `wayland`（backend）・`sessiond`・`backlight-probe`、Linux の `libkeiland-backend.a`（`keiland-linux.mk`、-Werror）。`plan/tools/keiland-os-boundary/check.sh` PASS。新しい file の style-check 違反 0、既存の file（devfs.c・worker.c・panel-backlight.c）は新しい違反 0（devfs.c は 2 つ減った）。
- 未実施: QEMU（T1: `config-amd64-p013.mk` の image で `backlight-p013.sh`、/dev/backlight が空の directory であること）、実機（5330: `backlight-probe`・`backlight-probe 30`・`backlight-probe 100` で panel の明るさが変わるのをユーザーが目視。compositor が一度描いた後＝driver が panel を点けた後に。その前は EBUSY が正しい）。

## T1-130（2026-10-05）

- `backlight-p013.sh` PASS（QEMU、Venus）。`backlight-probe` は `BACKLIGHT error=6 step=open`: zedBSD の errno 6 は ENOENT（Linux の番号ではない）で、期待どおり（panel の無い guest に backlight0 は無い）。probe は `reason=<strerror>` も出すようにした。
- 実機（5330）は ws159-p005 の UAT の追加の項目（Q1）。
