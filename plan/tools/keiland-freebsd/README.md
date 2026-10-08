# Native FreeBSD Keiland regression probes

## FreeBSD 15.1 の試験の guest と backend の試験（WS137）

T1・T2 と実装の担当が、自分の worktree で FreeBSD の native の build と試験を流すための道具。guest は QEMU+KVM、
i915 の passthrough は使わない（virtio-vga は QMP の screendump だけ、GPU は無い）。SSH は `127.0.0.1` の転送ポートだけ、
serial は繋がず、console・serial の log を判定に使わない（AGENTS.md の WS109 の例外）。全ての command に `timeout` を付ける。

```sh
timeout 2400 sh plan/tools/keiland-freebsd/build-guest.sh            # 既定 OUT=build/keiland-freebsd/guest、既存は再利用
timeout 300  sh plan/tools/keiland-freebsd/guest.sh start
timeout 60   sh plan/tools/keiland-freebsd/guest.sh ssh 'freebsd-version -ku; pkg info -q gmake seatd'
timeout 60   sh plan/tools/keiland-freebsd/guest.sh shot "$PWD/build/keiland-freebsd/console.png"
timeout 5400 sh plan/tools/keiland-freebsd/backend-test.sh           # 動いていなければ start し、終わると stop
timeout 120  sh plan/tools/keiland-freebsd/guest.sh stop
```

- `build-guest.sh [--force] [OUT]`: 公式の `FreeBSD-15.1-RELEASE-amd64-BASIC-CLOUDINIT-ufs.qcow2.xz` と `CHECKSUM.SHA256`
  （`download.freebsd.org/releases/VM-IMAGES/15.1-RELEASE/amd64/Latest/`）を取得し、image の行が WS109 の記録
  [q550/CHECKSUM.SHA256](../../history/ws109/q550/CHECKSUM.SHA256) と同じこと、取得物の SHA256 がその値であることを確かめる。
  OUT に guest 専用の SSH の鍵（`id_ed25519`）と NoCloud の seed（`seed.iso`: root と `kei`（wheel・video）に鍵だけで login）を作り、
  image を 24 GiB にして一度起動し、初回の起動（growfs・nuageinit・初回の pkg の upgrade と再起動）が終わるのを SSH で待つ
  （`/firstboot` が消え boottime が 30 秒変わらない）。`pkg install` で native の build の package（README.freebsd.md の一覧と pkgconf）
  を入れ、`packages.txt`（名前・版・license）・`versions.txt`・`pkg-install.txt`・`first-boot.png` を残して止め、`guest.qcow2` にする。
  準備の guest は `OUT/prepare` と port 2236（`PREPARE_PORT`）を使い、動いている試験の guest と重ならない。
- `guest.sh start|stop|status|ssh|put|get|copy|shot`（`guest.py`）: 起動ごとに `GUEST_RUN`（既定 `build/keiland-freebsd/run`）に
  overlay を作り、`guest.qcow2` は読み取り専用の base（他の checkout の image も `GUEST_DIR` で使える。鍵は `GUEST_DIR/id_ed25519`）。
  既定 `SSH_PORT=2235`、`GUEST_MEMORY=8G`、`GUEST_CPUS=8`、KVM は [qemu-accel.sh](../guest/qemu-accel.sh)。
  `ssh 'CMD'` は root（`SSH_USER=kei` で利用者、上限 `GUEST_COMMAND_TIMEOUT` 秒、既定 120）。`copy DEST PATH...` は working tree の
  追跡中と無視されない file を tar で guest の DEST（専用の directory、先に消す）へ写す。`shot PNG` は QMP の screendump。
  2 つの guest を同時に動かす時は `GUEST_RUN` と `SSH_PORT` を変える。
  音の試験の時だけ `GUEST_AUDIO_WAV=PATH` で intel-hda と hda-duplex を足し、出力を WAV（48 kHz・2 ch・S16）に書く（WS191 p004。既定は音の device 無し）。
