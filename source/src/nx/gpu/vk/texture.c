/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Textures from the guest's texture headers (TIC, 32 bytes each), and samplers from its sampler headers (TSC). A
   texture whose address holds a render target is that render target (a copy of its top-left corner when the header
   is smaller). Others are loaded from guest memory, deswizzled on the CPU, and checked for changes now and then:
   often at first, rarely once they have stayed the same. */
#include <stdlib.h>
#include <string.h>
#include "nx/gpu/gpu.h"
#include "nx/gpu/renderer.h"
#include "nx/gpu/texture/astc.h"
#include "nx/gpu/texture/bc7.h"
#include "nx/gpu/texture/etc2.h"
#include "nx/gpu/texture/texpack.h"
#include "nx/runtime/main.h"
#include "nx/runtime/platform/log.h"
#include "nx/gpu/vk/vk.h"
#include "nx/gpu/vk/prompts.h"
#include "nx/gpu/vk/nvn_res.h"
#include "nx/runtime/platform/plat.h"

/* the block height and depth (log2 GOBs) of a mip level of a block-linear texture. The hardware shrinks them for
   small levels: the height from the base level on, the depth only from level 1 on and by one step a level
   (Ryujinx's SizeCalculator) */
static void bl_level_blocks(u32 h, u32 d, const VkFmt* f, u32 bh, u32 bd, u32 level, u32* lbh, u32* lbd) {
    u32 lh = h >> level ? h >> level : 1, gy = ((lh + f->bh - 1) / f->bh + 7) / 8;
    while (bh > 0 && gy <= (1u << (bh - 1))) bh--;
    for (u32 l = 1; l <= level; l++) {
        u32 ld = d >> l ? d >> l : 1;
        if (bd > 0 && ld <= (1u << (bd - 1))) bd--;
    }
    *lbh = bh;
    *lbd = bd;
}
/* the bytes from one row of a block-linear level to the next as the GOB layout counts them: the row of tw blocks, and
   with the texture header's tile width spacing tws (word 3 bits 10-12) aligned to 64 << tws bytes, unless the level is
   smaller than such a GOB (at most that wide, at most 8 << bh rows high, or less than 1 << bd deep): Eden's
   StrideAlignment and NumGobs (src/video_core/texture_cache/util.cpp, GPL-2.0-or-later, from yuzu and Ryujinx).
   Bayonetta 3's 1026-texel-wide ASTC 6x6 UI atlases have it: read 171 blocks a row instead of 176, their sprites came
   out shifted with padding between them */
static u32 bl_row_bytes(u32 tw, u32 th, u32 td, const VkFmt* f, u32 bh, u32 bd, u32 tws) {
    u32 rb = tw * f->bpp, al = 64u << tws;
    if (!tws || rb <= al || th <= (8u << bh) || td < (1u << bd)) return rb;
    return (rb + al - 1) / al * al;
}
/* bytes of one mip level of a block-linear texture; lbh and lbd (may be NULL) get its block height and depth */
static u64 bl_level_size(u32 w, u32 h, u32 d, const VkFmt* f, u32 bh, u32 bd, u32 tws, u32 level, u32* lbh, u32* lbd) {
    u32 lw = w >> level ? w >> level : 1, lh = h >> level ? h >> level : 1, ld = d >> level ? d >> level : 1;
    u32 tw = (lw + f->bw - 1) / f->bw, th = (lh + f->bh - 1) / f->bh;
    u32 gx = (bl_row_bytes(tw, th, ld, f, bh, bd, tws) + 63) / 64, gy = (th + 7) / 8, b, bdd;
    bl_level_blocks(h, d, f, bh, bd, level, &b, &bdd);
    if (lbh) *lbh = b;
    if (lbd) *lbd = bdd;
    u64 gys = ((gy + (1u << b) - 1) >> b) << b;
    u64 lds = ((ld + (1u << bdd) - 1) >> bdd) << bdd;
    return (u64)gx * gys * lds * 512;
}
/* bytes from one layer of a block-linear texture to the next: all its levels, aligned to a block of GOBs that is
   shrunk for a small base level (Ryujinx's AlignLayerSize) */
static u64 bl_layer_size(u32 w, u32 h, u32 d, const VkFmt* f, u32 bh, u32 bd, u32 tws, u32 levels) {
    u64 sz = 0;
    for (u32 l = 0; l < levels; l++) sz += bl_level_size(w, h, d, f, bh, bd, tws, l, NULL, NULL);
    u32 gy = ((h + f->bh - 1) / f->bh + 7) / 8;
    while (bh > 0 && gy <= (1u << (bh - 1))) bh--;
    while (bd > 0 && d <= (1u << (bd - 1))) bd--;
    u64 al = 512ull << (bh + bd);
    return (sz + al - 1) / al * al;
}

/* one level of one layer, from guest bytes to tightly packed rows, converted when the format needs it */
static void level_convert(const VkFmt* f, const u8* lin, u32 lw, u32 lh, u32 ld, u32 rowb, u8* out) {
    u32 th = (lh + f->bh - 1) / f->bh;
    if (f->conv >= VKC_ETC2) { /* each slice's blocks, decoded */
        int kind = f->conv - VKC_ETC2;
        for (u32 z = 0; z < ld; z++)
            etc2_decode_image(lin + (u64)z * rowb * th, lw, lh, kind, out + (u64)z * lw * lh * etc2_pixel_bytes(kind));
        return;
    }
    switch (f->conv) {
        case VKC_RGBA4:
        case VKC_RGB5A1:
        case VKC_A5B5G5R1:
            for (u64 i = 0; i < (u64)lw * lh * ld; i++) {
                u32 v = ((const u16*)lin)[i], r, g, b, a;
                if (f->conv == VKC_RGBA4) {
                    r = (v & 15) * 17;
                    g = ((v >> 4) & 15) * 17;
                    b = ((v >> 8) & 15) * 17;
                    a = ((v >> 12) & 15) * 17;
                } else if (f->conv == VKC_RGB5A1) {
                    r = (v & 31) * 255 / 31;
                    g = ((v >> 5) & 31) * 255 / 31;
                    b = ((v >> 10) & 31) * 255 / 31;
                    a = (v >> 15) ? 255 : 0;
                } else { /* A5B5G5R1: one bit of red at the bottom, then green, blue and alpha (Ryujinx's swizzle) */
                    r = (v & 1) ? 255 : 0;
                    g = ((v >> 1) & 31) * 255 / 31;
                    b = ((v >> 6) & 31) * 255 / 31;
                    a = ((v >> 11) & 31) * 255 / 31;
                }
                ((u32*)out)[i] = r | (g << 8) | (b << 16) | (a << 24);
            }
            break;
        case VKC_RGB32F:
            for (u64 i = 0; i < (u64)lw * lh * ld; i++) {
                float* o = (float*)out + i * 4;
                memcpy(o, lin + i * 12, 12);
                o[3] = 1.0f;
            }
            break;
        default: memcpy(out, lin, (u64)rowb * th * ld);
    }
}
static u64 level_out_size(const VkFmt* f, u32 lw, u32 lh, u32 ld, u32 rowb) {
    u32 th = (lh + f->bh - 1) / f->bh;
    if (f->conv >= VKC_ETC2) return (u64)lw * lh * ld * etc2_pixel_bytes(f->conv - VKC_ETC2);
    switch (f->conv) {
        case VKC_RGBA4:
        case VKC_RGB5A1:
        case VKC_A5B5G5R1: return (u64)lw * lh * ld * 4;
        case VKC_RGB32F: return (u64)lw * lh * ld * 16;
    }
    return (u64)rowb * th * ld;
}

/* one level of one layer as tightly packed rows, from its lsz guest bytes at src. A block-linear row's 16-byte pieces:
   the row's place (bl_offset at x 0), then a block of GOBs per 64 bytes, and within a GOB's row 32-byte halves 256
   bytes apart and 16-byte quarters 32 apart. A whole piece is one 16-byte move rather than a call to memcpy: a
   texture rewritten every frame (a video's pictures) comes through here each time */
static void level_rows(const Surf* t, const u8* src, u64 lsz, u32 rowb, u32 th, u32 ld, u32 lbh, u32 lbd, u8* out) {
    if (!t->bl) {
        for (u64 y = 0; y < (u64)th * ld; y++) memcpy(out + y * rowb, src + y * t->pitch, rowb);
        return;
    }
    u64 bsz = 512ull << (lbh + lbd);
    u32 rb = bl_row_bytes(rowb / t->f.bpp, th, ld, &t->f, t->bh, t->bd, t->tws); /* the guest's row, rowb packed */
    for (u32 z = 0; z < ld; z++)
        for (u32 y = 0; y < th; y++) {
            u8* dr = out + ((u64)z * th + y) * rowb;
            u64 o0 = bl_offset(0, y, z, rb, th, lbh, lbd);
            u32 x = 0;
            for (; x + 16 <= rowb; x += 16) {
                u64 o = o0 + (u64)(x >> 6) * bsz + ((x & 63) >> 5) * 256 + ((x & 31) >> 4) * 32;
                if (o + 16 <= lsz)
                    memcpy(dr + x, src + o, 16);
                else
                    memset(dr + x, 0, 16);
            }
            if (x < rowb) {
                u64 o = o0 + (u64)(x >> 6) * bsz + ((x & 63) >> 5) * 256 + ((x & 31) >> 4) * 32;
                if (o + (rowb - x) <= lsz)
                    memcpy(dr + x, src + o, rowb - x);
                else
                    memset(dr + x, 0, rowb - x);
            }
        }
}

static u32 g_tex_upload_gen;
static struct { /* first ASTC uploads since the last report (vk_tex_frame) */
    u64 n, ns, px, worst_ns, early, early_px, unknown;
} g_ahead;
static u32 g_tex_inval_gen;
void vk_tex_invalidate_all(void) { g_tex_inval_gen++; }
/* Entering an area gives a frame 150-230 Mpx of new ASTC textures to decode, about 100 ms of GPU the game's thread
   waits for. A big one with a mip chain is decoded from its third level down at first (1/16 of the work) and drawn from
   there through views that start at that level; the two levels above wait in a queue, decoded at the next presents up
   to SNX_ASTC_SPREAD_MPX texels a present (0 decodes everything at once) */
