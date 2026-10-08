# Keiland Linux の試験の道具

WS105 の Debian 13 guest を作り、loopback SSH と QMP で操作する。host の画面・入力 device は使わない。host に `/opt/keiland` を install しない。
SSH は `127.0.0.1:2225` → QEMU guest の port 22。試験専用の鍵は ignored build directory に保存する。
全ての試験 command に `timeout` を付ける。host の Vulkan 試験は `KEILAND_DRM_DEVICE=none` と `env -u WAYLAND_DISPLAY -u DISPLAY` を使う。

## Guest の作成・起動・確認

repo root で実行する。

```sh
timeout 600 sh plan/tools/keiland-linux/build-guest.sh
timeout 200 sh plan/tools/keiland-linux/guest.sh start
timeout 30 sh plan/tools/keiland-linux/guest.sh ssh 'uname -r; ls /dev/dri/card0 /dev/snd/controlC0; ls /dev/input/'
timeout 30 sh plan/tools/keiland-linux/guest.sh ssh 'env -u WAYLAND_DISPLAY -u DISPLAY vulkaninfo --summary'
timeout 30 sh plan/tools/keiland-linux/guest.sh ssh 'modprobe mac80211_hwsim radios=2; iw dev'
timeout 30 sh plan/tools/keiland-linux/guest.sh ssh 'amixer -c 0 scontrols; id kei'
timeout 30 sh plan/tools/keiland-linux/guest.sh screenshot "$PWD/build/keiland-linux/console.png"
timeout 30 python3 plan/tools/keiland-linux/png-probe.py --size build/keiland-linux/console.png
timeout 30 python3 plan/tools/keiland-linux/png-probe.py build/keiland-linux/console.png 0 0 100 100
timeout 30 sh plan/tools/keiland-linux/guest.sh key a
timeout 30 sh plan/tools/keiland-linux/guest.sh click 100 100
timeout 45 sh plan/tools/keiland-linux/guest.sh stop
```

`build-guest.sh [--force] [OUT] [base|gdm]` は既存 image を再利用する。gdm の既定 OUT は `build/keiland-linux/guest-gdm`（OUT を空文字にして variant を指定）。共有 image の `--force` は main だけが利用中の guest が無いことを確認して使う。
image は raw ext4、8 GiB。mmdebstrap unshare mode なので sudo 不要。`packages.txt` に package の版を保存する。

## 操作

`guest.sh` は `guest.py` を呼ぶ。既定 `GUEST_DIR=build/keiland-linux/guest`、`GUEST_IMAGE=$GUEST_DIR/guest.img`、`GUEST_RUN=build/keiland-linux/run`、`SSH_PORT=2225`。音の試験の時だけ `GUEST_AUDIO_WAV=PATH` で HD Audio の出力を WAV（48 kHz・2 ch・S16）に書く（WS191 p004。既定は `-audiodev none`）。
path は絶対 path に変換する。起動ごとに overlay を作り、停止後に消す。base image は読み取り専用。

| Command | 操作 |
| --- | --- |
| `start` / `stop` / `status` | SSH ready（180 秒上限）まで待つ / poweroff・QMP quit / process と SSH の状態 |
| `ssh 'COMMAND'` | root で command（既定 120 秒上限、`GUEST_COMMAND_TIMEOUT` で指定） |
| `put SRC DST` / `get SRC DST` | file を guest に送る / 受け取る |
| `screenshot PNG` | QMP screendump（絶対 path、RGB PNG） |
| `key ctrl alt f2` | 全 key を押し、逆順で離す。super は `meta_l` |
| `type 'text'` | 英小文字・数字・空白・`-`・`.`・`/` を入力 |
| `move X Y` / `click X Y [left\|right\|middle]` | screenshot の実寸で絶対座標に変換して入力 |

`SSH_USER=kei` で非 root の利用者を指定する。起動・停止には既定の root を使う。
別 guest は run directory と port を変える。

