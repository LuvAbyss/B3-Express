/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from Eden (src/video_core/engines/maxwell_dma.cpp, src/video_core/engines/maxwell_dma.h,
   src/video_core/textures/decoders.cpp):
   SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
   SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
   SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project */
/* The copy engine (MAXWELL_DMA_COPY_A, 0xB0B5; open-gpu-doc's clb0b5.h): one-dimensional copies, and multi-line
   copies between pitch linear and block linear surfaces, with the remap of components and its constant fills, and
   the completion semaphore. Also the copies of linear sources read at submission (dma_snap_*), and the block linear
   span helpers the 2D engine shares */
#include "nx/gpu/maxwell/engines_int.h"

#define DMA_MAX_BYTES (256u << 20) /* a launch past this is a bad register value */

/* ------------------------------------------------------------ block linear spans */
u64 bl_span_size(u32 wbytes, u32 h, u32 bh, u32 z, u32 bd) {
    u64 gobs_x = ((u64)wbytes + 63) >> 6, rows = ((((u64)h + 7) >> 3) + (1u << bh) - 1) >> bh;
    return gobs_x * rows * ((u64)512 << (bh + bd)) * ((u64)(z >> bd) + 1);
}
u8* bl_span_ptr(u64 base, u32 wbytes, u32 h, u32 bh, u32 z, u32 bd, u64* span) {
    u64 n = bl_span_size(wbytes, h, bh, z, bd);
    *span = n;
    return n && n <= DMA_MAX_BYTES ? gmem_ptr(base, n) : NULL;
}

/* ------------------------------------------------------------ a launch's geometry */
/* a surface operand: pitch linear rows, or a block linear surface (the DMA Parameters at 0x1C3 and 0x1CA) */
typedef struct {
    u64 addr;
    int bl;
    u32 pitch;           /* pitch linear */
    u32 wb, h, z, bh, bd; /* block linear: width in bytes, height, the slice (layer), block height and depth */
    u32 ox, oy;          /* the origin, x in bytes */
} DmaSide;
typedef struct {
    int multi, remap, fill, shuffle, keep;
    u32 cs, ns, nd, sel[4]; /* the remap: component size, source and destination components, each one's source */
    u32 width, lines;       /* elements in a line, lines */
    u32 line_in, line_out;  /* bytes of a line on each side */
    DmaSide src, dst;
} DmaOp;
static void dma_side(DmaSide* s, const u32* r, u64 addr, int bl, u32 pitch, u32 params, u32 elem) {
    const u32* p = r + params;
    s->addr = addr;
    s->bl = bl;
    s->pitch = pitch;
    s->bh = (p[0] >> 4) & 15;
    s->bd = (p[0] >> 8) & 15;
    s->wb = p[1] * elem;
    s->h = p[2];
    s->z = p[4];
    s->ox = (p[5] & 0xFFFF) * elem;
    s->oy = p[5] >> 16;
}
static void dma_geometry(const u32* r, u32 v, DmaOp* o) {
    u32 k, elem_in = 1, elem_out = 1, c = r[0x1C2];
    memset(o, 0, sizeof *o);
    o->multi = (v >> 9) & 1;
    o->remap = (v >> 10) & 1;
    o->width = r[0x106];
    o->lines = o->multi ? r[0x107] : 1;
    if (o->remap) {
        o->cs = ((c >> 16) & 3) + 1;
        o->ns = ((c >> 20) & 3) + 1;
        o->nd = ((c >> 24) & 3) + 1;
        elem_in = o->cs * o->ns;
        elem_out = o->cs * o->nd;
        o->fill = 1;
        for (k = 0; k < o->nd; k++) {
            o->sel[k] = (c >> (4 * k)) & 7;
            if (o->sel[k] < 4 || o->sel[k] > 5) o->fill = 0; /* CONST_A and CONST_B only: nothing is read */
            if (o->sel[k] != k) o->shuffle = 1;
            if (o->sel[k] >= 6) o->keep = 1; /* NO_WRITE: the destination's own bytes stay */
        }
        if (o->ns != o->nd) o->shuffle = 1;
    }
    o->line_in = o->width * elem_in;
    o->line_out = o->width * elem_out;
    /* a single line is one-dimensional, as Eden and Ryujinx copy it, whatever the layouts */
    dma_side(&o->src, r, ((u64)(r[0x100] & 0xFF) << 32) | r[0x101], o->multi && !((v >> 7) & 1), r[0x104], 0x1CA,
             elem_in);
    dma_side(&o->dst, r, ((u64)(r[0x102] & 0xFF) << 32) | r[0x103], o->multi && !((v >> 8) & 1), r[0x105], 0x1C3,
             elem_out);
}
/* the bytes of a pitch linear operand's lines */
static u64 dma_pitch_span(const DmaSide* s, u32 lines, u32 line) { return (u64)s->pitch * (lines - 1) + line; }
static u64 dma_span(const DmaSide* s, u32 lines, u32 line) {
    return s->bl ? bl_span_size(s->wb, s->h, s->bh, s->z, s->bd) : dma_pitch_span(s, lines, line);
}