#define SPREAD_SKIP   2u
#define SPREAD_QUEUE  512
#define SPREAD_MIN_PX (512u * 512u)
static Surf* g_spread[SPREAD_QUEUE];
static int g_nspread;
static u64 g_spread_px = ~0ull; /* the budget a present, texels; ~0 until read */
static struct {
    u64 n, px, done, done_px, frames;
} g_spread_st;                                /* since the last report */
static void tex_set_base(Surf* t, u32 base) { /* views from level base: the old ones go, made again when asked */
    if (t->astc_base == base) return;
    t->astc_base = (u8)base;
    for (int i = 0; i < t->nsv; i++) vk_defer_image(VK_NULL_HANDLE, VK_NULL_HANDLE, t->sv[i].v, VK_NULL_HANDLE);
    t->nsv = 0;
}
void vk_tex_forget(Surf* s) {
    for (int i = 0; i < g_nspread; i++)
        if (g_spread[i] == s) {
            memmove(g_spread + i, g_spread + i + 1, (size_t)(g_nspread - i - 1) * sizeof *g_spread);
            g_nspread--;
            return;
        }
}
static u64 level_px(const Surf* t, u32 l0, u32 l1) { /* texels of levels l0 to l1 - 1, every layer */
    u64 px = 0;
    for (u32 l = l0; l < l1; l++) {
        u64 w = t->w >> l, h = t->h >> l;
        px += (w ? w : 1) * (h ? h : 1);
    }
    return px * t->layers;
}
/* the level a first upload of t starts at: SPREAD_SKIP for a big ASTC texture with levels to spare, else 0 */
static u32 spread_first(const Surf* t) {
    if (g_spread_px == ~0ull) {
        const char* e = getenv("SNX_ASTC_SPREAD_MPX");
        g_spread_px = (u64)(e ? atoi(e) : 16) * 1000000;
    }
    if (!g_spread_px || t->f.conv != VKC_ASTC || t->uploaded || t->levels <= SPREAD_SKIP ||
        t->type == VK_IMAGE_VIEW_TYPE_3D || (u64)t->w * t->h < SPREAD_MIN_PX || g_nspread == SPREAD_QUEUE)
        return 0;
    return SPREAD_SKIP;
}
/* levels l0 to l1 - 1 copied (the others are left as they are). 1 when every one was; 0 when memory or staging ran out,
   and upload_valid keeps it for a retry */
u64 g_vk_tex_budget, g_vk_tex_deferred;
int g_vk_tex_later;
static int tex_upload_levels(Surf* t, u32 l0, u32 l1) {
    const VkFmt* f = &t->f;
    if (g_vk_tex_budget && f->conv == VKC_ASTC && !t->uploaded) {
        /* a whole level's worth of first-time textures decoded at once stalled the frames after it for 200-360 ms */
        static u64 spent;
        static u32 frame;
        if (frame != g_vk_present) {
            frame = g_vk_present;
            spent = 0;
        }
        if (spent >= g_vk_tex_budget) {
            t->upload_valid = 0;
            g_vk_tex_later = 1;
            g_vk_tex_deferred++;
            return 0;
        }
        spent += (u64)t->w * t->h * t->layers * (t->depth3d ? t->depth3d : 1) * (t->levels > 1 ? 4 : 3) / 3;
    }
    t->upload_valid = 0;
    t->wgen = ++g_tex_upload_gen; /* even a new texture at a reused address invalidates an old target's copy */
    vk_render_end();              /* copies and clears happen outside rendering passes */
    VkCommandBuffer cb = vk_cmd();
    if (f->conv != VKC_ASTC) /* ASTC's: in vk_astc_flush, after the decodes, so no barrier comes between them */
        vk_surf_use(cb, t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                    VK_ACCESS_2_TRANSFER_WRITE_BIT);
    int packed_depth = f->depth && f->stencil;
    int is3d = t->type == VK_IMAGE_VIEW_TYPE_3D;
    u32 layers = t->layers, d3 = is3d ? t->depth3d : 1;
    VkBufferImageCopy* reg =
        f->conv == VKC_ASTC ? NULL : calloc((size_t)layers * t->levels * (packed_depth ? 2 : 1), sizeof *reg);
    if (f->conv != VKC_ASTC && !reg) return 0;
    u32 nreg = 0;
    int complete = 1;
    VkBuffer buf = VK_NULL_HANDLE, regbuf = VK_NULL_HANDLE;
    u64 off = 0;
    for (u32 layer = 0; layer < layers; layer++) {
        u64 at = layer * t->layer_size;
        for (u32 l = 0; l < t->levels; l++) {
            u32 lw = t->w >> l, lh = t->h >> l, ld = d3 >> l;
            if (!lw) lw = 1;
            if (!lh) lh = 1;
            if (!ld) ld = 1;
            u32 tw = (lw + f->bw - 1) / f->bw, th = (lh + f->bh - 1) / f->bh;
            u32 rowb = tw * f->bpp, lbh = 0, lbd = 0;
            u64 lsz =
                t->bl ? bl_level_size(t->w, t->h, d3, f, t->bh, t->bd, t->tws, l, &lbh, &lbd) : (u64)t->pitch * th * ld;
            if (l < l0 || l >= l1) {
                at += lsz;
                continue;
            }
            u8* src = gmem_ptr(t->gva + at, lsz);
            u8* tmp = NULL;
            /* a format the GPU takes as it is goes from guest memory straight into the staging buffer; the others
               are converted from packed rows */
            int direct = f->conv == VKC_NONE && !packed_depth;
            u64 lin_size = (u64)rowb * th * ld;
            u8* lin = direct ? NULL : malloc(lin_size ? lin_size : 1);
            if (!src) tmp = src = malloc(lsz ? lsz : 1);
            if (!src || (!direct && !lin)) { /* up to 512 MB: a level that doesn't fit is left as it was */
                LOG_LIMIT(1, "vulkan: out of memory uploading a %ux%u texture level (left out)\n", lw, lh);
                free(tmp);
                free(lin);
                complete = 0;
                goto done;
            }
            if (tmp) gmem_read(t->gva + at, tmp, lsz);
            if (!direct) level_rows(t, src, lsz, rowb, th, ld, lbh, lbd, lin);
            if (f->conv == VKC_ASTC) {
                free(tmp);
                vk_astc_upload(cb, t, lin, lw, lh, ld, l, layer);
                free(lin);
                at += lsz;
                continue;
            }
            u64 pixels = (u64)lw * lh * ld;
            u64 osz = packed_depth ? pixels * 5 : level_out_size(f, lw, lh, ld, rowb);
            u8* p = vk_stage(osz, 16, &buf, &off);
            if (p) {
                /* the staging ring grew into a new buffer: copy what went to the old one first */
                if (nreg && buf != regbuf) {
                    vkCmdCopyBufferToImage(cb, regbuf, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, nreg, reg);
                    nreg = 0;
                }
                regbuf = buf;
                if (direct)
                    level_rows(t, src, lsz, rowb, th, ld, lbh, lbd, p);
                else if (packed_depth)
                    vk_depth_unpack(f, lin, pixels, p, p + pixels * 4);
                else
                    level_convert(f, lin, lw, lh, ld, rowb, p);
                reg[nreg++] = (VkBufferImageCopy){
                    .bufferOffset = off,
                    .imageSubresource = {f->depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, l, layer, 1},
                    .imageExtent = {lw, lh, ld}};
                if (packed_depth) {
                    reg[nreg] = reg[nreg - 1];
                    reg[nreg].bufferOffset += pixels * 4;
                    reg[nreg++].imageSubresource.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
                }
            } else
                complete = 0;
            free(tmp);
            free(lin);
            if (!complete) goto done;
            at += lsz;
        }
    }
done:
    if (nreg) vkCmdCopyBufferToImage(cb, regbuf, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, nreg, reg);
    free(reg);
    if (complete) t->uploaded = 1;
    return t->upload_valid = complete;
}
/* SNX_TEX_HASHLOG=file: each ASTC texture's first upload as a BC7 pack would key it (a hash of its bytes, as Witch Time's
   tex_pack_hash; their size: one layer's levels for a single layer, else every layer at its stride), with its size and
   footprint, to check a pack made from the game's archives finds the textures the renderer uploads */
static void tex_hashlog(const Surf* t) {
    static FILE* f;
    static int tried;
    u32 d3 = t->type == VK_IMAGE_VIEW_TYPE_3D ? t->depth3d : 1;
    u64 one = 0, size, h, w;
    u8* p;
    u8* tmp = NULL;
    if (!tried) {
        const char* e = getenv("SNX_TEX_HASHLOG");
        tried = 1;
        if (e && *e) f = fopen(e, "w");
    }
    if (!f || t->f.conv != VKC_ASTC || t->uploaded || !t->bl) return;
    for (u32 l = 0; l < t->levels; l++)
        one += bl_level_size(t->w, t->h, d3, &t->f, t->bh, t->bd, t->tws, l, NULL, NULL);
    size = t->layers == 1 ? one : t->layer_size * t->layers;
    p = gmem_ptr(t->gva, size);
    if (!p && (tmp = p = malloc(size ? size : 1)) != NULL) gmem_read(t->gva, tmp, size);
    if (!p) return;
    h = 0x9E3779B97F4A7C15ull ^ size;
    for (u64 i = 0; i + 8 <= size; i += 8) {
        memcpy(&w, p + i, 8);
        h = (h ^ w) * 0x100000001B3ull;
        h ^= h >> 29;
    }
    free(tmp);
    fprintf(f, "%016llx\t%llu\t%u\t%u\t%u\t%u\t%ux%u\n", (unsigned long long)h, (unsigned long long)size, t->w, t->h,
            t->levels, t->layers, t->f.bw, t->f.bh);
    fflush(f);
}