```sh
timeout 200 env GUEST_RUN="$PWD/build/keiland-linux/run2" SSH_PORT=2226 sh plan/tools/keiland-linux/guest.sh start
timeout 45 env GUEST_RUN="$PWD/build/keiland-linux/run2" SSH_PORT=2226 sh plan/tools/keiland-linux/guest.sh stop
```

## Install

`install-guest.sh [STAGE]`（既定 `build/keiland-linux/stage`）は `STAGE/opt/keiland` を guest の `/opt/keiland` に置き換える。
`STAGE/usr/share/wayland-sessions/keiland.desktop` があれば gdm の session file も送る。host の install tree は変えない。

```sh
timeout 180 sh plan/tools/keiland-linux/install-guest.sh build/keiland-linux/stage
```

## zedBSD の回帰

[zedBSD の検証手順](zedbsd-commands.md)（WS104 から移した）を参照する。Linux guest の boot 確認は SSH と PNG、zedBSD は `plan/tools/boot-test.sh`。
QEMU serial / console log を受け入れ判定に使わない。

## Linux build の確認

```sh
timeout 180 make -j16 keiland-linux
timeout 180 make -j16 keiland-linux CC=clang KEILAND_LINUX_BUILD=build/keiland-linux-clang
timeout 60 make keiland-linux-install DESTDIR="$PWD/build/keiland-linux/stage"
timeout 30 sh plan/tools/keiland-linux/elf-check.sh build/keiland-linux/stage
timeout 30 sh plan/tools/keiland-linux/makefile-sync.sh
timeout 120 sh plan/tools/keiland-linux/header-check.sh
```

`elf-check.sh` は全 ELF の RUNPATH、library の SONAME、我々の NEEDED の存在を確認する。`KEILAND_PREFIX` を変えた build では同じ変数を export して確認する。
`makefile-sync.sh` は package ごとの source token を双方向に比較し、zedbsd / linux / wpa と説明付き skip / only を扱う。
`header-check.sh` は system header も含む `-M` を全 source に行い、system の Wayland / EGL / GLES の混入を検出する。`CC` と `KEILAND_LINUX_BUILD` を export して別 build を指定できる。
`lib-smoke.c` は libkeiland の version 22 と `kl_system_*`（display 無しは EINVAL、compositor が動いていれば Keiland の拡張か ENOTSUP）の確認用（ws131-p011。libkeiland は OS に触れない）。staged の libkeiland.so と libwayland-client に link する。

## Vulkan chain の確認

`vk-chain-test.c` を staged libvulkan.so.1 に DT_NEEDED で link し、空の XDG_RUNTIME_DIR で走らせる。
Vulkan 1.0 + KHR_get_physical_device_properties2 の instance から device / queue を作り、1 MiB の fill / copy / fence / 全 word 一致と、WSI の禁止名を確認する。
`interpose-check.sh` は `LD_DEBUG=bindings` の既定の backend → compat binding が0件であることと、NO_DEEPBIND opt-out でも command が通ることを確認する。

```sh
mkdir -p build/keiland-linux/test/empty-xdg
chmod 700 build/keiland-linux/test/empty-xdg
timeout 30 cc -std=gnu17 -Wall -Wextra -Werror -o build/keiland-linux/test/vk-chain-test plan/tools/keiland-linux/vk-chain-test.c -ldl \
  -Wl,--no-as-needed -Lbuild/keiland-linux/lib -l:libvulkan.so.1 -Wl,-rpath-link,build/keiland-linux/lib -Wl,-rpath,/opt/keiland/lib
timeout 60 env -u WAYLAND_DISPLAY -u DISPLAY KEILAND_DRM_DEVICE=none XDG_RUNTIME_DIR="$PWD/build/keiland-linux/test/empty-xdg" \
  LD_LIBRARY_PATH="$PWD/build/keiland-linux/stage/opt/keiland/lib" build/keiland-linux/test/vk-chain-test
timeout 150 sh plan/tools/keiland-linux/interpose-check.sh
```

