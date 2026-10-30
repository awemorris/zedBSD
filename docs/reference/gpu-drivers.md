# GPU drivers and node kinds

Status: experimental reference for GPU ABI version 1.

<!-- spec: One paragraph: which GPU drivers publish /dev/gpuN nodes, how
many nodes each physical GPU publishes and of which kind, and what each
kind of node offers. The capability bits are the authority at run time
(GPU_GET_INFO); this table explains them. Link back to gpu.md and
gpu-capabilities.md. -->

PENDING

## Node kinds

<!-- spec: One table, one row per kind of node (not per driver): find
every caller of drv_gpu_register under src/drivers/gpu/ (grep; today
amdrdna/node.c, bcm2711/render-device.c, bcm2711/display/display-device.c,
ghci/ghci.c, i915/i915.c, mali/render.c, mali/display/display.c,
nvrtx/node.c, powervr/render.c, powervr/display/display.c,
venus/venus.c) and, for each, the ops structure's capabilities value
(follow macros such as VENUS_CAPABILITIES to their numeric value) and
every place that adds bits. Where one driver publishes different kinds
(i915: the resident display, the boot-framebuffer path and the capture
path; read i915/display/display.c, i915/display/bootfb.c,
i915/display/capture.c), give each its own row. Columns: | Driver | Node kind | Architectures | Capability bits
(names) | Bits added later and when | Display behaviour | Source |.
Capability bits lists names, not numbers. "Bits added later" says
whether a runtime binding adds or removes bits (amdrdna/node.c and
amdrdna/present.c, nvrtx/node.c, powervr/render-runtime.c,
bcm2711/render-runtime.c, mali/sched.c and mali/render.c, ghci/ghci.c and
ghci/vk/vk-node.c, any other found by grep), and whether that happens
before or after drv_gpu_register publishes the node (read the order of calls; if
after, a program that opened the node earlier sees new bits on its next
GPU_GET_INFO; say so exactly). Display behaviour gives the output flags
that tell when a present returns (GPU_DISPLAY_FIFO,
GPU_DISPLAY_VIRTUAL_CLOCK, GPU_DISPLAY_ATOMIC_MODE_PRESENT), whether
presents wait for adoption, how the driver's display wait operation
bounds GPU_DISPLAY_WAIT, what the output shows after a lease ends, and
whether the node exposes cursor and overlay planes. Architectures from config/drivers/ and the drivers'
build rules. -->

PENDING

## Render and display pairs

<!-- spec: Which drivers split rendering and display across two nodes
(bcm2711, powervr, mali, any other), how a program finds the pair
(GPU_DEVICE_QUERY companion_id, gpu-memory.md) and which sharing route
they use (GPU_DISPLAY_CONSTRAINTS flags, gpu-display.md). -->

PENDING

## Known differences between capability bits and behaviour

<!-- spec: List each case where a set bit does not mean every guarded
request works on that node, with the condition and the errno: at least
venus with GPU_CAP_JOB while the host has no strict queue (GPU_JOB_RESERVE
and GPU_JOB_CAPACITY answer ENOTSUP; read venus/venus.c venus_get_info
and venus/transport.c). Describe the behaviour as it is; do not call it a
defect or promise a change. -->

PENDING

## Driver-specific documents

<!-- spec: Links to visionfive2-gpu.md (PowerVR on VisionFive 2) and any
other existing docs/reference document about one GPU (search
docs/reference for the driver names). -->

PENDING