/* ---- the texture pack (gpu/texture/texpack.h): the game's ASTC textures made BC7 ahead by texpack.exe, from the
   game's own archives. A new ASTC texture the pack holds is made a BC7 image and filled from the pack; one it doesn't
   goes the ASTC way. When the game rewrites a pack texture's memory, its new bytes are looked up again and, when the pack
   lacks them (a mod, a texture the game makes), encoded here: the image stays BC7. SNX_TEXPACK=dir reads the pack there
   (shader_cache/texpack by default), SNX_TEXPACK=0 not at all */
static int g_pack = -1; /* textures in the pack; -1 before it is opened */
static PMutex g_pack_lk = PMUTEX_INIT;
static struct {
    u64 hits, misses, encoded, bytes, ns;
} g_pack_st;
int vk_texpack_ready(void) {
    if (__atomic_load_n(&g_pack, __ATOMIC_ACQUIRE) < 0) {
        pm_lock(&g_pack_lk); /* the game's thread (registrations) and the render thread both ask first */
        if (g_pack < 0) {
            const char* e = getenv("SNX_TEXPACK");
            char dir[1100];
            int n = 0;
            if (!e || strcmp(e, "0")) {
                if (e && *e)
                    snprintf(dir, sizeof dir, "%s", e);
                else
                    snprintf(dir, sizeof dir, "%sshader_cache/texpack", g_cachedir);
                n = (int)texpack_open(dir);
                LOG("vulkan: texture pack %s: %d textures%s\n", dir, n, n ? "" : " (none, ASTC is decoded as before)");
            }
            __atomic_store_n(&g_pack, n, __ATOMIC_RELEASE);
        }
        pm_unlock(&g_pack_lk);
    }
    return g_pack > 0;
}
/* the format number the game's texture records give an ASTC format, which the pack is keyed by */
static u32 astc_format(const VkFmt* f) {
    static const u8 k[14][2] = {{4, 4}, {5, 4},  {5, 5},  {6, 5},  {6, 6},   {8, 5},   {8, 6},
                                {8, 8}, {10, 5}, {10, 6}, {10, 8}, {10, 10}, {12, 10}, {12, 12}};
    int srgb = f->vk == VK_FORMAT_BC3_SRGB_BLOCK || f->vk == VK_FORMAT_R8G8B8A8_SRGB, i;
    for (i = 0; i < 14; i++)
        if (k[i][0] == f->bw && k[i][1] == f->bh) return 0x79 + (u32)i + (srgb ? 14 : 0);
    return 0;
}
static VkFmt bc7_of(const VkFmt* astc) {
    VkFmt r = {astc_format(astc) >= 0x87 ? VK_FORMAT_BC7_SRGB_BLOCK : VK_FORMAT_BC7_UNORM_BLOCK, 16, 4, 4, 0, 0, 0, 0};
    r.conv = VKC_NONE;
    return r;
}
/* the bytes the pack keys a texture by: one layer's levels for a single layer, else every layer at its stride */
static u64 pack_size(u32 w, u32 h, u32 levels, u32 layers, const VkFmt* f, u32 bh, u32 bd, u32 tws, u64 stride) {
    u64 one = 0;
    for (u32 l = 0; l < levels; l++) one += bl_level_size(w, h, 1, f, bh, bd, tws, l, NULL, NULL);
    return layers == 1 ? one : stride * layers;
}
/* the pack's BC7 of the ASTC texture of size bytes at gva (malloc'd, *raw bytes), or NULL */
static u8* pack_lookup(u64 gva, u64 size, u32 w, u32 h, u32 levels, u32 layers, const VkFmt* f, u32* raw) {
    u64 t0 = plat_ns(), hash;
    u8* p = gmem_ptr(gva, size);
    u8* tmp = NULL;
    u8* out;
    const TexPackEntry* e;
    if (!p) {
        if (!(tmp = p = malloc(size ? size : 1))) return NULL;
        gmem_read(gva, tmp, size);
    }
    hash = texpack_hash(p, size);
    free(tmp);
    e = texpack_find(hash, size, astc_format(f), w, h, levels, layers);
    if (!e || !(out = malloc(e->raw ? e->raw : 1))) {
        g_pack_st.misses++;
        return NULL;
    }
    if (!vk_texahead_copy(e, out)) { /* read ahead by texahead.c, or read now */
        WARN_LIMIT(4, "vulkan: the texture pack's entry for a %ux%u texture couldn't be read\n", w, h);
        free(out);
        g_pack_st.misses++;
        return NULL;
    }
    *raw = e->raw;
    g_pack_st.hits++;
    g_pack_st.bytes += e->stored;
    g_pack_st.ns += plat_ns() - t0;
    return out;
}
/* BC7 blocks of every level of every layer into t, as the pack lays them out: 1 when copied */
static int pack_upload(Surf* t, const u8* data, u32 raw) {
    VkBufferImageCopy reg[16 * 6];
    VkBuffer buf = VK_NULL_HANDLE;
    u64 off = 0, o = 0;
    u32 n = 0, layer, l;
    VkCommandBuffer cb;
    u8* p;
    t->upload_valid = 0;
    if ((u64)t->layers * t->levels > sizeof reg / sizeof reg[0]) return 0;
    for (layer = 0; layer < t->layers; layer++)
        for (l = 0; l < t->levels; l++) {
            u32 lw = t->w >> l ? t->w >> l : 1, lh = t->h >> l ? t->h >> l : 1;
            reg[n++] = (VkBufferImageCopy){.bufferOffset = o,
                                           .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, layer, 1},
                                           .imageExtent = {lw, lh, 1}};
            o += (u64)((lw + 3) / 4) * ((lh + 3) / 4) * 16;
        }
    if (o != raw) {
        WARN_LIMIT(4, "vulkan: a %ux%u texture's pack entry has %u bytes, its levels %llu: left to the ASTC decoder\n",
                   t->w, t->h, raw, (unsigned long long)o);
        return 0;
    }
    t->wgen = ++g_tex_upload_gen;
    vk_render_end();
    cb = vk_cmd();
    if (!(p = vk_stage(raw, 16, &buf, &off))) return 0; /* staging ran out: tried again, as an upload is */
    memcpy(p, data, raw);
    for (l = 0; l < n; l++) reg[l].bufferOffset += off;
    vk_surf_use(cb, t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT);
    vkCmdCopyBufferToImage(cb, buf, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, n, reg);
    t->uploaded = 1;
    return t->upload_valid = 1;
}
/* a pack texture whose memory the game rewrote, encoded here from its new ASTC blocks (malloc'd), or NULL */
static u8* pack_encode(const Surf* t, u32* raw) {
    Surf as = *t; /* its levels read as the ASTC texture they are */
    u64 total = 0, o = 0;
    u8 *out, *lin = NULL, *rgba = NULL, *tmp = NULL;
    as.f = t->src;
    for (u32 l = 0; l < t->levels; l++) {
        u32 lw = t->w >> l ? t->w >> l : 1, lh = t->h >> l ? t->h >> l : 1;
        total += (u64)((lw + 3) / 4) * ((lh + 3) / 4) * 16;
    }
    total *= t->layers;
    if (!(out = malloc(total ? total : 1))) return NULL;
    for (u32 layer = 0; layer < t->layers; layer++) {
        u64 at = layer * t->layer_size;
        for (u32 l = 0; l < t->levels; l++) {
            u32 lw = t->w >> l ? t->w >> l : 1, lh = t->h >> l ? t->h >> l : 1, lbh = 0, lbd = 0;
            u32 tw = (lw + as.f.bw - 1) / as.f.bw, th = (lh + as.f.bh - 1) / as.f.bh;
            u64 lsz = bl_level_size(t->w, t->h, 1, &as.f, t->bh, t->bd, t->tws, l, &lbh, &lbd);
            u8* src = gmem_ptr(t->gva + at, lsz);
            lin = realloc(lin, (size_t)tw * th * 16);
            rgba = realloc(rgba, (size_t)lw * lh * 4);
            if (!src) {
                tmp = realloc(tmp, lsz ? lsz : 1);
                if (tmp) gmem_read(t->gva + at, tmp, lsz);
                src = tmp;
            }
            if (!src || !lin || !rgba) {
                free(out);
                out = NULL;
                goto done;
            }
            level_rows(&as, src, lsz, tw * 16, th, 1, lbh, lbd, lin);
            astc_decode_image(lin, lw, lh, as.f.bw, as.f.bh, rgba, astc_format(&t->src) >= 0x87);
            bc7_encode_image(rgba, lw, lh, out + o);
            o += (u64)((lw + 3) / 4) * ((lh + 3) / 4) * 16;
            at += lsz;
        }
    }
    *raw = (u32)total;
    g_pack_st.encoded++;
done:
    free(lin);
    free(rgba);
    free(tmp);
    return out;
}
/* a pack texture's memory changed: its new bytes from the pack, or encoded here */
static int pack_reupload(Surf* t) {
    u32 raw = 0;
    u8* data =
        pack_lookup(t->gva, pack_size(t->w, t->h, t->levels, t->layers, &t->src, t->bh, t->bd, t->tws, t->layer_size),
                    t->w, t->h, t->levels, t->layers, &t->src, &raw);
    int ok;
    if (!data) data = pack_encode(t, &raw);
    if (!data) return t->upload_valid = 0;
    ok = pack_upload(t, data, raw);
    free(data);
    return ok;
}

/* the whole texture, or for a big new ASTC one its lower levels with the rest queued (astc_spread) */
static int tex_upload(Surf* t) {
    u32 first;
    if (t->packed) return pack_reupload(t);
    first = spread_first(t);
    tex_hashlog(t);
    int ok = tex_upload_levels(t, first, t->levels);
    if (ok && first) {
        tex_set_base(t, first);
        g_spread[g_nspread++] = t;
        g_spread_st.n++;
        g_spread_st.px += level_px(t, 0, first);
    } else if (ok && t->astc_base) { /* a full upload over a queued one (its memory changed): whole now */
        vk_tex_forget(t);
        tex_set_base(t, 0);
    }
    return ok;
}
/* each present: the queued textures' top levels, oldest first, up to the budget. A texture over the budget on its own
   still goes when it is the first, so the queue always moves */