/* ------------------------------------------------------------ copies of linear sources made at submission */
/* The console's copy engine reads a source microseconds after the submission, while the GPU thread may run the copy
   frames later, and games free a copy's source right after submitting it (Catherine frees its texture uploads'
   staging at the start of the next frame, and the piano's keys reflected garbage). So a launch with a linear source
   has its source read at submission, in the channel's order; the copy uses it unless the GPU has written the source
   since (guest memory then has the newer data) */
#define SNAP_RING 64
#define SNAP_TOTAL (256u << 20) /* bytes held at most across the channels: past it copies read memory */
typedef struct {
    u64 job, src, len, gen;
    u32 launch;
    u8* data;
} DmaSnap;
typedef struct {
    DmaSnap e[SNAP_RING];
    u32 head, tail;
} DmaSnapRing;
static DmaSnapRing* g_snaps[128];
static u64 g_snap_bytes;
static PMutex g_snap_lk = PMUTEX_INIT;
static void snap_drop(DmaSnapRing* q) { /* the oldest, the lock held */
    DmaSnap* e = &q->e[q->tail++ % SNAP_RING];
    g_snap_bytes -= e->len;
    free(e->data);
    e->data = NULL;
}
static int dma_linear_source(const DmaOp* o) { return !o->fill && !o->src.bl && o->width && o->lines; }
void dma_snap_launch(u32 channel, u64 job_id, u32 launch_index, const u32* r, u32 v) {
    DmaOp o;
    DmaSnapRing* q;
    DmaSnap* e;
    u64 len, gen;
    u8* data;
    if (channel >= 128) return;
    dma_geometry(r, v, &o);
    if (!dma_linear_source(&o)) return;
    len = dma_pitch_span(&o.src, o.lines, o.line_in);
    pm_lock(&g_snap_lk);
    if (!g_snaps[channel]) g_snaps[channel] = (DmaSnapRing*)calloc(1, sizeof(DmaSnapRing));
    q = g_snaps[channel];
    if (!q || q->head - q->tail == SNAP_RING || len > SNAP_TOTAL - g_snap_bytes) {
        pm_unlock(&g_snap_lk);
        return;
    }
    g_snap_bytes += len;
    pm_unlock(&g_snap_lk);
    gen = gpu_mem_generation(); /* before the read: a GPU write after it shows */
    data = (u8*)malloc((size_t)len);
    if (data) gmem_read_raw(o.src.addr, data, len);
    pm_lock(&g_snap_lk);
    if (!data) {
        g_snap_bytes -= len;
        pm_unlock(&g_snap_lk);
        return;
    }
    e = &q->e[q->head++ % SNAP_RING];
    e->job = job_id;
    e->launch = launch_index;
    e->src = o.src.addr;
    e->len = len;
    e->gen = gen;
    e->data = data;
    pm_unlock(&g_snap_lk);
}
/* the copy of launch (job, launch) when there is one for this source, taken over by the caller; older ones go */
static u8* snap_take(u32 channel, u64 job, u32 launch, u64 src, u64 len, u64* gen) {
    DmaSnapRing* q;
    u8* data = NULL;
    if (channel >= 128) return NULL;
    pm_lock(&g_snap_lk);
    q = g_snaps[channel];
    while (q && q->tail != q->head) {
        DmaSnap* e = &q->e[q->tail % SNAP_RING];
        if (e->job < job || (e->job == job && e->launch < launch)) { /* stale */
            snap_drop(q);
            continue;
        }
        if (e->job == job && e->launch == launch) {
            if (e->src != src || e->len != len) {
                WARN_LIMIT(4, "gpu: the copy of DMA source %llx (%llu bytes) doesn't match its launch (%llx, %llu)\n",
                           (unsigned long long)e->src, (unsigned long long)e->len, (unsigned long long)src,
                           (unsigned long long)len);
                snap_drop(q);
                break;
            }
            data = e->data;
            *gen = e->gen;
            e->data = NULL;
            g_snap_bytes -= e->len;
            q->tail++;
        }
        break;
    }
    pm_unlock(&g_snap_lk);
    return data;
}
void dma_snap_finish(u32 channel, u64 job_id) {
    DmaSnapRing* q;
    if (channel >= 128) return;
    pm_lock(&g_snap_lk);
    q = g_snaps[channel];
    while (q && q->tail != q->head && q->e[q->tail % SNAP_RING].job <= job_id) snap_drop(q);
    pm_unlock(&g_snap_lk);
}
void dma_snap_reset(u32 channel) {
    DmaSnapRing* q;
    if (channel >= 128) return;
    pm_lock(&g_snap_lk);
    q = g_snaps[channel];
    g_snaps[channel] = NULL;
    while (q && q->tail != q->head) snap_drop(q);
    pm_unlock(&g_snap_lk);
    free(q);
}