再入の試験は `fake-backend.c` を gcc `-fPIC -shared` で build する（Bsymbolic・fno-semantic-interposition は付けない）。
直接の再帰 C call は gcc が local alias に結び付けることがあるため、fake は同じ `vkCreateInstance` assembler symbol への extern alias から PLT を呼ぶ。
`VK_CHAIN_REENTER=1` は試験の program にだけある入口。NO_DEEPBIND と fake の absolute backend path で実行すると、production の再入検出が診断して exit134 を返す。

## Wayland WSI の host 試験

`timeout 120 bash plan/tools/keiland-linux/wsi-check.sh`。gcc/clang の build と DESTDIR install の後に実行する。
`KEILAND_LINUX_BUILD` と `STAGE` で独立 build を指定できる。system の Wayland server は試験 fixture のみ、client は我々の libwayland/libvulkan を使う。
FIFO / CPU fallback / 30 frame ごとの resize / MAILBOX を各90 frame。全画素・サイズ、通常 IMPORT_SYNC_FILE 90回成功、fallback ENOTTY 1回と以後再試行無し、CPU completion wait を assert。
raw fence/timeline は加工せず記録する。空/完了時の kernel stub が1になるため、raw count だけで経路を区別しない。
`sync-unavailable.c` は試験 LD_PRELOAD fixture、compile 時指定で ENOTTY を供給する。production の試験環境変数は作らない。
log は build/test/client-*.out / probe-*.out。X/WAYLAND の host 接続は使わず DRM=none。各 process 90秒、内部deadline60秒。

## KMS の確認

`display-probe.c` は guest 専用。`--acquire` は card を最初の Vulkan call 前に開き、取得後に元 fd を閉じる。direct は library が master を取得する。1280×800 の赤・緑・青を各5秒表示し、赤→緑では oldSwapchain を更新して旧 chain を破棄する。終了で CRTC を戻す。`vkdemo --time-ms=1000 --hold=10` は描画の確認用。host で KMS 試験を実行しない。

```sh
cc -std=gnu17 -Wall -Wextra -Werror -o build/keiland-linux/stage/opt/keiland/bin/display-probe plan/tools/keiland-linux/display-probe.c \
  -Lbuild/keiland-linux/lib -l:libvulkan.so.1 -Wl,-rpath-link,build/keiland-linux/lib -Wl,-rpath,/opt/keiland/lib
```

`flip-delay.c` は guest専用のtest-only preload。1回だけDRM pollを250ms遅らせてtimeout0を返す。旧100ms総期限ではOUT_OF_DATE、修正後は次のreal pollで実際のeventを読んで3色PASS。productionの設定を追加せず、poll≤100ms/総期限5sの道を確かめる。

## Linux compositor の dma-buf bounds probe

`dmabuf-forge.c` は我々の Vulkan frontend で本当の64×64のdma-bufをexportし、同じfdの高さだけ4096と偽る。compositorからparamsの`out_of_bounds`（6）を受けた時だけPASS。productionの試験用switchは使わない。

```sh
cc -std=gnu17 -Wall -Wextra -Werror -I. -Iuserland/desktop/include \
  -o build/keiland-linux/stage/opt/keiland/bin/dmabuf-forge \
  plan/tools/keiland-linux/dmabuf-forge.c -Lbuild/keiland-linux/lib \
  -l:libwayland-client.so -l:libvulkan.so.1 \
  -Wl,-rpath-link,build/keiland-linux/lib -Wl,-rpath,/opt/keiland/lib
sh plan/tools/keiland-linux/install-guest.sh
sh plan/tools/keiland-linux/guest.sh ssh 'XDG_RUNTIME_DIR=/run WAYLAND_DISPLAY=keiland-0 KEILAND_DRM_DEVICE=none timeout 30 /opt/keiland/bin/dmabuf-forge'
```