static void astc_spread(void) {
    u64 px = 0;
    if (g_nspread) g_spread_st.frames++;
    while (g_nspread && px < g_spread_px) {
        Surf* t = g_spread[0];
        u32 base = t->astc_base;
        u64 n = level_px(t, 0, base);
        if (px && px + n > g_spread_px) break;
        if (!tex_upload_levels(t, 0, base)) break; /* staging full: tried again at the next present */
        vk_tex_forget(t);
        tex_set_base(t, 0);
        px += n;
        g_spread_st.done++;
        g_spread_st.done_px += n;
    }
}

/* change checks: a hash of the whole texture when it is contiguous in host memory, else of 64 bytes of each page
   (the sampled check for big long-stable textures is vk_sample_hash) */
static u64 mem_hash(u64 gva, u64 size) {
    u8* p = gmem_ptr(gva, size);
    if (p) return vk_hash(p, size);
    u64 h = 1469598103934665603ull;
    for (u64 o = 0; o < size; o += 4096) {
        u64 v[8] = {0};
        gmem_read(gva + o, v, size - o < 64 ? size - o : 64);
        for (int i = 0; i < 8; i++) h = (h ^ v[i]) * 1099511628211ull;
    }
    return h;
}
/* the texture's memory hashed again, and loaded again when that changed: 1 if it did */
static int tex_rehash(Surf* t) {
    t->gw = gpu_mem_generation();
    u64 hh = mem_hash(t->gva, t->size);
    if (hh == t->hash && t->upload_valid) return 0;
    if (tex_upload(t)) t->hash = hh; /* an incomplete upload keeps the old hash, so the next check retries it */
    return 1;
}

void vk_tex_note_writeback(Surf* t) {
    t->hash = mem_hash(t->gva, t->size);
    t->gw = gpu_mem_generation();
    t->wseen = vk_ww_now();
    t->checked = g_vk_present;
    t->stable = 0;
}

