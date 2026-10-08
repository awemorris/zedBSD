# Public browser component checks

Run from the repository root:

```sh
sh plan/tools/browser-component/run.sh plain
sh plan/tools/browser-component/run.sh asan
```

The existing WS074 host builder produces a real `libbrowser.so` with production
exports. This client includes only public `<browser.h>` and links that library,
standard Vulkan and the C runtime. It links no Wayland library. Set
`BROWSER_HOST_BUILD` to select another isolated output root (default
`build/ws107-host`); `CC` selects the host compiler. Production Linux browser
packaging is outside this fixture.

The client checks two independent views and their input/callback state, borrowed
query strings, deferred close requests, rejected dimensions/stride/targets,
navigation allocation rollback preserving a forward history entry, failed and
canceled asynchronous history navigation, and real lavapipe rendering compared
with the CPU reference. It submits recorded commands with a caller-owned fence,
releases targets, recreates a resized offscreen and continues using another view
once the first is destroyed. Framebuffer and strdup failures use executable
symbol interposition; production code has no test switches.

`run.py` binds a temporary port on 127.0.0.1, serves two local pages, and closes a
subsequent history response to cause a transport failure. The client has a
60-second timeout. No external network, GPU hardware or Wayland server is needed.
The fonts are the tree's own (`userland/desktop/fonts`; before 2026-10-08 the fixture read `build/ws035-fonts`, an old
build output whose `Mahora-Regular.ttf` had gone, and every drawing failed with ENOENT).

ASan requires `detect_stack_use_after_return=0`: the engine's conservative
collector scans the real stack. The sanitizer run checks the same production
paths including Vulkan; CPU success is not presented as GPU evidence. Same-view
mutation/destruction inside a callback is outside the agreed public contract;
the fixture records the request and acts after the outer engine call returns.
