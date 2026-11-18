# Networking Architecture

Status: current

This document describes the networking architecture of zedBSD: the kernel network
stack, the `networkd` management daemon, and the relationships between the
administrative tools `/sbin/net`, `/sbin/ifconfig`, and `/sbin/wifi`.

---

## 1. Architectural Layers

```
+--------------------------------------------------------------------------+
| Userland CLI Tools                                                       |
|                                                                          |
|   /sbin/net                      /sbin/ifconfig          /sbin/wifi      |
|   (High-level declarative/       (Low-level socket       (Low-level      |
|    transactional CLI)             ioctls)                 802.11 ioctls) |
+--------------------------------------------------------------------------+
         |                                |                       |
         | UNIX Domain Socket             |                       |
         | /run/networkd.sock             |                       |
         v                                |                       |
+------------------------------------+    |                       |
| networkd (System Network Daemon)   |    |                       |
|   - Link carrier monitoring        |    |                       |
|   - Managed LAN & WLAN engine      |    |                       |
|   - DHCPv4, DHCPv6 & SLAAC         |    |                       |
|   - /etc/resolv.conf publication   |    |                       |
+------------------------------------+    |                       |
         |                                |                       |
         +--------------------------------+-----------------------+
                                          |
                                          | Socket ioctl / Netlink / devfs
                                          v
+--------------------------------------------------------------------------+
| Kernel Network Stack (src/kern/net/)                                     |
|   - IPv4 / IPv6 protocol engines, ARP, ICMP, TCP, UDP                    |
|   - Routing tables, interface queues, loopback                           |
|   - Network Device Drivers: Ethernet, USB CDC, Wi-Fi                     |
+--------------------------------------------------------------------------+
```

The networking system cleanly separates low-level interface operations from
system-wide automation and policy:

1. **Kernel Subsystem (`src/kern/net/`)**:
   Implements network sockets, packet buffers, protocol handling (IPv4, IPv6,
   TCP, UDP), routing tables, and device drivers for Ethernet and Wi-Fi.
2. **System Network Daemon (`networkd`)**:
   Runs as a background daemon (`/sbin/networkd`). It monitors physical link
   states, runs DHCP/SLAAC lease engines, automatically associates with known
   Wi-Fi networks, and updates system DNS resolvers.
3. **Control Interfaces (`/sbin/net`, `/sbin/ifconfig`, `/sbin/wifi`)**:
   Offer distinct levels of abstraction for automated scripts, administrative
   policy, and low-level debugging.

---

## 2. The `networkd` Daemon

[`networkd`](../../userland/base/networkd/main.c) is responsible for automatic
network configuration without manual user intervention.

### 2.1 Core Responsibilities

- **Link State Detection**:
  Listens for kernel carrier status events. When an Ethernet cable is plugged in
  or a Wi-Fi link connects, `networkd` initiates address acquisition.
- **Dynamic Configuration (DHCP & SLAAC)**:
  Contains integrated lightweight clients for:
  - DHCPv4 ([`dhcp.c`](../../userland/base/net/dhcp.c))
  - DHCPv6 ([`dhcp6.c`](../../userland/base/net/dhcp6.c))
  - IPv6 Stateless Address Autoconfiguration ([`slaac.c`](../../userland/base/networkd/slaac.c))
- **Managed LAN**:
  Automatically brings up wired interfaces when a link carrier is detected,
  requests a DHCP lease, and configures the default gateway.
- **Managed WLAN**:
  Coordinates background scanning and automatic association with saved SSIDs
  stored in the encrypted/protected Wi-Fi profile store.
- **Dynamic DNS Resolver**:
  Consolidates nameservers received via DHCPv4 option 6 and IPv6 RDNSS options,
  atomically publishing them to `/etc/resolv.conf`.

### 2.2 Daemon IPC (`/run/networkd.sock`)

`networkd` listens on a UNIX domain socket at `/run/networkd.sock`. Clients such
as `/sbin/net` and desktop status panels connect to:
- Query real-time interface states and active IP allocations.
- Subscribe to link change and scan completion events (`net watch`).
- Submit explicit connect, disconnect, or interface configuration requests.

---

## 3. Administrative Tooling: `/sbin/net` vs `/sbin/ifconfig` vs `/sbin/wifi`

| Feature / Aspect | `/sbin/net` | `/sbin/ifconfig` | `/sbin/wifi` |
| --- | :--- | :--- | :--- |
| **Level** | High-level / System Policy | Low-level / Imperative | Low-level / 802.11 Specific |
| **Communication** | `networkd` IPC socket | Kernel socket ioctls | Kernel WLAN ioctls |
| **Scope** | Persistent profiles, routing, Wi-Fi keys, DNS | Interface flags, IP/netmask, MTU | Raw scan, link status, raw passphrase connect |
| **Persistence** | Yes (`/etc/rc.conf`, `/etc/wifi.conf`) | No (in-memory kernel state only) | No (immediate link state only) |
| **Target User** | System administrators, desktop scripts | Network developers, debuggers | Firmware debuggers, low-level testing |