static int format_samples(VkFormat f) {
    static VkFormat ok[64], bad[64];
    static int nok, nbad;
    for (int i = 0; i < nok; i++)
        if (ok[i] == f) return 1;
    for (int i = 0; i < nbad; i++)
        if (bad[i] == f) return 0;
    VkFormatProperties p;
    vkGetPhysicalDeviceFormatProperties(g_vk.phys, f, &p);
    int r = (p.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0;
    if (r && nok < 64) ok[nok++] = f;
    if (!r && nbad < 64) {
        bad[nbad++] = f;
        WARN("vulkan: the GPU can't sample format %d (textures in it aren't drawn)\n", (int)f);
    }
    return r;
}

static Surf* tex_create(u64 gva, u32 w, u32 h, u32 layers, u32 d3, u32 levels, VkImageViewType type, VkFmt f, u32 fkey,
                        u32 bl, u32 bh, u32 bd, u32 pitch, u64 layer_size) {
    Surf* t = calloc(1, sizeof *t);
    if (!t) return NULL;
    t->gva = gva;
    t->w = t->sw = w;
    t->h = t->sh = h;
    t->layers = layers;
    t->depth3d = d3;
    t->levels = levels;
    t->type = type;
    t->f = f;
    t->fkey = fkey;
    t->bl = bl;
    t->bh = bh;
    t->bd = bd;
    t->pitch = pitch;
    t->layer_size = layer_size;
    t->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                            .imageType = type == VK_IMAGE_VIEW_TYPE_3D ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D,
                            .format = f.vk,
                            .extent = {w, h, type == VK_IMAGE_VIEW_TYPE_3D ? d3 : 1},
                            .mipLevels = levels,
                            .arrayLayers = layers,
                            .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                            .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkFormatProperties format_props;
    vkGetPhysicalDeviceFormatProperties(g_vk.phys, f.vk, &format_props);
    if (format_props.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) {
        ii.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
        t->storage_capable = 1;
    }
    if (type != VK_IMAGE_VIEW_TYPE_3D && layers >= 6 && w == h) ii.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    VkViewFormats vf;
    vk_view_formats(&ii, &vf);
    VmaAllocationCreateInfo aci = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (!VK_OK(vmaCreateImage(g_vk.vma, &ii, &aci, &t->img, &t->mem, NULL))) {
        free(t);
        return NULL;
    }
    vk_mem_tag(t->mem, f.conv == VKC_ASTC ? VKM_ASTC : f.bw > 1 ? VKM_BC : VKM_TEX);
    t->last_used = g_vk_present;
    vk_surf_add(t);
    return t;
}

/* a render target read through a smaller header or a feedback draw: a copy refreshed when the target is drawn to. One
   copy per size, in a list through next: one draw can hold copies of a target at two sizes (two headers, or two
   stages), so a copy isn't replaced while a draw may hold it. Copies not read for a second go; their views and image
   when the frame being recorded is done, as descriptors may still name them */
/* the target's top-left w x h copied out, in the target's format, or in as: another color format of the same texel
   size, the target's bytes read as that format (a bit copy, which vkCmdCopyImage makes between such formats) */
static Surf* rt_crop(Surf* rt, u32 w, u32 h, const VkFmt* as) {
    Surf* c = NULL;
    VkFmt f = as ? *as : rt->f;
    for (Surf** pp = &rt->crop; *pp;) {
        Surf* x = *pp;
        if (x->w == w && x->h == h && x->f.vk == f.vk)
            c = x;
        else if ((i32)(g_vk_present - (u32)x->last_used) > 60) {
            *pp = x->next;
            for (int i = 0; i < x->nsv; i++) vk_defer_image(VK_NULL_HANDLE, VK_NULL_HANDLE, x->sv[i].v, VK_NULL_HANDLE);
            vk_defer_image(x->img, x->mem, VK_NULL_HANDLE, VK_NULL_HANDLE);
            free(x);
            continue;
        }
        pp = &x->next;
    }
    if (!c) {
        c = calloc(1, sizeof *c);
        if (!c) return rt;
        c->gva = rt->gva;
        c->w = w;
        c->h = h;
        c->sw = vk_sc(w) < rt->sw ? vk_sc(w) : rt->sw; /* the same region of the scaled target */
        c->sh = vk_sc(h) < rt->sh ? vk_sc(h) : rt->sh;
        c->layers = rt->layers;
        c->levels = c->depth3d = 1;
        c->f = f;
        c->fkey = rt->fkey;
        c->type = rt->type;
        c->is_rt = 1; /* sampled like one; not in the table, so never found as one */
        c->layout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                .imageType = VK_IMAGE_TYPE_2D,
                                .format = f.vk,
                                .extent = {c->sw, c->sh, 1},
                                .mipLevels = 1,
                                .arrayLayers = rt->layers,
                                .samples = VK_SAMPLE_COUNT_1_BIT,
                                .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
        VkViewFormats vf;
        vk_view_formats(&ii, &vf);
        VmaAllocationCreateInfo aci = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        if (!VK_OK(vmaCreateImage(g_vk.vma, &ii, &aci, &c->img, &c->mem, NULL))) {
            free(c);
            return rt;
        }
        vk_mem_tag(c->mem, VKM_SURF);
        c->crop_gen = ~0u;
        c->next = rt->crop;
        rt->crop = c;
        if (w != rt->w || h != rt->h)
            LOG_LIMIT(10, "vulkan: %ux%u texture reads the %ux%u render target at %llx: only that region is used\n", w,
                      h, rt->w, rt->h, (unsigned long long)rt->gva);
    }
    c->last_used = g_vk_present;
    if (c->crop_gen != rt->wgen) {
        VkCommandBuffer cb = vk_cmd();
        VkImageCopy r[2];
        vk_surf_use(cb, rt, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                    VK_ACCESS_2_TRANSFER_READ_BIT);
        vk_surf_use(cb, c, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                    VK_ACCESS_2_TRANSFER_WRITE_BIT);
        /* VkImageCopy takes one aspect per region: depth and stencil are two (a view samples one of them) */
        VkImageAspectFlags a = rt->f.depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
        memset(r, 0, sizeof r);
        r[0].srcSubresource.aspectMask = a;
        r[0].srcSubresource.layerCount = rt->layers;
        r[0].dstSubresource.aspectMask = a;
        r[0].dstSubresource.layerCount = rt->layers;
        r[0].extent.width = c->sw;
        r[0].extent.height = c->sh;
        r[0].extent.depth = 1;
        r[1] = r[0];
        r[1].srcSubresource.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
        r[1].dstSubresource.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
        vkCmdCopyImage(cb, rt->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, c->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       rt->f.stencil ? 2 : 1, r);
        c->crop_gen = rt->wgen;
    }
    return c;
}

Surf* vk_rt_snapshot(Surf* rt) {
    if (!rt || rt->layout == VK_IMAGE_LAYOUT_UNDEFINED) return rt;
    return rt_crop(rt, rt->w, rt->h, NULL);
}

/* texture header type -> what the image is created as */
static VkImageViewType tic_view_type(u32 type, u32 d) {
    switch (type) {
        case 2: return VK_IMAGE_VIEW_TYPE_3D;
        case 3: return VK_IMAGE_VIEW_TYPE_CUBE;
        case 8: return VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
        case 4:
        case 5: return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        case 1:
        case 7: return d > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    }
    return VK_IMAGE_VIEW_TYPE_2D; /* 1D textures are 2D images one texel high */
}

static u32 g_bud_frame;
static i64 g_bud;
static Surf* tex_from_tic(const u32* tic, int raw) {
    u64 gva = ((u64)(tic[2] & 0xFFFF) << 32) | tic[1];
    u32 hv = (tic[2] >> 21) & 7;
    int bl = hv == 3 || hv == 4;
    u32 w = (tic[4] & 0xFFFF) + 1, h = (tic[5] & 0xFFFF) + 1, d = ((tic[5] >> 16) & 0x3FFF) + 1;
    u32 type = (tic[4] >> 23) & 0xF;
    int srgb = (tic[4] >> 22) & 1;
    u32 glevels = ((tic[3] >> 28) & 0xF) + 1;
    u32 bh = (tic[3] >> 3) & 7, bd = (tic[3] >> 6) & 7, pitch = bl ? 0 : ((tic[3] & 0xFFFF) << 5);
    u32 tws = bl ? (tic[3] >> 10) & 7 : 0; /* tile width spacing (bl_row_bytes) */
    if (!gva) return NULL;
    if (hv == 0 || type == 6) {
        /* Buffer TICs describe a linear row of texels. The guest's 1D fetch is represented by our
           height-one 2D image and the texture shader already supplies y = 0. */
        if (hv != 0 || type != 6) return NULL;
        w = (((tic[3] & 0xFFFFu) << 16) | (tic[4] & 0xFFFFu)) + 1;
        h = d = 1;
        glevels = 1;
        bl = 0;
        bh = bd = 0;
    }
    if ((tic[7] >> 8) & 0xF) LOG_LIMIT(1, "vulkan: multisampled textures are read as single-sampled\n");
    if (type == 0 || type == 4) h = 1;
    if (type == 3)
        d = 6;
    else if (type == 8)
        d *= 6;
    VkFmt f = vk_tic_format(tic[0], srgb);
    if (raw) {
        if (f.vk != VK_FORMAT_R8G8B8A8_UNORM && f.vk != VK_FORMAT_R8G8B8A8_SRGB) return NULL;
        f.vk = VK_FORMAT_R32_UINT;
        f.is_int = 1;
        f.sint = 0;
    }
    if (type == 6 && f.bpp && w <= UINT32_MAX / f.bpp) pitch = w * f.bpp;
    if (type == 6 && w > 16384) { /* the row of a 2D image: drawing has no texel buffer binding for it */
        LOG_LIMIT(1, "vulkan: a buffer texture of %u texels is longer than a texture row here (read as zeros)\n", w);
        return NULL;
    }
    if (!f.bpp || !w || w > 16384 || h > 16384 || d > 2048) return NULL;
    if ((u64)w * h * d * (f.bw > 1 ? 1 : f.bpp) > (512ull << 20)) return NULL;
    if (!bl && pitch < (w + f.bw - 1) / f.bw * f.bpp) {
        LOG_LIMIT(1, "vulkan: a pitch-linear texture's pitch (%u bytes) is shorter than its rows (not drawn)\n", pitch);
        return NULL;
    }
    VkImageViewType vt = tic_view_type(type, d);
    if (vt == VK_IMAGE_VIEW_TYPE_CUBE && d != 6) vt = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    if (vt == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY && !g_vk.feat.imageCubeArray) vt = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    u32 layers = vt == VK_IMAGE_VIEW_TYPE_3D ? 1 : d, d3 = vt == VK_IMAGE_VIEW_TYPE_3D ? d : 1;
    /* bytes from one layer to the next: all the levels the header has, whichever of them it shows */
    u64 stride = bl ? bl_layer_size(w, h, d3, &f, bh, bd, tws, glevels) : (u64)pitch * ((h + f.bh - 1) / f.bh) * d3;
    /* A texture upload reads guest memory. Complete any compute storage writes that overlap it first. */
    if (g_cs_any) vk_compute_mem_need(gva, stride * layers);
    /* the levels the shader sees, base level to max level (word 7), as a texture of their own the way Ryujinx makes
       it: the base level's address, size and blocks. Pitch-linear textures have one level */
    u32 levels = bl ? glevels : 1;
    u32 base = tic[7] & 0xF, maxl = (tic[7] >> 4) & 0xF;
    if (bl && base < glevels && (base || maxl + 1 != glevels)) {
        for (u32 l = 0; l < base; l++) gva += bl_level_size(w, h, d3, &f, bh, bd, tws, l, NULL, NULL);
        bl_level_blocks(h, d3, &f, bh, bd, base, &bh, &bd);
        w = w >> base ? w >> base : 1;
        h = h >> base ? h >> base : 1;
        d3 = d3 >> base ? d3 >> base : 1;
        levels = maxl >= base ? maxl - base + 1 : 1;
        if (levels > glevels - base) levels = glevels - base;
    }
    /* no more levels than the size has (Vulkan requires it) */
    u32 maxdim = w > h ? w : h, maxlev = 1;
    if (d3 > maxdim) maxdim = d3;
    while (maxdim >> maxlev) maxlev++;
    if (levels > maxlev) levels = maxlev;
    /* a render target at that address: read it on the GPU, unless its contents are undefined (never drawn, or memory
       it can't load was written over it): then memory has them */
    Surf* rt = type == 6 ? NULL : vk_rt_find(gva);
    /* a texture of several levels isn't the target (its other levels are other targets' memory, a mip chain drawn level
       by level): loaded from memory */
    if (rt && rt->layout != VK_IMAGE_LAYOUT_UNDEFINED && f.bw == 1 && (!f.conv || f.depth) && type != 2 && rt->w >= w &&
        rt->h >= h && (d == 1 || type == 1 || type == 7) && levels == 1) {
        /* a header of another texel size covers other memory than the target's pixels (depth is read through its
           own aspect, whatever the host format's size) */
        if (f.bpp == rt->f.bpp || f.depth || rt->f.depth) {
            vk_rt_note_read(rt);
            if (rt->layout == VK_IMAGE_LAYOUT_UNDEFINED) goto texture_from_memory;
            /* the same bytes in another color format (an RGBA8 target read as R32_Uint): sampling the target itself
               would convert its texels, so a copy in the texture's format holds them as they are. sRGB and linear
               twins are views of one image and need no copy */
            if (!f.depth && !rt->f.depth && vk_srgb_as(f.vk, 0) != vk_srgb_as(rt->f.vk, 0))
                return rt_crop(rt, w, h, &f);
            return rt->w == w && rt->h == h ? rt : rt_crop(rt, w, h, NULL);
        }
        LOG_LIMIT(1,
                  "vulkan: a texture with %u-byte texels reads the render target at %llx, which has %u-byte pixels: it "
                  "is loaded from memory\n",
                  f.bpp, (unsigned long long)gva, rt->f.bpp);
    }
texture_from_memory:
    /* loaded from memory, with what render targets drew into its layers and levels copied in after (tex_take_drawn) */
    /* what the device can make */
    const VkPhysicalDeviceLimits* lim = &g_vk.props.limits;
    int cube = vt != VK_IMAGE_VIEW_TYPE_3D && layers >= 6 && w == h; /* created cube compatible */
    if (vt == VK_IMAGE_VIEW_TYPE_3D
            ? w > lim->maxImageDimension3D || h > lim->maxImageDimension3D || d3 > lim->maxImageDimension3D
            : w > lim->maxImageDimension2D || h > lim->maxImageDimension2D || layers > lim->maxImageArrayLayers ||
                  (cube && w > lim->maxImageDimensionCube)) {
        LOG_LIMIT(1, "vulkan: a %ux%ux%u texture is bigger than the GPU's images can be (not drawn)\n", w, h, d);
        return NULL;
    }
    u32 fkey = (tic[0] & 0x7FFFF) | ((u32)srgb << 31) | ((u32)raw << 30);
    for (Surf* t = vk_surf_first(gva); t; t = t->next) {
        if (t->is_rt || t->gva != gva || t->w != w || t->h != h || t->layers != layers || t->depth3d != d3 ||
            t->fkey != fkey || t->levels != levels || t->type != vt || t->bl != (u32)bl || t->bh != bh || t->bd != bd ||
            t->tws != tws || t->pitch != pitch || t->layer_size != stride)
            continue;
        t->last_used = g_vk_present;
        /* an upload that ran out of memory or staging (here, or a re-upload below) is tried again once a frame. A
           texture is only returned whole */
        if (!t->upload_valid) {
            if (t->checked != g_vk_present) {
                t->checked = g_vk_present;
                tex_rehash(t);
            }
            if (!t->upload_valid) {
                if (g_vk_tex_budget && !t->uploaded) g_vk_tex_later = 1;
                return NULL;
            }
        }
        /* the GPU front end wrote its memory (DMA, I2M, 2D, compute results): checked now, whatever the schedule
           below says, as writes within a frame have to show in the same frame. Not written: it still matches as of
           now, so the next check asks only about later writes */
        int fresh = 0;
        int invalidated = t->tex_inval_gen != g_tex_inval_gen;
        if (gpu_mem_dirty_since(t->gw, gva, t->size)) {
            fresh = 1;
            if (tex_rehash(t)) t->stable = 0;
        } else
            t->gw = gpu_mem_generation();
        /* Invalidation also checks CPU writes within this frame. It does not imply every texture changed:
           write tracking can prove its memory is unchanged without hashing all its texels. */
        if (invalidated || t->checked != g_vk_present) {
            u32 wnow = vk_ww_now();
            int wr = vk_written_since(gmem_ptr(gva, t->size), t->size, t->wseen);
            t->tex_inval_gen = g_tex_inval_gen;
            if (wr >= 0) {
                t->checked = g_vk_present;
                if (wr && !fresh) tex_rehash(t);
                t->wseen = wnow + 1; /* this scan's marks are stamped with the clock after wnow */
                return t->upload_valid ? t : NULL;
            }
            /* Without write tracking an explicit invalidation must bypass the normal backoff. */
            if (invalidated) {
                if (!fresh && tex_rehash(t)) t->stable = 0;
                t->checked = g_vk_present;
                return t->upload_valid ? t : NULL;
            }
        }
        /* without it: often at first, backing off while the texture stays the same, within a per-frame budget */
        if (t->checked != g_vk_present && (i32)(g_vk_present - t->next_check) >= 0) {
            if (g_bud_frame != g_vk_present) {
                g_bud_frame = g_vk_present;
                g_bud = 24 << 20;
            }
            if (t->stable >= 40 && t->size >= 16384 && (i32)(g_vk_present - t->tfull) < 120) {
                t->checked = g_vk_present;
                if (vk_sample_hash(gva, t->size) == t->shash) {
                    t->next_check = g_vk_present + 20 + ((u32)(gva >> 9) & 7);
                    return t->upload_valid ? t : NULL;
                }
                t->stable = 0;
            }
            if (g_bud > 0 || t->stable < 4) {
                t->checked = g_vk_present;
                g_bud -= (i64)(t->size >> (t->size > 65536 ? 1 : 0));
                if (tex_rehash(t))
                    t->stable = 0;
                else if (t->stable < 100000)
                    t->stable++;
                if (t->stable >= 39 && t->size >= 16384) {
                    t->shash = vk_sample_hash(gva, t->size);
                    t->tfull = g_vk_present;
                }
                t->next_check = g_vk_present + (t->stable < 4 ? 4 : t->stable < 12 ? 8 : 20) + ((u32)(gva >> 9) & 3);
            }
        }
        return t->upload_valid ? t : NULL;
    }
    /* an ASTC texture the pack holds becomes a BC7 image, filled from the pack below; its memory stays ASTC (src) */
    VkFmt src = f;
    u8* pdata = NULL;
    u32 praw = 0;
    if (f.conv == VKC_ASTC && bl && vt != VK_IMAGE_VIEW_TYPE_3D && (u64)layers * levels <= 96 && vk_texpack_ready()) {
        vk_prompts_texture(gva, &f, w, h, bh); /* painted first, so a button atlas is looked up as it will be */
        pdata =
            pack_lookup(gva, pack_size(w, h, levels, layers, &f, bh, bd, tws, stride), w, h, levels, layers, &f, &praw);
        if (pdata && format_samples(bc7_of(&f).vk)) f = bc7_of(&f);
    }
    if (!format_samples(f.vk)) {
        free(pdata);
        return NULL;
    }
    Surf* t = tex_create(gva, w, h, layers, d3, levels, vt, f, fkey, bl, bh, bd, pitch, stride);
    if (!t) {
        free(pdata);
        return NULL;
    }
    t->tws = tws;
    t->tex_inval_gen = g_tex_inval_gen;
    t->src = src;
    t->packed = f.vk != src.vk && pdata;
    /* this texture's levels in one layer; the layers before the last one take their whole stride */
    u64 sz = 0;
    for (u32 l = 0; l < levels; l++) sz += bl ? bl_level_size(w, h, d3, &src, bh, bd, tws, l, NULL, NULL) : stride;
    t->size = stride * (layers - 1) + sz;
    vk_prompts_texture(gva, &src, w, h, bh); /* the button atlas is painted before it is hashed and loaded */
    t->wseen = vk_ww_now();
    vk_written_since(gmem_ptr(gva, t->size), t->size, 0); /* starts tracking it */
    t->wseen = vk_ww_now();
    t->gw = gpu_mem_generation();
    t->hash = mem_hash(gva, t->size);
    t->checked = g_vk_present;
    t->next_check = g_vk_present + 4;
    if (t->packed) {
        int ok = pack_upload(t, pdata, praw);
        free(pdata);
        return ok ? t : NULL;
    }
    free(pdata);
    if (f.conv != VKC_ASTC) return tex_upload(t) ? t : NULL;
    { /* the ahead-of-draw measure: how long a first ASTC upload takes, and how long before it the game registered it */
        u64 t0 = plat_ns();
        int ok = tex_upload(t);
        long long known = vk_nvn_tex_known(gva);
        u64 px = (u64)w * h * layers * d3;
        g_ahead.n++;
        g_ahead.ns += plat_ns() - t0;
        g_ahead.px += px;
        if (known < 0)
            g_ahead.unknown++;
        else if ((long long)g_vk_present - known >= 30) {
            g_ahead.early++;
            g_ahead.early_px += px;
        }
        if (plat_ns() - t0 > g_ahead.worst_ns) g_ahead.worst_ns = plat_ns() - t0;
        return ok ? t : NULL;
    }
}

VkImageView vk_tex_view(Surf* s, VkImageViewType type, int srgb, const VkComponentMapping* map, int stencil) {
    /* what a view of each image type can be */
    u32 layers;
    switch (type) {
        case VK_IMAGE_VIEW_TYPE_2D:
            if (s->type == VK_IMAGE_VIEW_TYPE_3D) return VK_NULL_HANDLE;
            layers = 1;
            break;
        case VK_IMAGE_VIEW_TYPE_2D_ARRAY:
            if (s->type == VK_IMAGE_VIEW_TYPE_3D) return VK_NULL_HANDLE;
            layers = s->layers;
            break;
        case VK_IMAGE_VIEW_TYPE_CUBE:
            if (s->layers < 6 || s->w != s->h || s->type == VK_IMAGE_VIEW_TYPE_3D || s->is_rt) return VK_NULL_HANDLE;
            layers = 6;
            break;
        case VK_IMAGE_VIEW_TYPE_CUBE_ARRAY:
            if (s->layers < 6 || s->w != s->h || s->type == VK_IMAGE_VIEW_TYPE_3D || s->is_rt ||
                !g_vk.feat.imageCubeArray)
                return VK_NULL_HANDLE;
            layers = s->layers / 6 * 6;
            break;
        case VK_IMAGE_VIEW_TYPE_3D:
            if (s->type != VK_IMAGE_VIEW_TYPE_3D) return VK_NULL_HANDLE;
            layers = 1;
            break;
        default: return VK_NULL_HANDLE;
    }
    VkFormat fmt = s->f.depth ? s->f.vk : vk_srgb_as(s->f.vk, srgb);
    stencil = stencil && s->f.stencil;
    u64 key = (u64)type | (u64)map->r << 4 | (u64)map->g << 8 | (u64)map->b << 12 | (u64)map->a << 16 | (u64)fmt << 20 |
              (u64)stencil << 60;
    for (int i = 0; i < s->nsv; i++)
        if (s->sv[i].key == key) return s->sv[i].v;
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                .image = s->img,
                                .viewType = type,
                                .format = fmt,
                                .components = *map,
                                .subresourceRange = {stencil      ? VK_IMAGE_ASPECT_STENCIL_BIT
                                                     : s->f.depth ? VK_IMAGE_ASPECT_DEPTH_BIT
                                                                  : VK_IMAGE_ASPECT_COLOR_BIT,
                                                     s->astc_base, s->levels - s->astc_base, 0, layers}};
    /* a view in the image's sRGB twin is sampled only: storage, which a render target may have, has no sRGB format */
    VkImageViewUsageCreateInfo vu = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO,
                                     .usage = VK_IMAGE_USAGE_SAMPLED_BIT};
    if (fmt != s->f.vk) vi.pNext = &vu;
    VkImageView v;
    if (!VK_OK(vkCreateImageView(g_vk.dev, &vi, NULL, &v))) return VK_NULL_HANDLE;
    if (s->nsv == (int)(sizeof s->sv / sizeof s->sv[0])) { /* full: drop the oldest */
        vk_defer_image(VK_NULL_HANDLE, VK_NULL_HANDLE, s->sv[0].v, VK_NULL_HANDLE);
        memmove(s->sv, s->sv + 1, sizeof s->sv - sizeof s->sv[0]);
        s->nsv--;
    }
    s->sv[s->nsv++] = (SurfView){key, v};
    return v;
}

