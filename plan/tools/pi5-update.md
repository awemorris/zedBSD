# Bringing the Raspberry Pi 5 up to a new build

The Pi 5 (see the "実機" section of [../tools.md](../tools.md)) runs zedBSD from its own disk. To put a new main on it
without rewriting the disk, only the kernel and the userland files that differ are replaced; the board's own state
stays. The script is [pi5-update.sh](pi5-update.sh); run it from the top of a worktree.

1. Build at the commit to deploy, in the worktree's own `build/` (copy the toolchain with `cp -a`, never a symlink):
   `make CONFIGMK=config/config.rpi5.mk -j4 BUILD_JOBS=4 rootfs < /dev/null` (the `rootfs` target builds `vmunix`
   first). The kernel is `build/rpi5/vmunix`, the root tree `build/rpi5/rootfs`. The first build stops at the
   rpi5-boot-firmware license question; a worktree can take the accepted cache with
   `cp -a <main>/build/sources/firmware/. build/sources/firmware/`.
2. `PI5=10.0.30.5 sh plan/tools/pi5-update.sh plan build/rpi5/rootfs OUT`: the board's cksums of every file on `/`
   (as root) against the tree. `OUT/overlay.txt` lists the files to add or replace, `OUT/kept.txt` those that differ
   but are the board's own (accounts and their databases, `/etc/ssh` host keys, network and rc configuration,
   `/etc/keiland/autologin` and `apps.conf`, `/home`, `/var`, `/root`, `/boot`), `OUT/links.txt` the symbolic links.
   Read the lists before going on. Nothing on the board changes.
3. `PI5=... sh plan/tools/pi5-update.sh deploy build/rpi5/rootfs OUT build/rpi5/vmunix`:
   - the board's copies of the files to be replaced go to `OUT/backup` (also `/tmp/pi5-update.N.bak` on the board,
     gone at the reboot);
   - `/boot/boot0/vmunix` is kept as `vmunix.old`, the new kernel copied in and read back;
   - the files are staged in the board's `/tmp` with scp, and root puts each beside its place with the tree's mode
     and renames it over the old one, so running programs keep the old `libc.so` until the reboot;
   - the cksums are taken again (`OUT/after.cksum`) and each installed file compared.
   `cmdline.txt`, `config.txt` and the dtbs are not touched.
4. `PI5=... sh plan/tools/pi5-update.sh reboot`, then `PI5=... sh plan/tools/pi5-update.sh check build/rpi5/vmunix`
   (uname, the kernel's cksum against the build, the process list, dmesg lines that name a panic, fault or error).
   If the board does not come back, its power is SwitchBot plug mini 1 (`AC276E42B252`); a cycle is Q1's or the
   assigned agent's, once.

To go back: rename `/boot/boot0/vmunix.old` to `vmunix`, and copy `OUT/backup` over `/` the same way (the files
`overlay.txt` names that are not in `OUT/backup` were new).
