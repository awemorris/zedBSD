# Security keys and smart card readers

Status: reference.

zedBSD gives programs two device interfaces for FIDO security keys and smart
cards. Both are reached through `open(2)`, `read(2)`, `write(2)`, `poll(2)`
and `ioctl(2)`; the library that speaks CTAP on top of them (libpasskey) is a
separate component.

| Node | Header | What it carries |
| --- | --- | --- |
| `/dev/input/hidrawN` | [`<uapi/hidraw.h>`](../../include/uapi/hidraw.h) | The raw reports of a HID interface that is not an input device: first the FIDO authenticators (CTAPHID over USB) |
| `/dev/smartcardN` | [`<uapi/ccid.h>`](../../include/uapi/ccid.h) | ISO 7816 APDUs to the card in one slot of a USB CCID reader: a contactless card on an NFC reader (ISO 14443-4), or a security key's own CCID interface |

A number is an allocation result, not an identity. Programs enumerate the
nodes, ask what each one is, and choose by that.

## Permissions

Both kinds of node are owned by root with mode 0600. While a graphical session
runs, sessiond gives every `hidraw` and `smartcard` node to the session's user
with mode 0600, and gives them back to root when it stops. The login screen is
never given them: a security key login is done by `/sbin/passkey`, which opens
the nodes as root.
Whoever can open a security key's node can ask the key to sign; the key's own
user presence (a touch) and user verification (its PIN) are what protect the
user.

## Raw HID: `/dev/input/hidrawN`

A HID interface whose report descriptor's first top-level collection is the
FIDO Alliance's (usage page `0xF1D0`, usage `0x01`, CTAPHID) is published as a
raw node instead of an event device. Keyboards, pointers, touch screens and
pens stay event devices (`/dev/input/eventN`) and get no raw node.

`read()` and `write()` have the meaning they have on Linux's hidraw, so the
same bytes go through on both systems:

- `read()` returns one input report. A device that numbers its reports puts
  the report ID in the first byte. A buffer shorter than the report receives
  its first bytes. An empty queue waits, or fails with `EAGAIN` when the file
  is `O_NONBLOCK`.
- `write()` sends one output report. The first byte is the report ID, `0` for
  a device that does not number its reports (that byte is then not sent). It
  returns the whole length once the device has taken the report. A length
  below 2 or above the output report's size plus one fails with `EINVAL`. A
  device that does not take the report within 5 seconds fails it with the
  transfer's error.
- `poll()` reports `POLLIN` while a report waits, `POLLOUT` while the device is
  there, and `POLLHUP` once it is gone. Then `read()` and `write()` fail with
  `ENODEV`.

Each open has its own queue of `HIDRAW_QUEUE` (64) input reports, so two
programs each see every report; when a queue is full its oldest report is
dropped. A program that must be alone with the key, such as the login's
security key check, grabs it with `HIDRAW_GRAB`: no other program can then
send the key a request that the user's touch would answer, nor read the
answers. Output reports from several opens go out one at a time, in the order
they were written. A CTAPHID program tells its own traffic apart by its
channel ID.