/* empty textures for slots whose texture is missing or doesn't match the shader: zero, or depth 1.0 for shadow
   samplers, which compare against depth formats only */
VkImageView vk_null_view(VkImageViewType type, int num) {
    static VkImageView views[8][4];
    static Surf* imgs[2][4]; /* [3D][num] */
    if ((unsigned)type >= 8 || num < 0 || num > 3) return VK_NULL_HANDLE;
    if (num == 3 && type == VK_IMAGE_VIEW_TYPE_3D) num = 0; /* no 3D depth images (nor 3D shadow samplers) */
    if (views[type][num]) return views[type][num];
    if (type == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY && !g_vk.feat.imageCubeArray) return VK_NULL_HANDLE;
    int is3d = type == VK_IMAGE_VIEW_TYPE_3D;
    Surf* s = imgs[is3d][num];
    if (!s) {
        static const VkFormat fm[4] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UINT, VK_FORMAT_R8G8B8A8_SINT,
                                       VK_FORMAT_D16_UNORM};
        s = calloc(1, sizeof *s);
        if (!s) return VK_NULL_HANDLE;
        s->w = s->h = s->sw = s->sh = s->levels = s->depth3d = 1;
        s->layers = is3d ? 1 : 6;
        s->type = is3d ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        s->f = num == 3 ? (VkFmt){.vk = fm[3], .bpp = 2, .bw = 1, .bh = 1, .depth = 1}
                        : (VkFmt){fm[num], 4, 1, 1, 0, 0, (u8)(num != 0), 0, (u8)(num == 2)};
        s->layout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                .flags = is3d ? 0 : VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT,
                                .imageType = is3d ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D,
                                .format = fm[num],
                                .extent = {1, 1, 1},
                                .mipLevels = 1,
                                .arrayLayers = s->layers,
                                .samples = VK_SAMPLE_COUNT_1_BIT,
                                .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
        VmaAllocationCreateInfo aci = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        if (!VK_OK(vmaCreateImage(g_vk.vma, &ii, &aci, &s->img, &s->mem, NULL))) {
            free(s);
            return VK_NULL_HANDLE;
        }
        vk_render_end(); /* clears happen outside rendering passes */
        VkCommandBuffer cb = vk_cmd();
        vk_surf_use(cb, s, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_CLEAR_BIT,
                    VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkImageSubresourceRange all = {vk_aspect(&s->f), 0, 1, 0, s->layers};
        if (num == 3) {
            VkClearDepthStencilValue far_depth = {1.0f, 0};
            vkCmdClearDepthStencilImage(cb, s->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &far_depth, 1, &all);
        } else {
            VkClearColorValue z = {{0, 0, 0, 0}};
            vkCmdClearColorImage(cb, s->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &z, 1, &all);
        }
        vk_surf_use(cb, s, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_SAMPLED_STAGES,
                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        imgs[is3d][num] = s;
    }
    VkComponentMapping id = {0};
    views[type][num] = vk_tex_view(s, type, 0, &id, 0);
    return views[type][num];
}

/* ------------------------------------------------------------ samplers */
static VkSamplerAddressMode wrap(u32 m) {
    switch (m) {
        case 0: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
        case 1: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        case 2:
        case 4: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        case 3:
        case 6: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        case 5:
        case 7:
            /* an optional feature: without it mirrored repeat, the same over the first mirror */
            return g_vk.has_mirror_clamp ? VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE
                                         : VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    }
    return VK_SAMPLER_ADDRESS_MODE_REPEAT;
}

/* tsc[0] bit 9 compare, bits 10-12 the compare function; tsc[1] filters (gpu/vk/render.c forces nearest for integer
   textures and the compare mode the shader declares) */
int g_vk_anisotropy;
VkSampler vk_sampler(const u32* tsc, int raise, int integer) {
    /* samplers are kept for good, and a device allows at least 4000 (maxSamplerAllocationCount): past 3072, a quarter
       of the table left free for probing, new sampler states get the first sampler made */
    enum { TAB = 4096, MAX = 3072 };
    static struct {
        u32 tsc[8];
        int raise, integer;
        VkSampler s;
    } tab[TAB];
    static u32 n, ncustom;
    static VkSampler first;
    u32 hk = 2166136261u;
    for (int i = 0; i < 8; i++) hk = (hk ^ tsc[i]) * 16777619u;
    hk = (hk ^ (u32)(raise << 1 | integer)) * 16777619u;
    u32 j = hk & (TAB - 1);
    for (u32 probe = 0; probe < TAB && tab[j].s; probe++, j = (j + 1) & (TAB - 1))
        if (!memcmp(tab[j].tsc, tsc, 32) && tab[j].raise == raise && tab[j].integer == integer) return tab[j].s;
    if (n >= MAX || tab[j].s) {
        LOG_LIMIT(1, "vulkan: more than %d sampler states: the others use the first one\n", MAX);
        return first;
    }
    u32 mag = tsc[1] & 3, min = (tsc[1] >> 4) & 3, mip = (tsc[1] >> 6) & 3;
    float minl = (float)(tsc[2] & 0xFFF) / 256.f, maxl = (float)((tsc[2] >> 12) & 0xFFF) / 256.f;
    i32 bias = (i32)((tsc[1] >> 12) & 0x1FFF);
    if (bias & 0x1000) bias |= ~0x1FFF;
    float lb = (float)bias / 256.f, lbmax = g_vk.props.limits.maxSamplerLodBias;
    if (lb > lbmax) lb = lbmax;
    if (lb < -lbmax) lb = -lbmax;
    static const float aniso_lut[8] = {1, 2, 4, 6, 8, 10, 12, 16}; /* Ryujinx's SamplerDescriptor */
    u32 aniso = (tsc[0] >> 20) & 7;
    float af = aniso_lut[aniso];
    /* anisotropy=N raises a mipmapped 2D texture's linear filtering (raise), a 3D scene's surfaces, to N. Nearest
       filtering (2D art, the HUD), samplers without a mip filter and depth compares (shadow maps) keep the game's */
    if (raise && (float)g_vk_anisotropy > af && min == 2 && mag == 2 && mip >= 2 && !((tsc[0] >> 9) & 1))
        af = (float)g_vk_anisotropy;
    if ((tsc[1] >> 10) & 3)
        LOG_LIMIT(1, "vulkan: min/max sampler reduction isn't supported (filtered as an average)\n");
    VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                              .magFilter = mag == 2 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
                              .minFilter = min == 2 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
                              .mipmapMode = mip == 3 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST,
                              .addressModeU = wrap(tsc[0] & 7),
                              .addressModeV = wrap((tsc[0] >> 3) & 7),
                              .addressModeW = wrap((tsc[0] >> 6) & 7),
                              .mipLodBias = lb,
                              .anisotropyEnable = af > 1 && g_vk.feat.samplerAnisotropy,
                              .maxAnisotropy = af,
                              .compareEnable = (tsc[0] >> 9) & 1,
                              .compareOp = (VkCompareOp)((tsc[0] >> 10) & 7),
                              .minLod = minl,
                              .maxLod = mip <= 1 ? minl + 0.25f : maxl, /* no mip filter: the base level only */
                              .borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK}; /* set below */
    if (si.maxAnisotropy > g_vk.props.limits.maxSamplerAnisotropy)
        si.maxAnisotropy = g_vk.props.limits.maxSamplerAnisotropy;
    if (si.maxLod < si.minLod) si.maxLod = si.minLod;
    /* the border color: the TSC's four words as the texture's texels read, floats for a float view and integers for an
       integer one (nouveau writes the integer ones as their bits), as Vulkan's FLOAT or INT colors. A custom color
       takes one of the device's few custom-color samplers while there are any left, else the nearest built-in one */
    float bc[4];
    i32 bi[4];
    memcpy(bc, &tsc[4], 16);
    memcpy(bi, &tsc[4], 16);
    int black = integer ? !bi[0] && !bi[1] && !bi[2] : bc[0] == 0 && bc[1] == 0 && bc[2] == 0;
    int white = integer ? bi[0] == 1 && bi[1] == 1 && bi[2] == 1 : bc[0] == 1 && bc[1] == 1 && bc[2] == 1;
    int clear = integer ? !bi[3] : bc[3] == 0, opaque = integer ? bi[3] == 1 : bc[3] == 1;
    int custom = !((black && (clear || opaque)) || (white && opaque));
    VkSamplerCustomBorderColorCreateInfoEXT cb = {.sType =
                                                      VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT};
    if (custom && (!g_vk.has_border_color || ncustom >= g_vk.border_color_max)) {
        float a = integer ? (float)bi[3] : bc[3];
        float lum = integer ? ((float)bi[0] + (float)bi[1] + (float)bi[2]) / 3 : (bc[0] + bc[1] + bc[2]) / 3;
        if (g_vk.has_border_color)
            LOG_LIMIT(1,
                      "vulkan: more than %u samplers with a border color of their own: the others take the nearest "
                      "of black or white\n",
                      g_vk.border_color_max);
        custom = 0;
        clear = !(a >= 0.5f); /* NaN: transparent, and black */
        black = !(lum >= 0.5f);
    }
    if (custom) {
        if (integer)
            memcpy(cb.customBorderColor.int32, bi, 16);
        else
            memcpy(cb.customBorderColor.float32, bc, 16);
        si.borderColor = integer ? VK_BORDER_COLOR_INT_CUSTOM_EXT : VK_BORDER_COLOR_FLOAT_CUSTOM_EXT;
        si.pNext = &cb;
    } else if (clear)
        si.borderColor = integer ? VK_BORDER_COLOR_INT_TRANSPARENT_BLACK : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    else if (black)
        si.borderColor = integer ? VK_BORDER_COLOR_INT_OPAQUE_BLACK : VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    else
        si.borderColor = integer ? VK_BORDER_COLOR_INT_OPAQUE_WHITE : VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    VkSampler s;
    if (!VK_OK(vkCreateSampler(g_vk.dev, &si, NULL, &s))) return VK_NULL_HANDLE;
    memcpy(tab[j].tsc, tsc, 32);
    tab[j].raise = raise;
    tab[j].integer = integer;
    tab[j].s = s;
    ncustom += custom;
    if (!n++) first = s;
    return s;
}

