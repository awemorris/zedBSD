# Keiland on Linux

Debian 13 / amd64 / glibc で検証した Linux 版。compositor、独自 Wayland client / Vulkan frontend、Terminal・Files・Settings・Notes・Text Editor・Image Viewer・PDF Viewer・IME・kuidemo・mview を `/opt/keiland` に置く。

## Build と install

`make keiland-linux` は、まず build に要る package が入っているかを package manager（apt・dnf/yum・pacman）で確かめる。足りない物があれば一覧を出し、端末なら導入してよいかを聞いて（y/N）、y なら sudo で導入する。build が成功したら、install してよいかを聞いて（y/N）、y なら `sudo make keiland-linux-install` と同じ install を行う。端末でない時と `KEILAND_ASK=n` の時は何も聞かず、足りない package と install の命令を出すだけにする（足りない時は build を始めずに止まる）。

```sh
make -j$(nproc) keiland-linux
```

Debian 13 で要る package（apt の名前。dnf/yum・pacman の名前は `tools/build/keiland-prerequisites.sh` の表）:

```sh
sudo apt install build-essential libvulkan-dev linux-libc-dev python3 curl
sudo make keiland-linux-install
```

clang は任意。別 build directory で実行できる。

```sh
make -j$(nproc) keiland-linux CC=clang KEILAND_LINUX_BUILD=build/keiland-linux-clang
```

image / font / compression libraries は、この repository の実装を build する。system の Wayland、libdrm、alsa-lib、libdbus、libsystemd の development package は production build に要らない。
IME の辞書は初回 build で取得し、archive と辞書の SHA256 を検証する。

`KEILAND_PREFIX` は build と install の両方で同じ値を指定する。既定は `/opt/keiland`。ELF の RUNPATH もその prefix の `lib` に固定される。
`DESTDIR` は install の配置先を変えるだけで、埋め込んだ prefix は変えない。

```sh
make keiland-linux-install DESTDIR="$PWD/build/keiland-linux/stage"
make keiland-linux-clean
```

`bin/` は app と compositor、`lib/` は共有 library、`libexec/` は IME、`share/` は font・辞書・wallpaper、`etc/keiland/apps.conf` は App Home の一覧。
wallpaper は `KEILAND_LINUX_WALLPAPER=/absolute/path/picture.png`（PNG か JPEG。`share/keiland/wallpaper.png` に入る）で build 時に指定できる。既定は tree の `userland/desktop/wallpapers/Birch-Lake.png`。ユーザー画像は git に入れない。

## Text console からの起動

system の Vulkan loader と対応する ICD が必要。検証環境は Mesa lavapipe で、Debian の `mesa-vulkan-drivers` を使用した。
他の compositor / display manager が DRM master を持っていない text console で、root として起動する。

```sh
sudo env KEILAND_SEAT=direct /opt/keiland/bin/wayland \
  --session --glass --wallpaper=/opt/keiland/share/keiland/wallpaper.png
```

`direct` は DRM / evdev device を直接開く。`--socket=/absolute/path` で socket を指定できる。
通常は `$XDG_RUNTIME_DIR/wayland-keiland`、runtime directory が無ければ `/tmp/wayland-keiland`。client には同じ runtime directory と `WAYLAND_DISPLAY=wayland-keiland` を渡す。App Home の child は compositor が指定した socket を引き継ぐ。
App Home の Log Out は compositor を終了する。SIGTERM でも正常終了し、取得した scanout と socket を返す。

## gdm session

gdm と systemd-logind が動いている環境で、session の登録を行う。

```sh
sudo make keiland-linux-install-session
```

この command は唯一 prefix 外の file、`/usr/share/wayland-sessions/keiland.desktop` を置く。gdm の session chooser で **Keiland** を選んで通常の利用者でログインする。
compositor は `XDG_SESSION_ID` から logind session を取得し、DRM / input fd を `TakeDevice` で借りる。Log Out は gdm の greeter に戻る。
VT を切り替える場合は別 console の `chvt` または logind の `Seat.SwitchTo` を使用する。Keiland 内の Ctrl+Alt+Fn は未実装。

