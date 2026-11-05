# zedBSD arm64: the Mali Valhall GPU driver and its display node.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

# The GPU's attach, the S7D's clocks and resets, the GPU control block,
# its address spaces, the firmware's file and image, the command stream
# front end and its interface, the driver's own stream, the render node
# with its scheduler, command stream builder, Vulkan executor and shader
# compiler, and the display node on the bootloader's screen and the
# VPU's first OSD plane.
ARM64_MALI_SOURCES := \
	src/drivers/gpu/mali/mali-attach.c \
	src/drivers/gpu/mali/mali-s7d.c \
	src/drivers/gpu/mali/mali-gpu.c \
	src/drivers/gpu/mali/mali-pagetable.c \
	src/drivers/gpu/mali/mali-mmu.c \
	src/drivers/gpu/mali/mali-firmware-file.c \
	src/drivers/gpu/mali/mali-firmware-image.c \
	src/drivers/gpu/mali/mali-interface.c \
	src/drivers/gpu/mali/mali-csf.c \
	src/drivers/gpu/mali/mali-queue.c \
	src/drivers/gpu/mali/cs/cs-build.c \
	src/drivers/gpu/mali/render.c \
	src/drivers/gpu/mali/sched.c \
	src/drivers/gpu/mali/vk/vk-object.c \
	src/drivers/gpu/mali/vk/vk-stream.c \
	src/drivers/gpu/mali/vk/vk-format.c \
	src/drivers/gpu/mali/vk/vk-device.c \
	src/drivers/gpu/mali/vk/vk-memory.c \
	src/drivers/gpu/mali/vk/vk-layout.c \
	src/drivers/gpu/mali/vk/vk-pipeline.c \
	src/drivers/gpu/mali/vk/vk-command.c \
	src/drivers/gpu/mali/vk/vk-desc.c \
	src/drivers/gpu/mali/vk/vk-surface.c \
	src/drivers/gpu/mali/vk/vk-queue.c \
	src/drivers/gpu/mali/vk/vk-draw.c \
	src/drivers/gpu/mali/vk/vk-frame.c \
	src/drivers/gpu/mali/vk/vk-preload.c \
	src/drivers/gpu/mali/vk/vk-meta.c \
	src/drivers/gpu/mali/vk/vk-meta-spirv.c \
	src/drivers/gpu/mali/compiler/va-compile.c \
	src/drivers/gpu/mali/compiler/va-encode.c \
	src/drivers/gpu/mali/compiler/va-schedule.c \
	$(filter-out $(KERN_GPU_SOURCES),src/drivers/gpu/compiler/spirv.c src/drivers/gpu/compiler/optimize.c src/drivers/gpu/i915/vk/vk-codec.c) \
	src/drivers/gpu/mali/display/osd.c \
	src/drivers/gpu/mali/display/scanout.c \
	src/drivers/gpu/mali/display/display.c
