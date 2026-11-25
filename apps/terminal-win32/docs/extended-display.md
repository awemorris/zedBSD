# Extended Display Host for Windows

Status: design

`mikuterm` on Windows can act as an Extended Display host, adding a virtual monitor to the Windows desktop and streaming it over the LAN to any peer running `mikuterm` in Network Display mode.

## Architecture

```
+-------------------------------------------------------------------+
| Windows Host                                                      |
|                                                                   |
|  [IddCx 1.4 UMDF Driver]                                          |
|     drivers/win32/mikuterm-idd/                                   |
|     - Virtual monitor via SwDeviceCreate (Root\MikutermIdd)       |
|     - IDDCX_SWAPCHAIN -> D3D11 staging copy -> Shared Memory Ring |
|                                                                   |
|           | (Shared Memory BGRA + Frame Event + IOCTL)            |
|           v                                                       |
|  [mikuterm.exe - extdisp.c]                                       |
|     - UDP 33890 discovery listener (netdisp_adv_decode)           |
|     - BGRA -> I420 -> libmedia H.264 low-latency Annex-B encoder  |
|     - Ephemeral TLS + rdp_server (librdp RDPEGFX AVC420)          |
|     - SendInput mouse/keyboard injection mapped to monitor rect   |
|     - Settings -> Extended Display pane (settings-page.c)         |
+-------------------------------------------------------------------+
            |                                           ^
            | UDP 33890 ADV (peer broadcast)            |
            |<------------------------------------------+
            | Encrypted UDP OFFER (X25519 + HKDF-SHA256 + ChaCha20 + HMAC)
            +------------------------------------------>|
            | TLS / RDP (ephemeral port + one-shot credentials)
            +<=========================================>|
```

## Protocol Reuse

The Windows host reuses the shared Network Display wire protocol defined in [netdisp-proto.h](../mikuterm-common/netdisp-proto.h):

1. **Discovery (`ADV`)**: `mikuterm.exe` listens on UDP port `33890` (`NETDISP_DISCOVERY_PORT`) and decodes `ZKND` v1 `ADV` packets with `netdisp_adv_decode()`. Peers not seen for 7 seconds are expired.
2. **Visual Verification**: Each discovered peer's `pk` and `nonce` produce an 8-hex-digit fingerprint (`XXXX-XXXX`) via `netdisp_fingerprint_hex()`, shown in the **Settings > Extended Display** pane.
3. **Encrypted `OFFER`**: When the user clicks **Connect**, `mikuterm.exe` generates ephemeral credentials, binds an ephemeral TCP port, encrypts `(nonce, rdp_port, username, password)` with `netdisp_offer_encrypt()` (X25519 + HKDF-SHA256 + ChaCha20 + HMAC-SHA256), and unicasts the `OFFER` to the peer's `reply_port`.

## Driver / Application IPC

- **Driver location**: `drivers/win32/mikuterm-idd/` (`driver.c`, `mikuterm_idd_ipc.h`, `mikuterm_idd.inf`, `mikuterm_idd.vcxproj`).
- **Software device instantiation**: `mikuterm.exe` loads `cfgmgr32.dll` dynamically (`LoadLibraryExW` with `LOAD_LIBRARY_SEARCH_SYSTEM32`) and invokes `SwDeviceCreate` / `SwDeviceClose` for `Root\MikutermIdd`, so the application binary does not require WDK headers at compile time.
- **Shared memory & event**: The UMDF 2 driver copies swapchain frames via a D3D11 staging texture into a named shared-memory BGRA ring (`Global\MikutermIddFrameRing`) and signals a named event (`Global\MikutermIddFrameEvent`). Cross-session DACLs grant access to the desktop user session.
- **IOCTL control**: `IOCTL_MIKUTERM_IDD_SET_MODE` configures the virtual monitor resolution/refresh rate, and `IOCTL_MIKUTERM_IDD_GET_STATUS` queries driver state.
- **Graceful fallback**: If the driver is not installed on the system, the host still completes discovery, TLS/RDP negotiation, and input injection, sending standby frames until the driver is installed.

## Security Model

- **Ephemeral TLS**: Each session generates a fresh self-signed certificate in memory via `SSL_CTX_generate_self_signed()`.
- **One-shot credentials**: Random per-session username (`mikuterm+extend<N>`) and password are generated via `RAND_bytes()`, encrypted under the peer's advertised X25519 public key, and accepted only once for that session.
- **Input scoping**: Pointer coordinates received over RDP are clamped to `[0, width) x [0, height)` and mapped to the virtual monitor's `EnumDisplayMonitors` rectangle before calling `SendInput()`.

## Driver Installation & Signing

- **Test-signed builds**: The `.github/workflows/win32-idd.yml` workflow builds `mikuterm_idd.vcxproj` on `windows-latest` with the WDK and produces a test-signed driver package (`.dll`, `.inf`, `.cat`, `.cer`).
- **Installing a test-signed driver**:
  1. Enable test signing on the target Windows machine (Administrator Command Prompt): `bcdedit /set testsigning on` and reboot.
  2. Install the test certificate (`mikuterm_idd_test.cer`) into `LocalMachine\Root` and `LocalMachine\TrustedPublisher`.
  3. Install the driver package via `pnputil /add-driver mikuterm_idd.inf /install`.
