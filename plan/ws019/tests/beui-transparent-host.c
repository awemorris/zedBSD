/* Exercise the actual packaged adapter against a patterned framebuffer. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <stdarg.h>
#include <assert.h>
#include <sys/ioctl.h>
#define ZEDBSD_IOC_OUT _IOC_READ
#define ZEDBSD_IOC(direction, group, number, size) _IOC(direction, group, number, size)
static int mock_ioctl(int, unsigned long, ...);
#define ioctl mock_ioctl
#include "userland/base/noct/noct/src/api/api-beui-zedbsd.c"
#undef ioctl

static uint32_t pixels[32][32];
static uint8_t ink[4] = {0xa5, 0xf0, 0x00, 0xff};
static unsigned fills, blits;
static int short_bitmap;
static int fail_fill;
static int returned_integer;

/* The fixture needs only the VM's integer return slot, not a running VM. */
bool noct_pin_local(NoctEnv *env, uint32_t count, ...)
{
    (void)env; assert(count == 1); return true;
}
bool noct_unpin_local(NoctEnv *env, uint32_t count, ...)
{
    (void)env; assert(count == 1); return true;
}
bool noct_set_return_make_int(NoctEnv *env, NoctValue *value, int integer)
{
    (void)env; (void)value; returned_integer=integer; return true;
}
static int mock_ioctl(int fd, unsigned long request, ...)
{
    va_list ap;
    void *argument;
    struct graphics_glyph *glyph;
    struct graphics_fill *fill_request;
    struct graphics_blit *blit;
    unsigned x, y;
    (void)fd;
    va_start(ap, request);
    argument = va_arg(ap, void *);
    va_end(ap);
    if (request == ZEDBSD_GRAPHICS_GET_GLYPH) {
        glyph = argument;
        assert(glyph->bitmap_capacity >= sizeof(ink));
        memcpy((void *)(uintptr_t)glyph->bitmap, ink, sizeof(ink));
        glyph->width = 8;
        glyph->height = 4;
        glyph->stride = 1;
        glyph->advance = 8;
        glyph->bitmap_size = short_bitmap ? 1 : sizeof(ink);
        return 0;
    }
    if (request == ZEDBSD_GRAPHICS_FILL_RECT) {
        fills++;
        if (fail_fill) return -1;
        fill_request = argument;
        for (y=0; y<fill_request->rect.height; y++) {
            for (x=0; x<fill_request->rect.width; x++) {
                assert(fill_request->rect.y+y < 32 && fill_request->rect.x+x < 32);
                pixels[fill_request->rect.y+y][fill_request->rect.x+x] = fill_request->color;
            }
        }
        return 0;
    }
    assert(request == ZEDBSD_GRAPHICS_BLIT);
    blits++;
    blit = argument;
    assert(blit->format == ZEDBSD_GRAPHICS_FORMAT_MONO1 && blit->background == 0x123456);
    return 0;
}
int main(void)
{
    struct backend_context context;
    unsigned x,y;
    uint32_t expected;
    int first[2], second[2];
    memset(&context, 0, sizeof(context));
    context.graphics_capabilities = ZEDBSD_GRAPHICS_CAP_GLYPH | ZEDBSD_GRAPHICS_CAP_BLIT_MONO1 | ZEDBSD_GRAPHICS_CAP_FILL;
    for (y=0; y<32; y++) for(x=0; x<32; x++) pixels[y][x] = y*32+x;
    assert(zedbsd_glyph_draw(&context, 5, 6, 'a', 0xffffff, UINT32_MAX));
    for (y=0; y<32; y++) for(x=0; x<32; x++) {
        expected = y*32+x;
        if (x>=5 && x<13 && y>=6 && y<10 && (ink[y-6] & (0x80U>>(x-5)))) expected=0xffffff;
        assert(pixels[y][x] == expected);
    }
    assert(fills == 6 && blits == 0);
    assert(zedbsd_glyph_draw(&context, 5, 6, 'a', 0xffffff, 0x123456));
    assert(fills == 6 && blits == 1);
    short_bitmap = 1;
    assert(!zedbsd_glyph_draw(&context, 5, 6, 'a', 0xffffff, UINT32_MAX));
    assert(fills == 6);
    short_bitmap = 0;
    fail_fill = 1;
    assert(!zedbsd_glyph_draw(&context, 5, 6, 'a', 0xffffff, UINT32_MAX));
    puts("BeUI transparent foreground/background preservation PASS");

    /* Exercise actual source retirement on EOF without scanning host devices. */
    memset(&backend, 0, sizeof(backend));
    for(x=0; x<INPUT_MAX_SOURCES; x++) backend.sources[x].fd=-1;
    backend.next_rescan=UINT64_MAX;
    backend.input_initialized=1;
    state.display_open=1;
    assert(cfunc_BeUI_hasInput(NULL) && returned_integer==0);
    assert(pipe(first)==0 && pipe(second)==0);
    assert(fcntl(first[0],F_SETFL,O_NONBLOCK)==0);
    assert(fcntl(second[0],F_SETFL,O_NONBLOCK)==0);
    backend.sources[0].fd=first[0];
    backend.sources[1].fd=second[0];
    assert(cfunc_BeUI_hasInput(NULL) && returned_integer==1);
    close(first[1]);
    assert(cfunc_BeUI_hasInput(NULL) && returned_integer==1);
    assert(backend.sources[0].fd==-1 && backend.sources[1].fd==second[0]);
    close(second[1]);
    assert(cfunc_BeUI_hasInput(NULL) && returned_integer==0);
    assert(backend.sources[1].fd==-1);
    state.display_open=0;
    assert(cfunc_BeUI_hasInput(NULL) && returned_integer==0);
    puts("BeUI input EOF retirement and remaining-source observation PASS");
    return 0;
}
