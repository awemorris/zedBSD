# Reference

Status: current navigation index

Reference documents define the commands, configuration formats, headers,
structures, constants, errors and permissions that programs and people use
([rules](../style.md)).

- [C interfaces and compatibility profile](compatibility-profile.md): feature
  selectors, ABI boundaries and conformance limits.
- [Console, graphics and system controls](control-devices.md): operations,
  ownership, permissions and the held GPU proposal.
- [Managed WLAN administration](managed-wlan.md) — current `net wifi`,
  credential-store, selection, state, recovery, and permission contract.
- [Network configuration console](network-console.md) — current candidate,
  atomic `commit`, temporary `commit confirmed`, and `rollback` contract.
- [Kernel boot parameters](kernel-boot-parameters.md) — current
  `boot0`–`boot3`, root-mode, `swap0`–`swap3`, and `init` contract for
  the four x86 production loaders, with the non-x86 NULL-source compatibility
  boundary.
- [evdev compatibility profile](evdev.md) — experimental UAPI with current
  input-core and USB HID producers; legacy console event/key-state UAPI has
  been removed while ordinary TTY and display operations remain.
- [Agent Acceptance Test interfaces](agent-acceptance-test.md): the test
  images' mouse and keyboard injection and screen capture.
- [Camera devices](camera.md): the /dev/camN nodes, their ownership,
  formats, buffers (including GPU memory imported from Vulkan), frame
  metadata, USB limits and suspend.
- [Security keys and smart card readers](security-keys.md): the raw HID nodes
  of FIDO keys and the slots of USB CCID readers, their requests and
  permissions.
- [Media playback](media-playback.md): the native media library's AAC-LC audio
  and H.264 video decoding on Vulkan Video, and what a GPU must provide.
- [Vulkan Video decode](vulkan-video.md): H.264 decode on Intel Gen12's video
  engine through the Vulkan Video extensions, its limits, result status and
  differences from the specification.
- [GPU device interface](gpu.md): the /dev/gpuN nodes, their permissions,
  capabilities, memory, sharing, submission, fences, jobs, display, command
  stream and sysctl variables, with the index of every request.
- [VisionFive 2 GPU](visionfive2-gpu.md): the PowerVR BXE-4-32's Vulkan
  range, memory, firmware and licence, start-up stages and the test programs
  gpu-selftest, vkmatrix and vkreset.

## System administration

- [Init and service management](init-services.md): boot sequencing,
  `rc.conf`, `service.d`, FD 3 readiness, supervision, control, and shutdown.

- [useradd, usermod and userdel](user-administration.md): root's commands
  that add, change and remove the people's accounts through account-admin.

- [x86 executable TLS](tls.md) — static PT_TLS, thread-pointer layout, pthread/fork lifetime, and dynamic-runtime boundary.

- [Atomic file publication](atomic-publication.md): no-replace rename, mv and directory durability.

- [Retained boot-source identities](boot-provenance.md): UEFI source selectors and configuration ambiguity.

- [Structured block and file command output](block-command-output.md): diskpart machine records, blkid export and stat formats.

- [Regular-file image formatters](image-formatters.md): UFS/ZEDSWAP2 initialization and read-only pristine verification.

- [Installer, install-admin and efiboot](installer.md): the amd64 UEFI installer's image and its rule, the installer's screens, the helper's requests, the installed system's layout and UEFI boot entries.