void vk_tex_frame(void) {
    vk_prompts_frame();
    astc_spread();
    if (g_vk_present % 600 == 0 && g_spread_st.n) {
        LOG("astc-spread: %llu textures drawn from level %u first, their top levels %.1f Mpx decoded over %llu "
            "presents (%llu done, %.1f Mpx), %d waiting\n",
            (unsigned long long)g_spread_st.n, SPREAD_SKIP, (double)g_spread_st.px / 1e6,
            (unsigned long long)g_spread_st.frames, (unsigned long long)g_spread_st.done,
            (double)g_spread_st.done_px / 1e6, g_nspread);
        memset(&g_spread_st, 0, sizeof g_spread_st);
    }
    if (g_vk_present % 600 == 0 && g_ahead.n) { /* every 10 s at 60 */
        LOG("astc-ahead: %llu first uploads, %.1f Mpx, %.1f ms on the render thread (worst %.1f); %llu (%.1f Mpx) "
            "registered 30+ frames before, %llu never registered\n",
            (unsigned long long)g_ahead.n, (double)g_ahead.px / 1e6, (double)g_ahead.ns / 1e6,
            (double)g_ahead.worst_ns / 1e6, (unsigned long long)g_ahead.early, (double)g_ahead.early_px / 1e6,
            (unsigned long long)g_ahead.unknown);
        memset(&g_ahead, 0, sizeof g_ahead);
    }
    if (g_vk_present % 600 == 0 && (g_pack_st.hits || g_pack_st.misses)) {
        LOG("texpack: %llu textures from the pack (%.1f MB read, %.1f ms), %llu not in it, %llu changed by the game and "
            "encoded here\n",
            (unsigned long long)g_pack_st.hits, (double)g_pack_st.bytes / 1e6, (double)g_pack_st.ns / 1e6,
            (unsigned long long)g_pack_st.misses, (unsigned long long)g_pack_st.encoded);
        memset(&g_pack_st, 0, sizeof g_pack_st);
        vk_texahead_report();
    }
    /* textures and render targets not used for ten seconds go; textures come back from memory if they are needed
       again */
    if (g_vk_present % 60 == 0 && g_vk_present > 600) vk_surf_evict(g_vk_present - 600);
}

/* s copied into t, both unchanged since: kept for t and one other texture, so two textures over one target don't
   copy it on each other's binds (a texture's upload generation is new even at an address reused) */