---

### 3.1 The `/sbin/net` Command

[`/sbin/net`](../../userland/base/net/main.c) is the primary user and administrative
command. It supports both one-shot commands and an interactive configuration console.

#### Operational Commands

```bash
# Display all interfaces, carrier states, IP addresses, and routes
net show

# Display details for a specific interface
net show eth0

# Stream live network state change events
net watch

# Bring interfaces up or down through networkd
net up eth0
net down eth0

# Request a DHCP lease on an interface
net dhcp eth0 --timeout=10

# Set a static IPv4 address and netmask
net static eth0 ipv4 192.168.1.50 netmask 255.255.255.0

# Configure default route gateway and DNS nameservers
net defaultroute 192.168.1.1
net dns 1.1.1.1 8.8.8.8

# Enable or disable managed LAN automation
net lan enable
net lan disable
```

#### Wi-Fi Management via `net`

`net` provides secure profile storage for Wi-Fi credentials (`wifi-store.c`):

```bash
# Add a new Wi-Fi network profile (password stored in protected store)
net wifi add MyHomeNetwork --password SecretPassword --auto yes

# Connect to a known Wi-Fi profile
net wifi connect MyHomeNetwork

# List visible and configured Wi-Fi networks
net wifi list

# Disconnect from current Wi-Fi network
net wifi disconnect

# Modify or delete a profile
net wifi modify MyHomeNetwork --auto no
net wifi delete MyHomeNetwork

# Enable or disable Wi-Fi radio subsystem
net wifi enable
net wifi disable
```

#### Transactional Configuration Console

Running `net` with no arguments (or `net startup`) enters an interactive
configuration mode supporting rollback:
- **`show`**: Inspect current configuration candidates.
- **`commit`**: Atomically validate and apply changes to `/etc/rc.conf` and `networkd`.
- **`rollback`**: Revert candidate changes to the active configuration.

---

### 3.2 The `/sbin/ifconfig` Command

[`/sbin/ifconfig`](../../userland/base/ifconfig/main.c) is the low-level BSD
diagnostic tool. It issues direct ioctl system calls (`SIOCGIFFLAGS`, `SIOCSIFADDR`,
`SIOCSIFNETMASK`, `SIOCSIFMTU`) against the kernel network stack without informing
`networkd` or updating configuration files.

```bash
# List all active interfaces
ifconfig -a

# Manually configure IPv4 address and prefix
ifconfig eth0 inet 192.168.1.100/24

# Set interface link state
ifconfig eth0 up
ifconfig eth0 down

# Configure MTU
ifconfig eth0 mtu 1500

# Add/remove IPv6 addresses
ifconfig eth0 inet6 2001:db8::1/64
ifconfig eth0 -inet6 2001:db8::1/64
```

> [!NOTE]
> Changes made via `ifconfig` modify the in-memory kernel interface directly.
> Because `networkd` may reassert managed state on carrier transitions, use
> `/sbin/net` for permanent system configuration.

---

### 3.3 The `/sbin/wifi` Command

[`/sbin/wifi`](../../userland/base/wifi/main.c) communicates directly with kernel
WLAN drivers via 802.11-specific ioctl requests. It is primarily used for hardware
bring-up, testing radio drivers, and troubleshooting signal reception.

```bash
# Bring the wireless radio up or down
wifi wlan0 up
wifi wlan0 down

# Trigger an active background scan
wifi wlan0 search start

# View raw scan results with signal strength and encryption flags
wifi wlan0 list

# View current link status (BSSID, channel, RSSI)
wifi wlan0 status

# Manually connect with raw credentials (bypassing profile storage)
wifi wlan0 connect MySSID MyPassphrase

# Disconnect from current access point
wifi wlan0 disconnect
```

---

## 4. Best Practices for Developers and Users

1. **For General Configuration & Desktop Use**:
   Always use **`/sbin/net`**. It maintains state synchronization with `networkd`,
   preserves credentials securely across reboots, and avoids conflicts with
   automatic DHCP leases.
2. **For Driver Bring-up and Testing**:
   Use **`/sbin/wifi`** to test driver scanning and association independently
   of `networkd`'s automatic state machine.
3. **For Container / Network Namespace Setup**:
   Use **`/sbin/ifconfig`** inside isolated environments where `networkd` is not
   running.