| Request | Argument | Answer |
| --- | --- | --- |
| `HIDRAW_GET_INFO` | `struct hidraw_info` | The bus (`HIDRAW_BUS_USB`, or `HIDRAW_BUS_VIRTUAL` for the test kernel's loopback key), vendor, product and release, the USB interface number, the top collection's usage page and usage, the largest input and output report without the ID's byte, and `HIDRAW_INFO_NUMBERED` when the reports are numbered |
| `HIDRAW_GET_DESCRIPTOR` | `struct hidraw_descriptor` | The report descriptor as the device gave it (at most 4096 bytes) |
| `HIDRAW_GET_NAME` | `struct hidraw_text` | The product's name, ended by a NUL (at most 63 bytes) |
| `HIDRAW_GET_PHYS` | `struct hidraw_text` | The device's place, `usbB/portP/deviceD/interfaceI` |
| `HIDRAW_GRAB` | `int` | Nonzero takes the device for this open alone; 0 gives it back. While an open holds it, the other opens receive no input report (the reports they had not read when it was taken are dropped) and their writes fail with `EBUSY`; a second grab fails with `EBUSY`. The last close of the holding file gives it back |

Any other request fails with `ENOTTY`. The node's arrival and removal are
posted to `/dev/system` as an input event (`KERN_SYSTEM_EVENT_INPUT`) whose
subject is `hidrawN`.

A security key's other interfaces are not changed: its OTP keyboard interface
stays a keyboard (zedBSD does not use the OTP function), and its CCID
interface is a smart card reader.

## Smart card slots: `/dev/smartcardN`

Every slot of a USB CCID reader (interface class `0x0B`) that exchanges whole
APDUs is a node of its own: an NFC reader's contactless slot, its SAM slot, a
security key's CCID interface. A reader that only exchanges TPDUs or
characters is not published. The reader frames the APDUs for the card (for a
contactless card, ISO 14443-4); the kernel carries whole APDUs and answers.

A slot node may be opened by any number of programs to read its status and to
watch for cards. Using the card is exclusive: `CCID_POWER_ON` claims the slot
for that open file, and from then until `CCID_POWER_OFF` or the file's last
close no other open may power the card or send it APDUs (`EBUSY`). The last
close of the claiming file powers the card off, so no application's security
state (a verified PIN, a selected applet) passes to the next program.

| Request | Argument | Answer |
| --- | --- | --- |
| `CCID_GET_INFO` | `struct ccid_info` | The reader's vendor, product and release, the interface number, this slot's number and the reader's slot count, the class descriptor's `dwFeatures`, `dwProtocols` and `dwMaxCCIDMessageLength`, the longest command and answer this slot takes, the product's name |
| `CCID_GET_STATUS` | `struct ccid_status` | Whether a card is there (`CCID_CARD_ABSENT`, `CCID_CARD_PRESENT`, `CCID_CARD_POWERED`), its ATR while powered, and the slot's change count (it grows at every insertion and removal) |
| `CCID_POWER_ON` | `struct ccid_status` | Claims the slot for this open, powers the card and answers its ATR. `ENXIO` when no card is there, `EBUSY` when another open holds the slot |
| `CCID_POWER_OFF` | none | Powers the card off and gives up the claim |
| `CCID_TRANSMIT` | `struct ccid_transmit` | Sends one command APDU and answers the response APDU with its status word (SW1 SW2). The call must hold the claim (`EPERM` otherwise) |

`struct ccid_transmit` carries 64-bit pointers and sizes, so 32-bit and 64-bit
programs pass the same structure: `command` and `command_size` (at most the
slot's longest command), `response` and `response_capacity`, `timeout_ms`
(0 for the default of 30 seconds; at most 120 seconds), and the answer's
`response_size`. The deadline holds even while the card asks for more time.
On a deadline or a signal the kernel aborts the command at the reader
(`EINTR` or `ETIMEDOUT`); the card stays powered but its state is unknown, and
a careful program powers it off and on again. An answer longer than
`response_capacity` fails with `EMSGSIZE` and is dropped. A removed card fails
the call with `ENXIO`; the program powers the next card on.

Whether a card accepts extended-length APDUs is the card's matter, not the
reader's. Programs that need long messages send short APDUs with ISO 7816-4
command chaining and collect long answers with `GET RESPONSE` (`61xx`).

`read()` returns `struct ccid_event` records, one for each insertion
(`CCID_EVENT_INSERTED`) and removal (`CCID_EVENT_REMOVED`) after the open,
with the slot's change count. A record is never split; a buffer smaller than
one fails with `EINVAL`, an empty queue waits or fails with `EAGAIN` when the
file is `O_NONBLOCK`. Each open keeps up to `CCID_EVENT_QUEUE` (16) records,
dropping the oldest. `poll()` reports `POLLIN` while a record waits and
`POLLHUP` once the reader is gone; then every call fails with `ENODEV`.

The node's arrival and removal are posted to `/dev/system` as a USB event
(`KERN_SYSTEM_EVENT_USB`) whose subject is `smartcardN`.
