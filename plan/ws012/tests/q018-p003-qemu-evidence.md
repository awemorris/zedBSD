# q018 ws012-p003 production QEMU evidence

Date: 2026-08-28

The production amd64 image was regenerated with `make -j16`, copied to a
disposable image, and booted once with:

```sh
qemu-system-x86_64 -machine pc -m 512 -smp 4 \
  -drive file=/tmp/ws012-p003-qemu.lZziUa/run.img,format=raw,if=ide \
  -display none -serial none \
  -debugcon file:/tmp/ws012-p003-qemu.lZziUa/guest.log \
  -monitor stdio -no-reboot
```

Input was delivered through the QEMU monitor's `sendkey` command. The amd64
console mirror on debug port `0xe9` supplied exact text evidence without OCR.
The root account used its installed empty password.

## Observed production behavior

- PID 1 reached `init: system running`; the login and shell prompts appeared.
- `stat /run/init.sock` reported a root-owned socket with `mode=c180`. The
  low permission bits are `0600`.
- `service list` returned all six registered services from typed ZSV1 state.
  It showed running `syslogd`, `networkd`, `cron`, and `getty_console`, the
  completed `net` oneshot, and disabled/stopped `ntpdate`.
- `service status networkd` returned enabled/running, `pid=4`, and direct
  `after syslogd` dependency data.
- `service stop cron` returned `OK stopped`; the following status showed
  enabled/stopped with `pid=0`, proving runtime control did not alter policy.
- `service start cron` started PID 16 and returned `OK started`.
- `service restart cron` started PID 18 and returned `OK restarted`.
- `service reload` returned the synchronous `OK reloaded` response.
- `service status bogus` decoded the typed `unknown-service` error and the
  shell remained usable.
- `/sbin/halt` received its ZSV1 acknowledgement and PID 1 then printed
  `init: stopping services`. This verifies the installed action client and
  successful acknowledgement path; the broken-peer/no-action side is covered
  by the production server helper fixture and the conditional PID 1 dispatch
  audit.

The final log scan found none of `fatal:`, `kernel panic`, `panic:`, amd64
fault markers, VFS initialization failure, control-socket failure, invalid
ZSV1 response, ZSV1 transport failure, or a nonzero storage `error=` marker.
The QEMU process was terminated through its monitor after the halt marker.

## Complementary host evidence

The protocol, client, server, and shutdown-argv fixtures directly exercise
the production shared code. Together they cover strict version/error
classification, fragmented I/O and `EINTR`, client `SHUT_WR`, EOF gating,
slow-drip absolute deadlines, response termination, `MSG_NOSIGNAL`, broken
peers, the single client deadline, dependency validation and 32/33 bounds,
and all shutdown argv-to-ZSV1 mappings. ASan/UBSan runs of the protocol,
client, and server fixtures also passed.
