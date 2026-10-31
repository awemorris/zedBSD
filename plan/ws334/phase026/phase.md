<!-- awesome-plan project=zedbsd record=ws334-p026 -->

# ws334-p026 統合: libmedia の Windows（Vulkan の loader・mediafile・GPU の絵の fd・Raspberry Pi 4 の回避）

- 親: [WS334](../ws.md)、設計 [design.md](../design.md) の §2.8・§5（M2〜M9）。
- 状態: planned（2026-11-01 p002 で作成）。
- 依存: p013（media-os）・p022（起こしの callback）。
- 担当の想定: Opus（phase-runner）を 1 回。使う約束は `libmedia/media-os.h`。zedBSD・Linux の挙動を変えない（M7 は arm64 の zedBSD の Raspberry Pi 4 で同じ結果になる形）。

## 変える file と中身

1. **M2 `vkvideo-runtime.c`**: `vkvideo_loader` の `dlopen` 2 回を `media_os_vulkan_open(&vkvideo_library)`（`static struct media_os_library *vkvideo_library`）、`dlsym(vkvideo_library, "vkGetInstanceProcAddr")` を `media_os_library_symbol`（`media_os_function` から `PFN_vkGetInstanceProcAddr` へは今の `memcpy` の形で）。`<dlfcn.h>` を外す。
2. **M4 `mediafile/mediafile.c`**: `opened->fd`・`open(O_CLOEXEC)`・`fstat`（:52・:60）を `media_os_file_open(path, &opened->file, &opened->size)`、`pread`（:288 の loop）を `media_os_file_read_at`、`close` を `media_os_file_close`。12 byte 未満の EINVAL は今のまま。`<fcntl.h>`・`<sys/stat.h>`・`<unistd.h>` を外す。`mediafile-private.h` の `int fd` を `struct media_os_file *file` に。
3. **M9 `picture.c:442`**: `close(slots->fd[index])` を `media_os_close_descriptor`。
4. **M7 `vkvideo-runtime.c:36-46`・`vkvideo.c:18-28`**: `#if defined(__aarch64__)` の `VKVIDEO_FLIGHTS`・`VKVIDEO_SKIP_FAILED` を実行時の値にする: device を選んだ時に `VkPhysicalDeviceProperties` の `vendorID` が 0x14e4（zedBSD の Raspberry Pi 4 の driver の値、`src/drivers/gpu/bcm2711/vk/vulkan-query.c:428`）の時だけ flights 4・失敗を飛ばす、他は 1・飛ばさない。値は runtime の構造体（`struct vkvideo_runtime` に `unsigned flights`・`int skip_failed`）に持ち、`VKVIDEO_FLIGHTS` を使う配列の大きさは最大の 4 で取る。最初に bcm2711 の Vulkan Video の queue がこの vendor の device にあることを source で確かめる（違えば止めて Q1 に）。comment の「only arm64 Vulkan Video device」を「the Raspberry Pi 4's firmware decoder, known by its vendor」に。
5. **起こし**: p022 で済み。
6. **sources.mk**: `LIBMEDIA_SOURCES` に `media-os-posix.c`。Windows の Makefile は `media-os-win32.c`。
7. **Windows の build**: `apps/browser-win32/Makefile` に `libmedia.dll`（`libmedia/exports.map` の glob の .def、`-lucrt`・winpthreads、Vulkan は media-os の実行時の読み込みなので import しない）。`cross-build.sh` に libmedia の `nm -u` と `objdump -p` の依存の DLL（kernel32・UCRT の api-ms-win-crt-*・libwinpthread-1.dll か静的）を足す。

## 試験（受け入れ）

- libmedia の host の試験が前と同じ件数（`plan/tools/h264sw/run-unit.sh all`、libmedia の他の host の試験を `plan/tools.md` で探す）。
- Wine: `plan/ws334/tests/media-main.c`（`media_engine_open` で同梱の小さな H.264 の mp4（試験の data は `plan/tools/h264sw/` の conformance の cache か tree の中の物を Q1 に確かめる）を開き、software の H.264 で数枚を取り出して大きさと最初の絵の hash を出す）が host の同じ program と同じ hash。Wine の Vulkan（lavapipe）は Vulkan Video を持たない見込みなので software の道を通る（Vulkan Video の道は実機の Windows で、p032 以後）。
- `run-unit.sh all`・`cross-build.sh` PASS。zedBSD amd64 の `libmedia.so`・arm64 の `libmedia.so` の build が warning 0（arm64 の command は p020 の amd64 の物の platform を config-arm64 に）。
- `git diff --check`。

## 変えてはいけない物と終わり

- module の header と .c、試験の道具。libbrowser（p022 で済み）。HAL・toolchain・共有の build/。rm をしない。push しない。
- file の群ごとの commit、SHA と結果を Q1 に、結果の節と ws.md の行。