guestにdirect compositorを起動した後に使う。確認はclientの終了0 / PASSとcompositorのIMPORT_ERROR増加・process継続。実機GPUの非同期waitはこのprobeの検証対象ではない。

## WiFi と ALSA

`wifi-setup.sh` は disposable guest 内の root 専用。mac80211_hwsim の2radioにhostapd / wpa_supplicantを起動し、試験専用192.0.2.2/24を付ける（DHCPはKeilandの外）。hostでは実行しない。

`network-probe.c`・`audio-probe.c` は libkeiland-backend の API（`kl_backend_network_*`・`kl_backend_audio_*`）を直接使う（ws131-p011 で libkeiland の旧 API を除いたため）。build の `lib/libkeiland-backend.a` に link し、guestのkeiで実行する。networkはsecured APのscan、saveが自動joinしないこと、PROFILES→JOIN、IPv4/MAC/MTU/counters、saved/DNS、disconnect。audioは40%のreadback、amixer外部70% event/readable、mute、silentfeedback。HDA raw0〜74でlibraryの40%はamixer41%（許容±3）。

```sh
for p in network-probe audio-probe; do
  cc -D_GNU_SOURCE -std=gnu17 -Wall -Wextra -Werror -I. -o build/keiland-linux/stage/opt/keiland/bin/$p plan/tools/keiland-linux/$p.c \
    build/keiland-linux/lib/libkeiland-backend.a -lm
done
timeout 30 sh plan/tools/keiland-linux/guest.sh put plan/tools/keiland-linux/wifi-setup.sh /tmp/wifi-setup.sh
timeout 60 sh plan/tools/keiland-linux/guest.sh ssh 'sh /tmp/wifi-setup.sh'
timeout 140 env SSH_USER=kei sh plan/tools/keiland-linux/guest.sh ssh 'timeout 120 /opt/keiland/bin/network-probe'
timeout 80 env SSH_USER=kei sh plan/tools/keiland-linux/guest.sh ssh 'timeout 60 /opt/keiland/bin/audio-probe'
```

WiFiはnetdevのcontrolsocket権限を使う。radioのup/down ioctlはCAP_NET_ADMINがなければEPERM。資格情報query/saveの応答上限2秒、watchのrequest/updateは応答を待たず後のupdateで完了する。各connectionはprivate `/tmp/keiland-wpa-XXXXXX/socket` を所有しcloseでdirectoryも返す。ALSA mixerのみ、PCMfeedbackは無音。

## logind の fd と D-Bus wire の独立 fixture

`seat-fd.c` は guest の実際の DRM で nonmaster の拒否、caller fd の生存、release 後の master 保持を確認する。host で実行しない。
`dbus-wire.c` / `dbus-wire.py` は host の socketpair で production の D-Bus reader に独立に marshal した frame を送る。fragment / interleave / SCM_RIGHTS、missing fd、oversized body、partial EOF、ancillary overflow の5ケース。普通と ASan/UBSan の両方で確認する。

```sh
clang -D_GNU_SOURCE -std=gnu17 -Wall -Wextra -Werror -I. -Iuserland/desktop/include \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  plan/tools/keiland-linux/dbus-wire.c userland/desktop/libkeiland-backend-linux/dbus-linux.c \
  -o build/keiland-linux/test/dbus-wire
timeout 60 python3 plan/tools/keiland-linux/dbus-wire.py build/keiland-linux/test/dbus-wire
cc -D_GNU_SOURCE -std=gnu17 -Wall -Wextra -Werror plan/tools/keiland-linux/seat-fd.c \
  -Lbuild/keiland-linux/lib -l:libvulkan.so.1 -Wl,-rpath-link,build/keiland-linux/lib \
  -Wl,-rpath,/opt/keiland/lib -o build/keiland-linux/stage/opt/keiland/bin/seat-fd
# install-guest.sh の後、guest の他の compositor / gdm を停止して実行する。
timeout 30 sh plan/tools/keiland-linux/guest.sh ssh '/opt/keiland/bin/seat-fd'
```
