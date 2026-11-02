/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* the 2D engine's blit in guest memory (fermi2d.c, with dma.c's bl_span_ptr), the renderer declining it: a block
   linear source scaled down by two into a block linear destination, read back pixel by pixel through the block
   linear layout itself (bl_offset below, the whole formula, against the blit's column table and row base); an odd
   destination window, whose runs start inside a 16 byte group, and nothing written outside it; the same through one
   host pointer when the surfaces are contiguous; and the renderer's hooks called once for the blit, not for each run */
#include "nx/gpu/maxwell/fermi2d.c"
#include "nx/gpu/maxwell/dma.c"

void (*g_gpu_release_note)(u32 ch, u64 job, int kind, int in_macro, u32 entry, u32 entries, u64 detail);

static unsigned checks;
#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        ++checks;                                                                                                      \
        if (!(x)) {                                                                                                    \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);                                                    \
            exit(1);                                                                                                   \
        }                                                                                                              \
    } while (0)

/* ------------------------------------------------ guest memory, its hooks counted */
#define MEM (1u << 20)
static u8 mem[MEM];
static int contiguous; /* gmem_ptr hands out a host pointer */
static unsigned needs, before_writes, written, raw_runs;
void gmem_need(u64 gva, u64 size) {
    (void)gva, (void)size;
    needs++;
}
void gmem_read_raw(u64 gva, void* dst, u64 size) {
    CHECK(gva + size <= MEM);
    memcpy(dst, mem + gva, size);
    raw_runs++;
}
int gmem_read(u64 gva, void* dst, u64 size) {
    gmem_need(gva, size);
    gmem_read_raw(gva, dst, size);
    return 0;
}
void gmem_before_write(u64 gva, u64 size) {
    (void)gva, (void)size;
    before_writes++;
}
void gmem_write_raw(u64 gva, const void* src, u64 size) {
    CHECK(gva + size <= MEM);
    memcpy(mem + gva, src, size);
    raw_runs++;
}
int gmem_write(u64 gva, const void* src, u64 size) {
    gmem_before_write(gva, size);
    gmem_write_raw(gva, src, size);
    gpu_mem_written(gva, size);
    return 0;
}
void gpu_mem_written(u64 gva, u64 size) {
    (void)gva, (void)size;
    written++;
}
u8* gmem_ptr(u64 gva, u64 size) {
    if (!contiguous || gva + size > MEM) return NULL;
    needs++;
    return mem + gva;
}
/* the block linear layout: GOBs of 64 bytes by 8 rows in blocks of 1 << bh GOBs, the GOB's 16 byte groups in
   their order */
u64 bl_offset(u32 x, u32 y, u32 z, u32 wb, u32 h, u32 bh_log2, u32 bd_log2) {
    u32 gob_w = (wb + 63) / 64, bh = 1u << bh_log2, bd = 1u << bd_log2;
    u32 blocks_h = ((h + 7) / 8 + bh - 1) / bh, gy = y / 8, xb = x % 64, yb = y % 8;
    u64 block = 512ull * bh * bd;
    u64 o = ((u64)(z / bd) * blocks_h * gob_w + (u64)(gy / bh) * gob_w + x / 64) * block +
            ((u64)(z % bd) * bh + gy % bh) * 512;
    return o + (xb / 32) * 256 + (yb / 2) * 64 + ((xb % 32) / 16) * 32 + (yb % 2) * 16 + xb % 16;
}
/* what dma.c's own copies reach, which the test does not run */
void (*g_gpu_before_write)(u64 gva, u64 size);
void gmem_w32(u64 gva, u32 v) { gmem_write(gva, &v, 4); }
void gpu_signal_point(void) {}
void gpu_barrier_point(void) {}
void gpu_release_write(u64 gva, const void* data, u32 len) { gmem_write(gva, data, len); }
u64 gpu_ticks(void) { return 0; }
/* dma.c's snapshots of linear sources: one thread here, and no GPU write ever lands */
void pm_lock(PMutex* m) { (void)m; }
void pm_unlock(PMutex* m) { (void)m; }
u64 gpu_mem_generation(void) { return 0; }
int gpu_mem_dirty_since(u64 since, u64 a, u64 sz) {
    (void)since, (void)a, (void)sz;
    return 0;
}
u32 gpu_color_bpp(u32 format) {
    (void)format;
    return 4;
}
int r3d_blit(const Surf2D* s, const Surf2D* d, int dx, int dy, int dw, int dh, double sx, double sy, double sdx,
             double sdy) {
    (void)s, (void)d, (void)dx, (void)dy, (void)dw, (void)dh, (void)sx, (void)sy, (void)sdx, (void)sdy;
    return 0; /* the renderer declines: the blit runs in guest memory */
}
static void no_target(u64 gva) { (void)gva; }
static const Renderer stand_in = {.name = "stand-in", .rt_flush = no_target, .rt_reload = no_target};
const Renderer* g_renderer = &stand_in;
int g_renderer_up = 1;
int g_verbose;
void logf_(int lvl, const char* fmt, ...) {
    (void)lvl;
    (void)fmt;
}
void fatal(const char* fmt, ...) {
    (void)fmt;
    exit(1);
}

