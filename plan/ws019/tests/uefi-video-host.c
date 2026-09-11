/* Host firmware mock: optional mode selection, ownership, and refusal paths. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "bootloader/uefi/video.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static EFI_GRAPHICS_OUTPUT_MODE_INFORMATION modes[3];
static EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE active;
static unsigned queries, releases, sets;
static int fault;

static EFI_STATUS EFIAPI query(EFI_GRAPHICS_OUTPUT_PROTOCOL *gop, UINT32 index,
    UINTN *size, EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **info)
{
    (void)gop;
    queries++;
    if (fault == 1) return EFI_DEVICE_ERROR;
    if (fault == 2) return EFI_SUCCESS;
    *info = malloc(sizeof(**info));
    assert(*info != NULL);
    **info = modes[index];
    *size = fault == 3 ? 1 : sizeof(**info);
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI release(void *buffer)
{
    releases++;
    free(buffer);
    return fault == 4 ? EFI_DEVICE_ERROR : EFI_SUCCESS;
}
static EFI_STATUS EFIAPI select_mode(EFI_GRAPHICS_OUTPUT_PROTOCOL *gop, UINT32 index)
{
    sets++;
    if (fault == 5) return EFI_DEVICE_ERROR;
    if (fault == 6) return EFI_SUCCESS;
    gop->Mode->Mode = index;
    gop->Mode->Info = &modes[index];
    gop->Mode->FrameBufferBase = 0x80000000U;
    return EFI_SUCCESS;
}
static void reset(void)
{
    unsigned i;
    memset(&active, 0, sizeof(active));
    memset(modes, 0, sizeof(modes));
    for (i = 0; i < 3; i++) {
        modes[i].HorizontalResolution = i == 0 ? 1280 : 640;
        modes[i].VerticalResolution = i == 0 ? 800 : 480;
        modes[i].PixelsPerScanLine = modes[i].HorizontalResolution;
        modes[i].PixelFormat = i == 1 ? PixelBltOnly : PixelBlueGreenRedReserved8BitPerColor;
    }
    active.MaxMode = 3;
    active.Info = &modes[0];
    active.SizeOfInfo = sizeof(modes[0]);
    queries = releases = sets = 0;
    fault = 0;
}
int main(void)
{
    EFI_BOOT_SERVICES boot;
    EFI_GRAPHICS_OUTPUT_PROTOCOL gop;
    EFI_STATUS status;
    const char *bad[] = {"video=", "video=0x480", "video=0640x480", "video=640X480",
        "video=640x480z", "video=640x480x1", "video=+640x480", "video=640x-480",
        "video=16385x480", "video=640x99999999999999999999999", "video=640x480 video=640x480"};
    unsigned i;
    memset(&boot, 0, sizeof(boot));
    memset(&gop, 0, sizeof(gop));
    boot.FreePool = release;
    gop.QueryMode = query;
    gop.SetMode = select_mode;
    gop.Mode = &active;
    reset();
    assert(zbl_uefi_video_select(NULL, NULL, "", 0) == EFI_SUCCESS);
    assert(zbl_uefi_video_select(&boot, &gop, "init=/video=640x480", strlen("init=/video=640x480")) == EFI_SUCCESS);
    assert(queries == 0 && sets == 0);
    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        assert(zbl_uefi_video_select(&boot, &gop, bad[i], strlen(bad[i])) == EFI_INVALID_PARAMETER);
        assert(queries == 0 && sets == 0);
    }
    assert(zbl_uefi_video_select(&boot, &gop, "v\0x", 3) == EFI_INVALID_PARAMETER);
    assert(zbl_uefi_video_select(&boot, &gop, "video=640x480!", 13) == EFI_INVALID_PARAMETER);
    assert(zbl_uefi_video_select(&boot, &gop, "", 3072) == EFI_INVALID_PARAMETER);
    status = zbl_uefi_video_select(&boot, &gop, "video=640x480", 13);
    assert(status == EFI_SUCCESS && sets == 1 && queries == 3 && releases == 3);
    assert(active.Mode == 2 && active.FrameBufferBase == 0x80000000U);
    assert(zbl_uefi_video_select(&boot, &gop, "video=640x480", 13) == EFI_SUCCESS);
    assert(sets == 1 && queries == 3);
    for (i = 1; i <= 6; i++) {
        reset();
        fault = (int)i;
        status = zbl_uefi_video_select(&boot, &gop, "video=640x480", 13);
        assert(EFI_ERROR(status));
        assert(releases == (i <= 2 ? 0 : queries));
    }
    reset();
    modes[2].PixelsPerScanLine = 639;
    assert(zbl_uefi_video_select(&boot, &gop, "video=640x480", 13) == EFI_UNSUPPORTED);
    assert(sets == 0 && releases == 3);
    reset();
    active.MaxMode = 4097;
    assert(zbl_uefi_video_select(&boot, &gop, "video=640x480", 13) == EFI_UNSUPPORTED);
    assert(queries == 0);
    puts("UEFI video selection PASS");
    return 0;
}
