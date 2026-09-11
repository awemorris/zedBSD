# q160 QEMU-accepted coexistence candidate

Date: 2026-09-09. This supplies WS019-p005's candidate record; it is not physical
Latitude acceptance or permission to overwrite its disk.

Installer source: /home/awe/zedBSD/build/amd64/hdd-image.img

SHA-256: 4425d3655705ae08c6b3296d5cab88259dfebf8337635fe397acd576bdc93d0d

Accepted installed QEMU target:
/home/awe/zedBSD/plan/ws019/temp/q159-public6/gpt.img

SHA-256: 0cc79ab8c01c498fd313651969a27670f306eb8416ec9e90554578f72af46f77

The target is a disposable synthetic five-GiB GPT image, not a whole-disk
deployment image for the Latitude. Preserve it; revalidate the source hash
before any future use because the build path will change during development.
The generated boot0 is explicit PARTUUID=78190000-2222-4222-8222-222222222222;
do not use this synthetic identity for physical installation.

[QEMU results](../../ws019/phase005/q160-results.md)
cover installed NVMe-only boot, overlay/swap, persistence and configuration
selection. WS003-p018's physical dependencies and acceptance remain unchanged.
