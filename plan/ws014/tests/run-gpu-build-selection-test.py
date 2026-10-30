#!/usr/bin/env python3
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
"""Check actual make-expanded GPU ownership for every existing platform list."""

import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[3]
TARGETS = (
    ("amd64", "amd64", "pcat", "AMD64_KERNEL_OBJS"),
    ("rpi4", "arm64", "rpi4", "ARM64_KERNEL_OBJS"),
    ("i386", "i386", "pcat", "KERN_OBJS"),
    ("pc98", "i386", "pc98", "KERN_OBJS"),
    ("sun4u", "sparcv9", "sun4u", "SPARCV9_KERNEL_OBJS"),
    ("x68k", "m68k", "x68k", "X68K_KERNEL_OBJS"),
)


def main():
    with tempfile.TemporaryDirectory(prefix="zedbsd-gpu-build-") as work:
        observer = pathlib.Path(work) / "observe.mk"
        observer.write_text(
            ".PHONY: q311-gpu-selection\n"
            "q311-gpu-selection:\n"
            "\t@printf '%s\\n' '$(KERN_GPU_SOURCES)' "
            "'$(filter %/gpu.o %/gpu-fence.o,$($(Q311_OBJECT_LIST)))' "
            "'$(filter %/handle.o %/fd-object.o,$($(Q311_OBJECT_LIST)))' "
            "'$(filter %/src/kern/fence.o,$($(Q311_OBJECT_LIST)))'\n"
        )
        for platform, architecture, board, objects in TARGETS:
            for selected in ("n", "y"):
                result = subprocess.run(
                    [
                        "make", "--no-print-directory", "-s", "-f", "Makefile",
                        "-f", str(observer), "ZEDBSD_CONFIG=/dev/null",
                        f"ZEDBSD_PLATFORM={platform}",
                        f"ZEDBSD_ARCHITECTURE={architecture}",
                        f"ZEDBSD_BOARD={board}",
                        # Every backend that selects the GPU framework.
                        f"CONFIG_DRIVER_PCI_VENUS={selected}",
                        f"CONFIG_DRIVER_PCI_I915={selected}",
                        f"CONFIG_DRIVER_PCI_GHCI={selected}",
                        f"CONFIG_DRIVER_BCM2711_GPU={selected}",
                        f"Q311_OBJECT_LIST={objects}", "q311-gpu-selection",
                    ], cwd=ROOT, text=True, capture_output=True, timeout=30,
                    check=True,
                )
                lines = result.stdout.splitlines()
                assert len(lines) == 4, (platform, selected, result.stdout)
                sources, gpu_objects, generic_objects, old_fence = lines
                assert not old_fence, (platform, old_fence)
                assert len(generic_objects.split()) == 2, (platform, generic_objects)
                if selected == "n":
                    assert not sources and not gpu_objects, (platform, lines)
                else:
                    assert sources.split() == [
                        "src/drivers/gpu/gpu.c", "src/drivers/gpu/gpu-fence.c"
                    ], (platform, sources)
                    assert len(gpu_objects.split()) == 2, (platform, gpu_objects)
                print(f"GPU build selection: {platform} backends={selected} PASS")
    print("GPU framework and fence are conditional together; generic fd/handle remain in all six lists PASS")


if __name__ == "__main__":
    main()
