# DisplaySink Protocol Specification

## 1. Overview & Roles

**DisplaySink** allows a PC to extend its desktop over a LAN onto another device's screen using a lightweight UDP discovery + encrypted handoff followed by a standard TLS/RDP (`RDPEGFX` H.264 AVC420) session.

To avoid confusion between the underlying TCP server/client roles and the user-facing display roles, DisplaySink defines two modes:

| Role | Meaning | Underlying Transport Role |
| --- | --- | --- |
| **DisplaySink (Display Mode)** | The **screen/receiver** device that advertises itself on the LAN, displays the remote monitor image, and sends mouse/keyboard events back. | UDP `33890` broadcaster, UDP `OFFER` receiver, **TCP/RDP Client** |
| **DisplaySink (PC mode)** | The **PC/sender** device that creates a virtual monitor (e.g. Windows IddCx) and streams H.264 video frames to a Display Mode peer. | UDP `33890` listener, UDP `OFFER` sender, **TCP/RDP Server** |

```text
DisplaySink (Display Mode)                   DisplaySink (PC mode)
  |                                            |
  |-- UDP broadcast :33890 (ADV) ------------->|  (every ~1s while waiting)
  |   device_id, pk, nonce, reply_port, WxH    |
  |                                            |  User selects peer & clicks Connect
  |<-- UDP unicast :reply_port (OFFER) --------|  Bind ephemeral TCP port + self-signed TLS
  |    ephemeral_pk, iv, ChaCha20(nonce,       |  Generate ephemeral username/password
  |    rdp_port, user, pass) + HMAC-SHA256     |
  |                                            |
  |-- TCP connect :rdp_port ------------------>|
  |<== TLS 1.2/1.3 (self-signed cert) ========>|
  |<== RDP + CredSSP/NTLM + RDPEGFX AVC420 ===>|
  |<-- H.264 Annex-B video frames -------------|
  |-- Fast-Path mouse / keyboard / resize ---->|
```

## 2. Wire Protocol (UDP Discovery & Encrypted Offer)

All multi-byte integers on the wire are **big-endian** (network byte order).
Discovery uses UDP port `33890` (`NETDISP_DISCOVERY_PORT`).

### 2.1 Common Header & Constants
- Magic: `0x5A 0x4B 0x4E 0x44` (`"ZKND"`)
- Version: `1` (`NETDISP_VERSION`)
- Message types:
  - `1`: `ADV` (`NETDISP_MSG_ADV`)
  - `2`: `OFFER` (`NETDISP_MSG_OFFER`)
- Key & ID sizes:
  - `device_id`: 16 bytes
  - `nonce`: 16 bytes
  - `pk` / `sk`: 32 bytes (X25519 raw public/private key)
  - `iv`: 12 bytes (ChaCha20 96-bit nonce)
  - `tag`: 32 bytes (HMAC-SHA256)

### 2.2 `ADV` Packet (`Display Mode -> PC mode`, UDP broadcast to `255.255.255.255:33890`)

Total length: `81 + name_len` bytes (`1 <= name_len <= 63`).

| Offset | Size | Field | Description |
| --- | --- | --- | --- |
| `0` | `4` | `magic` | `"ZKND"` (`5A 4B 4E 44`) |
| `4` | `1` | `version` | `1` |
| `5` | `1` | `msg_type` | `1` (`ADV`) |
| `6` | `2` | `flags` | Reserved (`0`) |
| `8` | `16` | `device_id` | Stable 16-byte peer identifier |
| `24` | `16` | `nonce` | Ephemeral random 16-byte nonce per session |
| `40` | `32` | `pk` | Ephemeral X25519 public key |
| `72` | `2` | `reply_port` | UDP port (`> 0`) where Display Mode listens for `OFFER` |
| `74` | `2` | `pref_width` | Preferred display width in pixels |
| `76` | `2` | `pref_height` | Preferred display height in pixels |
| `78` | `2` | `pref_scale120` | Preferred scale in `1/120` units (`120` = 100%) |
| `80` | `1` | `name_len` | UTF-8 device name length (`1..63`) |
| `81` | `name_len` | `device_name` | Sanitized UTF-8 device name (no control chars) |

#### Visual Verification Code
Both sides compute an 8-hex-digit fingerprint (`"XXXX-XXXX"`) from `SHA-256(pk || nonce)` using the first 4 bytes formatted in uppercase hex.

### 2.3 `OFFER` Packet (`PC mode -> Display Mode`, UDP unicast to `<peer_ip>:<reply_port>`)

Total length: `70 + ct_len + 32` bytes.

#### Outer Header (`70` bytes)

| Offset | Size | Field | Description |
| --- | --- | --- | --- |
| `0` | `4` | `magic` | `"ZKND"` (`5A 4B 4E 44`) |
| `4` | `1` | `version` | `1` |
| `5` | `1` | `msg_type` | `2` (`OFFER`) |
| `6` | `2` | `flags` | `0` |
| `8` | `16` | `device_id` | Target's `device_id` from `ADV` |
| `24` | `32` | `eph_pk` | PC mode's ephemeral X25519 public key |
| `56` | `12` | `packet_iv` | Random 12-byte IV |
| `68` | `2` | `ct_len` | Ciphertext length (`20 + user_len + pass_len`) |
| `70` | `ct_len` | `ciphertext` | ChaCha20-encrypted `offer_plain` |
| `70 + ct_len` | `32` | `tag` | HMAC-SHA256 over bytes `[0 .. 70 + ct_len)` |