/* ------------------------------------------------ the blit */
#define SRC 0x00000
#define DST 0x40000
enum { SW = 128, SH = 32, DW = 64, DH = 16 };
static u32 src_px(u32 x, u32 y) { return 0x10000000u | y << 12 | x; }
static u32 at(u64 base, u32 x, u32 y, u32 w, u32 h) {
    u32 v;
    memcpy(&v, mem + base + bl_offset(x * 4, y, 0, w * 4, h, 1, 0), 4);
    return v;
}
static void surface(GpuChannel* ch, int b, u64 addr, u32 w, u32 h) {
    u32* r = ch->r2d;
    r[b] = 0xd5;        /* A8B8G8R8 */
    r[b + 1] = 0;       /* block linear */
    r[b + 2] = 1u << 4; /* two GOBs a block */
    r[b + 3] = 1;       /* depth */
    r[b + 4] = 0;       /* layer */
    r[b + 6] = w;
    r[b + 7] = h;
    r[b + 8] = (u32)(addr >> 32);
    r[b + 9] = (u32)addr;
}
/* dw x dh at dx, dy from a sw-wide source scaled down by two (corner origin, no half texel) */
static void blit_from(u32 sw, i32 dx, i32 dy, i32 dw, i32 dh) {
    static GpuChannel ch;
    memset(&ch, 0, sizeof ch);
    surface(&ch, 0x80, DST, DW, DH);
    surface(&ch, 0x8C, SRC, sw, SH);
    u32* r = ch.r2d;
    r[0x223] = 1;
    r[0x22C] = (u32)dx;
    r[0x22D] = (u32)dy;
    r[0x22E] = (u32)dw;
    r[0x22F] = (u32)dh;
    r[0x231] = 2; /* du/dx 2.0 */
    r[0x233] = 2; /* dv/dy 2.0 */
    needs = before_writes = written = raw_runs = 0;
    fermi2d_method(&ch, 0x237, 0); /* src_y0 starts it */
}
static void blit(i32 dx, i32 dy, i32 dw, i32 dh) { blit_from(SW, dx, dy, dw, dh); }
static void fill_from(u32 sw) {
    memset(mem, 0xEE, sizeof mem);
    for (u32 y = 0; y < SH; y++)
        for (u32 x = 0; x < sw; x++) {
            u32 v = src_px(x, y);
            memcpy(mem + SRC + bl_offset(x * 4, y, 0, sw * 4, SH, 1, 0), &v, 4);
        }
}
static void fill(void) { fill_from(SW); }
static void whole(int direct) {
    contiguous = direct;
    fill();
    blit(0, 0, DW, DH);
    for (u32 y = 0; y < DH; y++)
        for (u32 x = 0; x < DW; x++) CHECK(at(DST, x, y, DW, DH) == src_px(2 * x, 2 * y));
    CHECK(before_writes == 1 && written == 1 && needs == (direct ? 2u : 1u));
    CHECK(direct ? raw_runs == 0 : raw_runs > 0);
}
static void window(int direct) {
    contiguous = direct;
    fill();
    blit(5, 3, 21, 9); /* runs start mid group at 20 bytes, end mid group at 104 */
    for (u32 y = 0; y < DH; y++)
        for (u32 x = 0; x < DW; x++) {
            int inside = x >= 5 && x < 26 && y >= 3 && y < 12;
            u32 v = at(DST, x, y, DW, DH);
            CHECK(inside ? v == src_px(2 * (x - 5), 2 * (y - 3)) : v == 0xEEEEEEEEu);
        }
    CHECK(before_writes == 1 && written == 1);
}

/* a source 37 pixels wide: its rows end 4 bytes into a 16 byte run */
static void odd_source(int direct) {
    contiguous = direct;
    fill_from(37);
    blit_from(37, 0, 0, 19, 16);
    for (u32 y = 0; y < DH; y++)
        for (u32 x = 0; x < DW; x++) {
            u32 v = at(DST, x, y, DW, DH);
            CHECK(x < 19 ? v == src_px(2 * x, 2 * y) : v == 0xEEEEEEEEu);
        }
}

int main(void) {
    whole(0);
    whole(1);
    window(0);
    window(1);
    odd_source(0);
    odd_source(1);
    printf("fermi2d_blit: %u checks passed\n", checks);
    return 0;
}
