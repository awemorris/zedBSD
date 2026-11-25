# Network Display Architecture

Status: design

Network Display allows a `mikuterm` client on the local area network (Win32,
Android, or zedBSD) to act as a **virtual extended display** for a running
graphical session on zedBSD.

Unlike a standalone headless RDP session (`wayland --headless`), Network Display
attaches a virtual output head (`virtual_rdp`) directly to the user's active
physical `wayland` compositor, streams its rendered desktop over H.264/RDPEGFX,
and injects remote pointer and keyboard input into the compositor's seat.

---

## 1. Design Principles

1. **Zero-configuration discovery with explicit user consent**:
   Idle `mikuterm` instances advertise themselves on the LAN via UDP broadcast
   (port `33890`), and the user explicitly approves each connection in
   Settings → Display via a confirmation dialog showing the device name, IP
   address, and visual verification fingerprint.
2. **Single-use, time-limited credentials**:
   `sessiond` issues ephemeral `<username>+extend<N>` credentials with a
   30-second TTL and immediate single-use invalidation upon authentication.
   Credentials are encrypted to the client's ephemeral X25519 public key and
   never stored on disk.
3. **Standard cryptographic primitives**:
   All platforms use `<compat/crypto/...>` (`libcrypto-compat`) with
   X25519 key agreement, HKDF-SHA256 key derivation, ChaCha20 encryption, and
   HMAC-SHA256 authentication (Encrypt-then-MAC).
4. **Non-blocking physical display rendering**:
   The virtual head renders into an offscreen Vulkan `VkImage`, copies to a
   host-visible staging buffer, and hands off BGRA-to-I420 conversion and
   `libmedia` H.264 encoding to a dedicated encoder thread so physical display
   frame pacing is never blocked.

---

## 2. Component Boundaries

| Component | Responsibility | Source |
| --- | --- | --- |
| Wire & crypto protocol | `ADV` / `OFFER` packet encoding, X25519 + HKDF-SHA256 + ChaCha20 + HMAC-SHA256, fingerprint | [`apps/mikuterm-common/netdisp-proto.h`](../../apps/mikuterm-common/netdisp-proto.h) |
| `mikuterm` client | UDP broadcast advertisement while idle, `OFFER` decryption, transition to `rdp_tab` | [`apps/mikuterm-common/netdisp-tab.c`](../../apps/mikuterm-common/netdisp-tab.c) |
| `wayland` compositor | Non-headless UDP discovery thread, `OFFER` transmission, virtual `kwl_head`, encoder thread, direct seat input, window/pointer evacuation on disconnect | [`userland/desktop/wayland/netdisp.c`](../../userland/desktop/wayland/netdisp.c) |
| `sessiond` | One-shot `<username>+extend<N>` issuance (`EXTEND_PREPARE`), NT hash lookup (`RDP_EXTEND_LOOKUP`), `socketpair` + `SCM_RIGHTS` handoff (`RDP_EXTEND`) | [`userland/desktop/sessiond/seat.c`](../../userland/desktop/sessiond/seat.c) |
| `rdpd` | `+extend` NTLM lookup & single-use attach via `/run/sessiond-rdp.sock`, `KWL_RDP_MSG_POINTER` / `KWL_RDP_MSG_KEY` forwarding | [`userland/desktop/rdpd/main.c`](../../userland/desktop/rdpd/main.c) |
| `libkeiland` & Settings | `kl_system_displays_v1` network display events and connect/disconnect requests, Settings UI & confirmation dialog | [`userland/desktop/libkeiland/system/kl-system-protocol.h`](../../userland/desktop/libkeiland/system/kl-system-protocol.h) |

---

## 3. Discovery and Encrypted Pairing Protocol

### 3.1 Advertisement (`ADV`, `mikuterm` → LAN Broadcast)

While waiting in the Network Display tab, `mikuterm` generates an ephemeral
X25519 keypair `(sk, pk)`, a 16-byte random `nonce`, and a 16-byte `device_id`,
and broadcasts an `ADV` packet every 2000 ms to `255.255.255.255:33890`:

- Magic `"ZKND"`, version `1`, message type `1` (`ADV`)
- `device_id` (16 bytes), `nonce` (16 bytes), `pk` (32 bytes)
- `reply_port` (2 bytes), preferred `width` / `height` / `scale120`
- Sanitized UTF-8 `device_name` (1..63 bytes)

The visual verification code displayed on both `mikuterm` and zedBSD Settings
is `SHA-256(pk || nonce)` formatted as `XXXX-XXXX` (first 4 bytes).

### 3.2 Encrypted Offer (`OFFER`, `wayland` → `mikuterm:reply_port`)

After the user confirms the connection in Settings → Display:

1. `wayland` requests one-shot credentials from `sessiond` over
   `SESSIOND_CONTROL_FD` (`EXTEND_PREPARE`).
2. `wayland` generates an ephemeral X25519 keypair `(eph_sk, eph_pk)`, derives
   `shared = X25519(eph_sk, pk)`, and runs HKDF-SHA256 with `salt = nonce` and
   `info = "zedbsd-netdisp-v1" || eph_pk || pk` to produce a 32-byte
   `enc_key` and 32-byte `mac_key`.
3. The plaintext payload (`nonce || rdp_port || username || password`) is
   encrypted with ChaCha20 using a random 12-byte `packet_iv` (counter `0`),
   and authenticated with `HMAC-SHA256(mac_key, header || ciphertext)`.
4. `mikuterm` verifies the HMAC tag in constant time (`CRYPTO_memcmp`),
   decrypts the payload, verifies that the decrypted `nonce` matches its local
   `nonce`, and initiates an RDP/TLS connection to the sender's IP and
   `rdp_port`.

---

## 4. Session Attachment, Rendering, and Input

1. **Authentication & Attachment**:
   `rdpd` authenticates the `+extend` user via `RDP_EXTEND_LOOKUP` over
   `/run/sessiond-rdp.sock` (plaintext authentication is rejected for `+extend`
   accounts). Upon connection, `rdpd` sends `RDP_EXTEND <user> <w> <h>`, which
   atomically consumes the one-shot entry and delivers a `socketpair` `rdp_fd`
   to both `wayland` (`EXTEND_ATTACH`) and `rdpd` via `SCM_RIGHTS`.
2. **Virtual Output & Offscreen Encoding**:
   `wayland` opens a `virtual_rdp` `kwl_head` positioned to the right of the
   physical display. The render thread draws into the head's offscreen
   `VkImage` and copies to a host-visible staging buffer, then wakes the
   dedicated encoder thread to run BGRA-to-I420 conversion, H.264 encoding, and
   `KWL_RDP_MSG_FRAME` transmission.
3. **Direct Seat Input & Safe Disconnect**:
   `rdpd` forwards pointer and keyboard events as `KWL_RDP_MSG_POINTER` and
   `KWL_RDP_MSG_KEY` control messages over `rdp_fd`. `wayland` maps pointer
   coordinates relative to the virtual head's plane origin and injects events
   directly into the seat. When `rdp_fd` closes or the user disconnects from
   Settings, `wayland` parks rendering, evacuates all windows and the pointer
   back to the primary display, and tears down the virtual head.