#### Plaintext `offer_plain` (`20 + user_len + pass_len` bytes)

| Offset | Size | Field | Description |
| --- | --- | --- | --- |
| `0` | `16` | `nonce` | Must match Display Mode's `ADV.nonce` |
| `16` | `2` | `rdp_port` | Ephemeral TCP port (`> 0`) on PC mode |
| `18` | `1` | `user_len` | `1..64` |
| `19` | `1` | `pass_len` | `1..64` |
| `20` | `user_len` | `username` | Ephemeral RDP username (UTF-8) |
| `20 + user_len` | `pass_len` | `password` | Ephemeral RDP password (UTF-8) |

#### Key Derivation, Encryption, and Authentication
1. **Shared Secret**: `shared = X25519(local_sk, peer_pk)` (32 bytes).
2. **HKDF-SHA256**:
   - `IKM`: `shared` (32 bytes)
   - `salt`: `ADV.nonce` (16 bytes)
   - `info`: `"zedbsd-netdisp-v1" || eph_pk || target_pk` (17 + 32 + 32 = 81 bytes, where `eph_pk` is PC mode's ephemeral public key and `target_pk` is Display Mode's `ADV.pk`)
   - Output (`64` bytes): `enc_key = okm[0..31]`, `mac_key = okm[32..63]`
3. **Stream Cipher**: OpenSSL `EVP_chacha20()` with key `enc_key` and 16-byte IV `0x00000000 || packet_iv` (32-bit counter = `0`, 96-bit nonce = `packet_iv`).
4. **MAC**: `HMAC-SHA256(mac_key, packet[0 .. 70 + ct_len))`. Display Mode verifies `tag` in constant time, decrypts `ciphertext`, and verifies `plain.nonce == expected_nonce` before connecting.

## 3. RDP / Media Session

After receiving a valid `OFFER`, Display Mode connects over TCP to `<sender_ip>:<plain.rdp_port>`:
1. **TLS**: Standard RDP TLS negotiation with an ephemeral self-signed certificate on PC mode.
2. **Authentication**: CredSSP / NTLM or standard RDP credentials using `plain.username` and `plain.password`.
3. **Graphics (`RDPEGFX`)**:
   - PC mode encodes desktop frames as **H.264 Constrained Baseline (Annex-B)** and sends them via `RDPEGFX` (`AVC420`).
   - Display Mode decodes H.264 frames and sends frame acknowledgements (`rdp_client_send_frame_ack`) to pace the stream.
4. **Input & Resize**:
   - Display Mode sends RDP Fast-Path keyboard scancode/Unicode and mouse pointer/wheel events.
   - Window resize requests are sent over `MS-RDPEDISP`.

## 4. Reference Implementation (Source Map)

| Component | Path | Purpose |
| --- | --- | --- |
| Wire protocol (`ADV` / `OFFER`, crypto) | [apps/mikuterm-common/netdisp-proto.h](../../apps/mikuterm-common/netdisp-proto.h)<br>[apps/mikuterm-common/netdisp-proto.c](../../apps/mikuterm-common/netdisp-proto.c) | Packet encoding/decoding, X25519, HKDF-SHA256, ChaCha20, HMAC-SHA256, fingerprint |
| zedBSD **Display Mode** standalone app (`/bin/dispsink`) | [userland/desktop/dispsink/main.c](../../userland/desktop/dispsink/main.c)<br>[userland/desktop/dispsink/Makefile](../../userland/desktop/dispsink/Makefile) | UDP `33890` `ADV` broadcaster, `OFFER` receiver, Keiland SHM window + `rdp-client.c` |
| `mikuterm` **Display Mode** tab | [apps/mikuterm-common/netdisp-tab.c](../../apps/mikuterm-common/netdisp-tab.c)<br>[apps/mikuterm-common/netdisp-tab.h](../../apps/mikuterm-common/netdisp-tab.h) | Cross-platform (Win32/Android/zedBSD) Display Mode launcher & `OFFER` receiver |
| Win32 **PC mode** host & IddCx driver | [apps/terminal-win32/extdisp.c](../../apps/terminal-win32/extdisp.c)<br>[drivers/win32/mikuterm-idd/driver.c](../../drivers/win32/mikuterm-idd/driver.c) | UDP `33890` listener, `OFFER` sender, `rdp_server` + `libmedia` H.264 encoder + `SendInput` |
| `mikuterm` Settings **DisplaySink (PC mode)** UI | [userland/desktop/terminal/settings-page.c](../../userland/desktop/terminal/settings-page.c) | Settings pane listing discovered Display Mode peers and Connect/Disconnect |
| RDP client/server library (`librdp`) | [userland/desktop/librdp/rdp.h](../../userland/desktop/librdp/rdp.h)<br>[userland/desktop/librdp/client.c](../../userland/desktop/librdp/client.c)<br>[userland/desktop/librdp/server.c](../../userland/desktop/librdp/server.c) | TPKT/MCS/TLS/CredSSP/RDPEGFX/RDPEDISP/Fast-Path state machines |
| Shared zedBSD RDP session & H.264 decoder | [userland/desktop/rdp/rdp-client.c](../../userland/desktop/rdp/rdp-client.c)<br>[userland/desktop/rdp/input.c](../../userland/desktop/rdp/input.c) | Background RDP + `libmedia` decode thread used by `/bin/dispsink` |