- `backend-test.sh [OUT]`: tree（build に要る path と `plan/ws131/tests`、`BACKEND_TEST_EXTRA_PATHS` で追加）を guest の
  `/root/keiland-src` に写し、base の clang で `keiland-freebsd.mk all`（exit 0 と `warning:` 0）、DESTDIR の install と
  `header-dependencies`、`native-build-audit.py`、`plan/ws131/tests` の host-seat-freebsd・host-session・host-power（ASan・UBSan）、
  `sync-rejected.c`・`dmabuf-export-rejected.c` を流す。step ごとの log と `summary.txt`（PASS/FAIL/SKIP）は OUT
  （既定 `build/keiland-freebsd/backend-test`）。全部 PASS の時だけ exit 0。build の取得物（seatd・emoji・辞書）は guest の
  `/root/keiland-distfiles` に残り、Makefile の SHA256 で毎回確かめる。GPU・seat・窓の probe（下の節）はこの guest では流せない。

Retained from WS109's actual FreeBSD15.1/i915 QEMU acceptance. These do not launch a VM,
change host devices or supply fake GPU/service providers. Run only in an explicitly owned,
prepared native guest; compile from the repository root with its native headers/Clang19.
Native build/install instructions: [FreeBSD guide](../../../userland/desktop/README.freebsd.md).
Full C style and [native scope](../../standards/ws109-native.md) apply. Actual acceptance receipts:
[q568](../../history/ws109/q568/result.md), [q569](../../history/ws109/q569/result.md),
[q570](../../history/ws109/q570/result.md), [q572](../../history/ws109/q572/result.md).

## Build/header/ELF audit

After `all install header-dependencies` with the same independent native build and DESTDIR:

```sh
python3.11 plan/tools/keiland-freebsd/native-build-audit.py build/native-final /root/keiland-stage-final/opt/keiland
```

It checks actual selected objects, native/system header families, private library SONAME/NEEDED
and install boundaries. No global loader or standard system headers are replaced.

## Native sync and Vulkan window probes

Compile `sync-rejected.c` with production `libvulkan-compat/freebsd/sync-freebsd.c`;
compile `dmabuf-export-rejected.c` with `libkeiland-backend-freebsd/sync-freebsd.c`.
Both use real native pipes/closed files and check output/error/borrowed-fd ownership.
`dmabuf-native-flags.c` reuses the registered real Vulkan exporter; link the production native
sync adapter plus staged `-l:libvulkan.so.1 -l:libwayland-client.so`, with `-I.`, the native build
include directory and `/usr/local/include`. Its real GPU zeroaccess query tests BUG-130's bounded
unavailable-ioctl adaptation; this does not resolve the upstream driver bug.
`wsi-window-freebsd.c` is a proper xdg-toplevel client; link the same two staged libraries, then
run `--frames 3 --timeout 5` against the actual native compositor. Require its positive
`wsi-probe-client: PASS` marker, not a tracing wrapper's exit code alone.

## Owned Intel GPU/seat and main-app fixtures

Compile `console-abi-freebsd.c` as `/tmp/ws109-console-abi`; it supplies installed native ioctl
constants to the independent capture/restoration controller. The fixtures expect the actual
Intel ICD, `/dev/dri/card0`, native seatd, and the reviewed staged prefix
`/root/keiland-stage-final/opt/keiland`. They refuse an existing `/opt/keiland` or seatd socket,
create only their own temporary installation/runtime/daemon, and restore captured console state
and input permissions. Run as root only inside that explicitly owned guest; the GUI runs as
nobody with supplementary video, without changing accounts/device permissions.

- `gpu-seat-fixture-freebsd.py`: actual wltest GPU window while VT1→2→1 retires/reacquires native
  leases. Polls actual fd generations with six-second limits. After `/tmp/ws109-lifecycle-ready`
  appears, inject QMP USB pointer/key events before the window ends; positive input/frame/exit
  evidence is required. A readonly DRM inquiry fd may remain while the primary lease is retired.
- `main-apps-gpu-freebsd.py`: seven main native apps and App Home. When
  `/tmp/ws109-app-terminal-ready` appears, send QMP keys `freebsd` followed by Enter. A real shell
  behind Terminal's native PTY must write exactly that text. Text/PNG/PDF fixtures are generated
  in its own runtime. `--only-desktop` checks just App Home's actual authenticated desktop role.
  The fixed desktop token is an ordinary supported compositor configuration; no production
  test-only switch is added. Fixtures and observer logs remain separate from QEMU console logs.

The test controller must inject through the owned QEMU's QMP endpoint; the guest scripts do not
open remote SSH connections or store credentials. The main-app GUI success and actual file-open
markers are individually checked; external hardware/radio tests remain user-waived, not tested.