/* ------------------------------------------------------------ the copy */
/* lines of line bytes of an operand into buf (line bytes apart). from: a host copy of a pitch linear operand */
static void dma_read(const DmaSide* s, u32 lines, u32 line, const u8* from, u8* buf) {
    u64 span = dma_span(s, lines, line);
    u32 y, x;
    if (from) {
        for (y = 0; y < lines; y++) memcpy(buf + (u64)y * line, from + (u64)y * s->pitch, line);
        return;
    }
    if (!s->bl) {
        const u8* p = gmem_ptr(s->addr, span);
        if (!p) gmem_need(s->addr, span);
        for (y = 0; y < lines; y++) {
            if (p)
                memcpy(buf + (u64)y * line, p + (u64)y * s->pitch, line);
            else
                gmem_read_raw(s->addr + (u64)y * s->pitch, buf + (u64)y * line, line);
        }
        return;
    }
    {
        u64 n;
        const u8* p = bl_span_ptr(s->addr, s->wb, s->h, s->bh, s->z, s->bd, &n);
        if (!p) gmem_need(s->addr, span);
        for (y = 0; y < lines; y++) {
            u8* d = buf + (u64)y * line;
            u64 row;
            if (s->oy + y >= s->h) { /* past the surface */
                memset(d, 0, line);
                continue;
            }
            row = bl_offset(0, s->oy + y, s->z, s->wb, s->h, s->bh, s->bd);
            for (x = 0; x < line;) {
                u32 at = s->ox + x, k = 16 - (at & 15);
                u64 off;
                if (k > line - x) k = line - x;
                off = row + bl_offset(at, 0, 0, s->wb, s->h, s->bh, s->bd);
                if (p)
                    memcpy(d + x, p + off, k);
                else
                    gmem_read_raw(s->addr + off, d + x, k);
                x += k;
            }
        }
    }
}
/* buf's lines into an operand: the hooks once over its span, the runs, one record of the write */
static void dma_write(const DmaSide* s, u32 lines, u32 line, const u8* buf) {
    u64 span = dma_span(s, lines, line), n;
    u8* p = s->bl ? bl_span_ptr(s->addr, s->wb, s->h, s->bh, s->z, s->bd, &n) : gmem_ptr(s->addr, span);
    u32 y, x;
    gmem_before_write(s->addr, span);
    for (y = 0; y < lines; y++) {
        const u8* src = buf + (u64)y * line;
        u64 row;
        if (!s->bl) {
            if (p)
                memcpy(p + (u64)y * s->pitch, src, line);
            else
                gmem_write_raw(s->addr + (u64)y * s->pitch, src, line);
            continue;
        }
        if (s->oy + y >= s->h) break;
        row = bl_offset(0, s->oy + y, s->z, s->wb, s->h, s->bh, s->bd);
        for (x = 0; x < line;) {
            u32 at = s->ox + x, k = 16 - (at & 15);
            u64 off;
            if (k > line - x) k = line - x;
            off = row + bl_offset(at, 0, 0, s->wb, s->h, s->bh, s->bd);
            if (p)
                memcpy(p + off, src + x, k);
            else
                gmem_write_raw(s->addr + off, src + x, k);
            x += k;
        }
    }
    gpu_mem_written(s->addr, span);
}
/* the remap: each destination component from a source component, constant A or B, or left as it is */
static void dma_remap(const DmaOp* o, const u32* r, const u8* in, u8* out) {
    u64 i, n = (u64)o->width * o->lines;
    u32 k;
    for (i = 0; i < n; i++) {
        const u8* s = in ? in + i * o->cs * o->ns : NULL;
        u8* d = out + i * o->cs * o->nd;
        for (k = 0; k < o->nd; k++) {
            u32 sel = o->sel[k];
            if (sel == 4 || sel == 5)
                memcpy(d + k * o->cs, &r[0x1C0 + sel - 4], o->cs); /* the constant's low bytes */
            else if (sel < 4) {
                if (s && sel < o->ns)
                    memcpy(d + k * o->cs, s + sel * o->cs, o->cs);
                else
                    memset(d + k * o->cs, 0, o->cs);
            }
        }
    }
}
static void dma_copy(GpuChannel* ch, const DmaOp* o, const u8* from) {
    const u32* r = ch->rdma;
    u64 in_bytes = o->fill ? 0 : (u64)o->line_in * o->lines;
    u64 out_bytes = o->remap && (o->fill || o->shuffle) ? (u64)o->line_out * o->lines : 0;
    u8 *buf, *out;
    if (in_bytes > DMA_MAX_BYTES || out_bytes > DMA_MAX_BYTES) {
        WARN_LIMIT(4, "gpu: a DMA copy of %u lines of %u elements is ignored\n", o->lines, o->width);
        return;
    }
    buf = (u8*)malloc((size_t)(in_bytes + out_bytes) + 1);
    if (!buf) {
        WARN_LIMIT(1, "gpu: no memory for a DMA copy of %llu bytes: skipped\n",
                   (unsigned long long)(in_bytes + out_bytes));
        return;
    }
    if (!o->fill) {
        if (!from && g_renderer_up && g_renderer->rt_flush) g_renderer->rt_flush(o->src.addr);
        dma_read(&o->src, o->lines, o->line_in, from, buf);
    }
    out = buf;
    if (out_bytes) {
        out = buf + in_bytes;
        if (o->keep)
            dma_read(&o->dst, o->lines, o->line_out, NULL, out);
        else
            memset(out, 0, (size_t)out_bytes);
        dma_remap(o, r, o->fill ? NULL : buf, out);
    }
    dma_write(&o->dst, o->lines, o->line_out, out);
    if (g_renderer_up && g_renderer->rt_reload) g_renderer->rt_reload(o->dst.addr);
    free(buf);
}
/* LAUNCH_DMA (0xC0) */
static void dma_launch(GpuChannel* ch, u32 v) {
    const u32* r = ch->rdma;
    u32 launch = ch->dma_launch_index++, sem = (v >> 3) & 3;
    DmaOp o;
    GPU_RELEASE_NOTE(ch, KR_DMA_COPY, ((u64)(r[0x100] & 0xFF) << 32) | r[0x101]);
    dma_geometry(r, v, &o);
    if (o.width && o.lines) {
        u8* snap = NULL;
        u64 gen = 0, len = dma_pitch_span(&o.src, o.lines, o.line_in);
        if (dma_linear_source(&o)) {
            snap = snap_take(ch->id, ch->dma_job_id, launch, o.src.addr, len, &gen);
            if (snap) {
                gmem_need(o.src.addr, len); /* pending GPU results over the source land, and count as writes */
                if (gpu_mem_dirty_since(gen, o.src.addr, len)) {
                    free(snap);
                    snap = NULL;
                }
            }
        }
        dma_copy(ch, &o, snap);
        free(snap);
    }
    /* the completion semaphore, after the copy: one word, or four (the payload, 0, and the timestamp) */
    if (sem == 1 || sem == 2) {
        u64 a = ((u64)(r[0x90] & 0xFF) << 32) | r[0x91];
        u32 w[4];
        GPU_RELEASE_NOTE(ch, KR_DMA_SEM, a);
        w[0] = r[0x92];
        w[1] = 0;
        if (sem == 2) {
            u64 t = gpu_ticks();
            memcpy(&w[2], &t, 8);
        }
        gpu_release_write(a, w, sem == 1 ? 4 : 16);
    }
}
void dma_method(GpuChannel* ch, u32 m, u32 v) {
    if (m >= 0x200) return;
    ch->rdma[m] = v;
    if (m == 0xC0) dma_launch(ch, v);
}