| Device / service | 必要なアクセス |
| --- | --- |
| DRM primary / evdev | root の direct mode、または gdm/logind の session fd。logind mode は `video` / `input` group による直接 open に依存しない |
| GPU render node | system Vulkan ICD が要求する render node の権限。通常は `render` group または session ACL |
| lavapipe dma-buf | `/dev/udmabuf` の権限。検証した Debian では `root:kvm 0660` のため `kvm` group が必要 |
| WiFi | wpa_supplicant control socket の group。検証では `ctrl_interface=DIR=/run/wpa_supplicant GROUP=netdev` と `netdev` group |
| ALSA mixer | `/dev/snd/controlC*` の read/write 権限。検証では `audio` group |

group を追加したらログインし直す。device の権限は distribution の設定も確認する。

## Network と音

wpa_supplicant は system 側で起動し、control socket を `/run/wpa_supplicant/` に公開する。Keiland は最初の non-P2P interface を選び、scan・保存済み profile・資格情報保存・接続 / 切断を扱う。
資格情報の保存だけでは接続しない。IP address / DHCP / routing は system の責務。WiFi radio の up/down は `CAP_NET_ADMIN` が無ければ EPERM となり、`netdev` group だけでは許可されない。
正常終了時は private `/tmp/keiland-wpa-XXXXXX/socket` を削除する。SIGKILL や crash では一時 directory が残る場合がある。

Settings と system bar は ALSA kernel control interface を直接使う。Master / PCM / Speaker の volume と対応する mute switch を扱い、他の process の変更も event で読み直す。
検証では HDA の raw 0〜74 を使用したため、表示値に整数丸めの差がある。最初にアクセスできる card を選択する。PCM playback / 音量変更時の確認音は実装していない。

## 環境変数

| 変数 | 用途 |
| --- | --- |
| `KEILAND_VULKAN_BACKEND` | system の後段 libvulkan の absolute path。未指定は prefix の `etc/vulkan-backend`、次に build 時の system path 一覧 |
| `KEILAND_VULKAN_NO_DEEPBIND=1` | glibc の RTLD_DEEPBIND を外す互換設定。既定は後段への symbol interposition を防ぐため有効 |
| `KEILAND_DRM_DEVICE` | DRM card の path。`none` は画面 inquiry を無効にする headless 用設定で、compositor の画面出力には使わない |
| `KEILAND_SEAT` | `direct` / `logind`。未指定は gdm session の有無から選択 |

## 検証と制限

再利用できる試験手順は [Linux tools](../../plan/tools/keiland-linux/README.md)、仕組みと方針は [WS105 design](../../plan/ws105/design.md)。
試験用 host dependencies は `clang`、`pkg-config`、`libwayland-dev`、`libwayland-bin`、`wayland-protocols`、`mesa-vulkan-drivers`、`vulkan-tools`、`qemu-system-x86`、`qemu-utils`、`mmdebstrap`、`e2fsprogs`。
実際の network / audio / gdm 試験は disposable Debian guest で行い、host の画面・入力を使用しない。

証拠は Debian 13 / amd64 / glibc の host と QEMU guest。実機 GPU、musl、ARM、FreeBSD は未検証。
KMS は Vulkan image → CPU readback → dumb buffer の複写経路。Wayland dma-buf は version 3、implicit sync と CPU completion の fallback を使う。
cooperative `PauseDevice` の ACK は source を確認したが、検証した systemd の VT switch は `force` notification のため実際の ACK は未観測。

FreeBSD、browser、X server、EGL / GLES、外部 Wayland app、Qt / GTK、explicit sync、zero-copy KMS、lock screen、session 内 Shut Down は [design §8](../../plan/ws105/design.md#8-範囲の外ws105-では作らないfuture-work-か後の-ws) の範囲外。
move / resize の既存問題は BUG-125・BUG-127 として追跡し、今回の Linux 移植では修復済みと扱わない。