static int copied_into(const Surf* s, const Surf* t) {
    for (int i = 0; i < 2; i++)
        if (s->copied[i].t == t && s->copied[i].wgen == s->wgen && s->copied[i].tgen == t->wgen) return 1;
    return 0;
}
static void mark_copied(Surf* s, Surf* t) {
    if (s->copied[0].t != t) s->copied[1] = s->copied[0];
    s->copied[0].t = t;
    s->copied[0].wgen = s->wgen;
    s->copied[0].tgen = t->wgen;
}
/* a scaled target (resolution= other than 1) reaches a texture by a blit, which f's tiling has to take both ways */
static int can_blit(VkFormat f) {
    VkFormatProperties p = {0};
    VkFormatFeatureFlags need = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
    vkGetPhysicalDeviceFormatProperties(g_vk.phys, f, &p);
    return (p.optimalTilingFeatures & need) == need;
}
/* the command buffer with s ready to be read from and t written to, by copies or blits */
static VkCommandBuffer copy_begin(Surf* s, Surf* t) {
    vk_render_end();
    VkCommandBuffer cb = vk_cmd();
    vk_surf_use(cb, s, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    vk_surf_use(cb, t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    return cb;
}
/* the first count layers of s into t's level l at layer or Z slice dst, w x h: copied, or blitted from s's scaled
   size, nearest (sampling applies the texture's filter later). A blit into a 3D image takes one slice from layer 0, so
   a scaled s goes into slices one at a time (count 1: layered targets take vk_unscaled_image's way there) */
static void copy_layers(VkCommandBuffer cb, Surf* s, Surf* t, u32 l, u32 dst, u32 count, u32 w, u32 h, int z) {
    VkImageAspectFlags aspect = t->f.depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT; /* a depth target */
    VkImageSubresourceLayers from = {aspect, 0, 0, z ? 1 : count};
    VkImageSubresourceLayers to = {aspect, l, z ? 0 : dst, z ? 1 : count};
    i32 dz = z ? (i32)dst : 0;
    if (s->sw == s->w && s->sh == s->h) { /* layers into layers, or into as many slices */
        from.layerCount = count;
        VkImageCopy c = {
            .srcSubresource = from, .dstSubresource = to, .dstOffset = {0, 0, dz}, .extent = {w, h, z ? count : 1}};
        vkCmdCopyImage(cb, s->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &c);
        return;
    }
    VkImageBlit b = {.srcSubresource = from,
                     .srcOffsets = {{0, 0, 0}, {(i32)vk_sc(w), (i32)vk_sc(h), 1}},
                     .dstSubresource = to,
                     .dstOffsets = {{0, 0, dz}, {(i32)w, (i32)h, dz + 1}}};
    vkCmdBlitImage(cb, s->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                   &b, VK_FILTER_NEAREST);
}

/* a texture whose layers, levels or 3D slices were drawn as render targets of their own is copied from those targets
   on the GPU: they are never written back to its memory. Only a target matching one subresource (or, layered, the
   layers from one), whose backing memory has not been overwritten since its draw. Generated textures can be first
   sampled several frames later. Targets are never block compressed: such a texture can't take their texels */
static void tex_take_drawn(Surf* t) {
    if (!t || t->is_rt || t->f.conv || t->f.stencil || t->f.bw != 1) return;
    Surf* small[32];
    Surf** d = small;
    u64 stride = t->layer_size;
    u64 hi = t->gva + t->size;
    int n = vk_rt_drawn_in(t->gva, hi, d, 32);
    if (n > 32) {
        d = malloc((size_t)n * sizeof *d);
        if (d)
            n = vk_rt_drawn_in(t->gva, hi, d, n);
        else {
            d = small;
            n = 32;
        }
    }
    for (int i = 0; i < n; i++) {
        Surf* s = d[i];
        /* depth targets (D16, D32F) feed depth textures of the same host format, through the depth aspect */
        if (s->f.depth != t->f.depth || s->f.stencil || s->f.bpp != t->f.bpp || (t->f.depth && s->f.vk != t->f.vk) ||
            copied_into(s, t))
            continue;
        /* a blit converts formats where a copy keeps the bits: a scaled target only into a texture of its own format */
        int scaled = s->sw != s->w || s->sh != s->h;
        if (scaled && (s->f.vk != t->f.vk || !can_blit(t->f.vk))) continue;
        u64 off = s->gva - t->gva;
        if (t->type == VK_IMAGE_VIEW_TYPE_3D) {
            if (t->f.depth) continue;
            /* A layered color target can be sampled as a 3D texture at the same address. Its slices only exist in
               the host image: guest memory is not updated by draws. Each array layer goes to the matching Z slice */
            if (s->layers > 1 && off == 0 && s->w == t->w && s->h == t->h && s->layers >= t->depth3d &&
                s->f.vk == t->f.vk && vk_rt_drawn_fresh(s)) {
                /* a blit can't take array layers into a 3D image's slices, only a copy can: scaled, the layers
                   are blitted down to an array at the guest's size first, and copied into the slices from there */
                VmaAllocation mm = VK_NULL_HANDLE;
                VkImage mid = scaled ? vk_unscaled_image(s, t->depth3d, &mm) : VK_NULL_HANDLE;
                if (scaled && !mid) continue;
                VkCommandBuffer cb = copy_begin(s, t);
                if (scaled) {
                    VkImageSubresourceLayers all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, t->depth3d};
                    VkImageBlit b = {.srcSubresource = all,
                                     .srcOffsets = {{0, 0, 0}, {(i32)vk_sc(t->w), (i32)vk_sc(t->h), 1}},
                                     .dstSubresource = all,
                                     .dstOffsets = {{0, 0, 0}, {(i32)t->w, (i32)t->h, 1}}};
                    vk_barrier(cb, mid, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_NONE, 0,
                               VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                    vkCmdBlitImage(cb, s->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, mid,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_NEAREST);
                    vk_barrier(cb, mid, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_BLIT_BIT,
                               VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                    /* layer z into slice z, all in one region */
                    VkImageCopy c = {.srcSubresource = all,
                                     .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                     .extent = {t->w, t->h, t->depth3d}};
                    vkCmdCopyImage(cb, mid, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t->img,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
                    vk_defer_image(mid, mm, VK_NULL_HANDLE, VK_NULL_HANDLE); /* freed once the frame is done */
                } else
                    copy_layers(cb, s, t, 0, 0, t->depth3d, t->w, t->h, 1);
                mark_copied(s, t);
                continue;
            }
            /* A 3D block-linear texture can be built a Z slice at a time through 2D targets. The target may be
               wider than the texture: its rows include the 64-byte GOB padding of a narrow slice. */
            if (!t->bl || !s->bl || s->f.vk != t->f.vk) continue;
            u64 lo = 0;
            for (u32 l = 0; l < t->levels; l++) {
                u32 lw = t->w >> l ? t->w >> l : 1, lh = t->h >> l ? t->h >> l : 1;
                u32 ld = t->depth3d >> l ? t->depth3d >> l : 1, lbh, lbd;
                u64 size = bl_level_size(t->w, t->h, t->depth3d, &t->f, t->bh, t->bd, t->tws, l, &lbh, &lbd);
                u32 padded = (ld + (1u << lbd) - 1) & ~((1u << lbd) - 1);
                u64 slice = size / padded;
                u32 rowb = lw * t->f.bpp, gobs_x = (rowb + 63) / 64;
                u32 gobs_y = ((lh + 7) / 8 + (1u << lbh) - 1) >> lbh;
                /* With block depth, Z slices are interleaved across multiple GOB planes rather than contiguous. */
                if (lbd && (gobs_x != 1 || gobs_y != 1)) {
                    lo += size;
                    continue;
                }
                u32 sbh;
                u64 source_size = bl_level_size(s->w, s->h, 1, &s->f, s->bh, 0, 0, 0, &sbh, NULL);
                if (off >= lo && off - lo < slice * ld && (off - lo) % slice == 0 && s->w >= lw && s->h == lh &&
                    sbh == lbh && source_size == slice && vk_rt_drawn_fresh(s)) {
                    copy_layers(copy_begin(s, t), s, t, l, (u32)((off - lo) / slice), 1, lw, lh, 1);
                    mark_copied(s, t);
                    break;
                }
                lo += size;
            }
            continue;
        }
        u32 layer = stride ? (u32)(off / stride) : 0;
        u64 rem = off - (u64)layer * stride, lo = 0;
        for (u32 l = 0; l < t->levels && layer < t->layers; l++) {
            u32 lw = t->w >> l ? t->w >> l : 1, lh = t->h >> l ? t->h >> l : 1;
            /* a block-linear cube mip narrower than a GOB (64 bytes) is drawn as a target a whole GOB row wide:
               only its left lw texels are the mip's */
            int padded_cube_row = t->type == VK_IMAGE_VIEW_TYPE_CUBE && t->bl && s->bl && s->f.vk == t->f.vk &&
                                  s->w > lw && (u64)s->w * s->f.bpp == 64 && (u64)lw * t->f.bpp < 64;
            if (rem == lo && (s->w == lw || padded_cube_row) && s->h == lh) {
                if (!vk_rt_drawn_fresh(s)) break; /* newer guest memory at this address takes precedence */
                /* a layered target spaced as the texture's layers fills as many of them as it has */
                u32 count = 1;
                if (s->layers > 1 && !s->volume && l == 0 && s->layer_size == stride) /* a volume's are GOB slices */
                    count = s->layers < t->layers - layer ? s->layers : t->layers - layer;
                copy_layers(copy_begin(s, t), s, t, l, layer, count, lw, lh, 0);
                mark_copied(s, t);
                break;
            }
            if (!t->bl) break;
            lo += bl_level_size(t->w, t->h, 1, &t->f, t->bh, t->bd, t->tws, l, NULL, NULL);
        }
    }
    if (d != small) free(d);
}
Surf* vk_tex_from_tic(const u32* tic) {
    Surf* t = tex_from_tic(tic, 0);
    tex_take_drawn(t);
    return t;
}
Surf* vk_tex_from_tic_raw(const u32* tic) {
    Surf* t = tex_from_tic(tic, 1);
    tex_take_drawn(t);
    return t;
}
