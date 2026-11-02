/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from Eden (src/video_core/host1x/codecs/h264.cpp, codecs/decoder.cpp, nvdec.cpp, ffmpeg.cpp,
   codec_types.h):
   SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
   SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
   SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project */
/* NVDEC: H.264 and VP9 jobs decoded with FFmpeg's libavcodec, each picture written into the surface its job names.

   NVDEC writes a picture into its job's surface as it decodes it, in decode order, and the game shows surfaces in
   display order on its own. FFmpeg hands pictures out in display order, later, or not at all for one it judges out
   of order, so its output is drained and dropped: what reaches the surface is the buffer the decoder allocated for
   the picture it started during this job (get_buf keeps a reference to the last one; references FFmpeg makes up for
   missing pictures start before it).

   FFmpeg is a hard dependency: a build without the decoder once left every movie black without a word */
#include "nx/gpu/video/host1x_int.h"
#include "nx/gpu/video/vp9_rebuild.h"
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/log.h>

#define APP_H264    3u /* SET_APPLICATION_ID (0x200), written before every job */
#define APP_VP9     9u
#define CTX_MAGIC   0x534E5856u /* "SNXV": what cmake/ffmpeg-vp9-nvdec.patch looks for at the context's start */
#define VP9_PROBS   0xEA0u      /* nvdec_vp9EntropyProbs_t */
#define HDR_CAP     1024u       /* an SPS and a PPS with full scaling lists and emulation prevention take ~800 */
#define MAX_STREAM  (16u << 20)
#define MAX_DIM     4096u
#define H264_SLOTS  17u
#define FF_LOG_MAX  20u
#define STATUS_TIME 100000u

/* The patched VP9 decoder reads the first three fields through AVCodecContext.opaque: they stay first, in order */
struct NvdecCtx {
    u32 magic;
    u32 prev_invisible;   /* the previous picture was not shown: NVDEC says so only in the next job's setup */
    u8 probs[VP9_PROBS];  /* the job's probability table, which the rebuilt compressed header can't carry whole */
    u32 app;              /* the codec the decoder was opened for (0 none). dec NULL with app set: it failed */
    int cur_new;          /* cur was started during this job */
    u32 last_hdr_n;       /* the SPS and PPS the decoder last accepted (0: send them with the next job) */
    AVCodecContext* dec;
    SnxVp9Builder* vp9;   /* VP9's reference state across jobs */
    AVPacket* pkt;
    AVFrame* cur;         /* the last buffer the decoder started */
    AVFrame* out;         /* FFmpeg's output, dropped */
    u8 last_hdr[HDR_CAP];
};

/* where a picture goes: NV12 planes, block linear, widths in bytes */
typedef struct {
    u64 luma, chroma;
    u32 pitch_y, pitch_c, bh, w, h; /* bh: the GOB block height log2; w, h: the surface's picture size */
} PicOut;

static void wr32(u8* p, u32 off, u32 v) { memcpy(p + off, &v, 4); }
static u32 rd16(const u8* p, u32 off) { return (u32)p[off] | (u32)p[off + 1] << 8; }
static u32 min_u32(u32 a, u32 b) { return a < b ? a : b; }

/* ---- FFmpeg's log */
/* the lines logged, by any decoding thread, and whether the callback is set, each on a cache line of its own */
static struct {
    u32 logged;
    char pad[60];
    int init;
} g_ff;
static void ff_log(void* avcl, int level, const char* fmt, va_list vl) {
    char line[512];
    int prefix = 1;
    size_t len;
    u32 n;
    if (level > AV_LOG_WARNING || __atomic_load_n(&g_ff.logged, __ATOMIC_RELAXED) > FF_LOG_MAX) return;
    n = __atomic_fetch_add(&g_ff.logged, 1, __ATOMIC_RELAXED);
    if (n > FF_LOG_MAX) return;
    if (n == FF_LOG_MAX) {
        WARN("ffmpeg: more messages not shown\n");
        return;
    }
    line[0] = 0;
    av_log_format_line2(avcl, level, fmt, vl, line, (int)sizeof line, &prefix);
    len = strlen(line);
    while (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = 0;
    WARN("ffmpeg: %s\n", line);
}
void ffmpeg_log_init(void) {
    if (__atomic_exchange_n(&g_ff.init, 1, __ATOMIC_ACQ_REL)) return;
    av_log_set_level(AV_LOG_WARNING);
    av_log_set_callback(ff_log);
}
static void ff_warn(const char* what, int err) {
    char msg[AV_ERROR_MAX_STRING_SIZE];
    msg[0] = 0;
    av_strerror(err, msg, sizeof msg);
    WARN_LIMIT(16, "video: NVDEC %s: %s\n", what, msg);
}

/* ---- an H.264 bit writer (RBSP) and NAL units */
typedef struct {
    u8* p;
    u32 cap, bytes, nb; /* whole bytes written, bits in the one being filled */
    int over;           /* a bit didn't fit */
} BW;
static void bw_bit(BW* w, u32 b) {
    if (w->bytes >= w->cap) {
        w->over = 1;
        return;
    }
    if (!w->nb) w->p[w->bytes] = 0;
    if (b) w->p[w->bytes] |= (u8)(0x80u >> w->nb);
    if (++w->nb == 8) {
        w->nb = 0;
        w->bytes++;
    }
}
static void bw_u(BW* w, u32 v, u32 n) {
    while (n--) bw_bit(w, (v >> n) & 1);
}
static void bw_ue(BW* w, u32 v) {
    u64 x = (u64)v + 1;
    u32 len = 0;
    int i;
    while (x >> (len + 1)) len++;
    bw_u(w, 0, len);
    for (i = (int)len; i >= 0; i--) bw_bit(w, (u32)(x >> i) & 1);
}
static void bw_se(BW* w, int v) { bw_ue(w, v > 0 ? 2u * (u32)v - 1 : 2u * (0u - (u32)v)); }
static void bw_trailing(BW* w) {
    bw_bit(w, 1);
    while (w->nb) bw_bit(w, 0);
}
/* a NAL unit: a 4-byte start code, the header byte, the RBSP with emulation prevention. Its length, 0 when it
   doesn't fit cap */
static u32 nal_out(u8* dst, u32 cap, u32 hdr, const u8* rbsp, u32 n) {
    u32 o = 5, zeros = 0, i;
    if (cap < 5) return 0;
    dst[0] = dst[1] = dst[2] = 0;
    dst[3] = 1;
    dst[4] = (u8)hdr;
    for (i = 0; i < n; i++) {
        if (zeros >= 2 && rbsp[i] <= 3) {
            if (o >= cap) return 0;
            dst[o++] = 3;
            zeros = 0;
        }
        if (o >= cap) return 0;
        dst[o++] = rbsp[i];
        zeros = rbsp[i] ? 0 : zeros + 1;
    }
    return o;
}
/* a field of b bits, sign extended */
static int sext(u32 v, u32 b) {
    u32 m = 1u << (b - 1);
    v &= (m << 1) - 1;
    return (int)(v ^ m) - (int)m;
}
static void scaling_list(BW* w, const u8* list, u32 n) {
    static const u8 zz4[16] = {0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15};
    static const u8 zz8[64] = {0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
                               12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
                               35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
                               58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};
    int last = 8;
    u32 i;
    for (i = 0; i < n; i++) {
        int v = list[n == 16 ? zz4[i] : zz8[i]], delta = v - last;
        if (delta > 127) delta -= 256;
        if (delta < -128) delta += 256;
        bw_se(w, delta);
        last = v;
    }
}
/* the SPS and PPS of nvdec_h264_pic_s s (the job's setup, offsets from its start). Their length in out, 0 when the
   setup can't make them or they don't fit */
static u32 h264_headers(const u8* s, u8* out, u32 cap) {
    u8 rb[HDR_CAP];
    BW w;
    u32 lo = rd32(s, 0xB0), hi = rd32(s, 0xB4), chroma = (lo >> 12) & 3, poc = (lo >> 14) & 3;
    u32 wmb = rd32(s, 0x64), hmb = rd32(s, 0x68), frame_mbs = rd32(s, 0x60) != 0, t8 = rd32(s, 0x88) != 0;
    u32 l0 = rd32(s, 0x78), l1 = rd32(s, 0x7C), refs, n, k, i;
    if (!wmb || !hmb || wmb > MAX_DIM / 16 || hmb > MAX_DIM / 16 || (!frame_mbs && hmb < 2)) return 0;
    refs = (l0 > l1 ? l0 : l1) < 15 ? (l0 > l1 ? l0 : l1) + 1 : 16;
    if (refs < 4) refs = 4;
    memset(&w, 0, sizeof w);
    w.p = rb;
    w.cap = sizeof rb;
    bw_u(&w, 100, 8); /* High */
    bw_u(&w, 0, 8);
    bw_u(&w, 51, 8);
    bw_ue(&w, 0); /* seq_parameter_set_id */
    bw_ue(&w, chroma);
    if (chroma == 3) bw_bit(&w, 0); /* separate_colour_plane_flag */
    bw_ue(&w, 0);                   /* bit depths 8 */
    bw_ue(&w, 0);
    bw_bit(&w, (rd32(s, 0x2D0) >> 1) & 1); /* qpprime_y_zero_transform_bypass_flag */
    bw_bit(&w, 0);                         /* seq_scaling_matrix_present_flag: the PPS carries them */
    bw_ue(&w, (lo >> 8) & 0xF);            /* log2_max_frame_num_minus4 */
    bw_ue(&w, poc);
    if (poc == 0)
        bw_ue(&w, rd32(s, 0x58));
    else if (poc == 1) {
        bw_bit(&w, rd32(s, 0x5C) != 0);
        bw_se(&w, 0);
        bw_se(&w, 0);
        bw_ue(&w, 0);
    }
    bw_ue(&w, refs);
    bw_bit(&w, 0); /* gaps_in_frame_num_value_allowed_flag */
    bw_ue(&w, wmb - 1);
    bw_ue(&w, (frame_mbs ? hmb : hmb / 2) - 1);
    bw_bit(&w, frame_mbs);
    if (!frame_mbs) bw_bit(&w, lo & 1); /* mb_adaptive_frame_field_flag */
    bw_bit(&w, (lo >> 1) & 1);          /* direct_8x8_inference_flag */
    bw_bit(&w, 0);                      /* frame_cropping_flag */
    bw_bit(&w, 0);                      /* vui_parameters_present_flag */
    bw_trailing(&w);
    if (w.over || !(n = nal_out(out, cap, 0x67, rb, w.bytes))) return 0;

    memset(&w, 0, sizeof w);
    w.p = rb;
    w.cap = sizeof rb;
    bw_ue(&w, 0); /* pic_parameter_set_id */
    bw_ue(&w, 0);
    bw_bit(&w, rd32(s, 0x70) != 0); /* entropy_coding_mode_flag */
    bw_bit(&w, rd32(s, 0x74) != 0); /* bottom_field_pic_order_in_frame_present_flag */
    bw_ue(&w, 0);                   /* num_slice_groups_minus1 */
    bw_ue(&w, l0);
    bw_ue(&w, l1);
    bw_bit(&w, (lo >> 2) & 1); /* weighted_pred_flag */
    bw_u(&w, hi & 3, 2);       /* weighted_bipred_idc */
    bw_se(&w, sext(lo >> 16, 6));
    bw_se(&w, 0); /* pic_init_qs_minus26 */
    bw_se(&w, sext(lo >> 22, 5));
    bw_bit(&w, rd32(s, 0x80) != 0); /* deblocking_filter_control_present_flag */
    bw_bit(&w, (lo >> 3) & 1);      /* constrained_intra_pred_flag */
    bw_bit(&w, rd32(s, 0x84) != 0); /* redundant_pic_cnt_present_flag */
    bw_bit(&w, t8);
    bw_bit(&w, 1); /* pic_scaling_matrix_present_flag */
    for (i = 0; i < 6; i++) {
        bw_bit(&w, 1);
        scaling_list(&w, s + 0x1C0 + 16 * i, 16);
    }
    if (t8)
        for (i = 0; i < (chroma == 3 ? 6u : 2u); i++) {
            bw_bit(&w, i < 2); /* NVDEC has two 8x8 lists: 4:4:4's chroma ones fall back to them */
            if (i < 2) scaling_list(&w, s + 0x220 + 64 * i, 64);
        }
    bw_se(&w, sext(lo >> 27, 5)); /* second_chroma_qp_index_offset */
    bw_trailing(&w);
    if (w.over || !(k = nal_out(out + n, cap - n, 0x68, rb, w.bytes))) return 0;
    return n + k;
}

/* ---- packets */
static int nal_ok(u8 b) { return !(b & 0x80) && (b & 0x1F) >= 1 && (b & 0x1F) <= 23; }
/* complete 4-byte big-endian length-prefixed NAL units, end to end */
static int is_avcc(const u8* s, u32 n) {
    u32 o = 0;
    while (o < n) {
        u32 len;
        if (n - o < 5) return 0;
        len = (u32)s[o] << 24 | (u32)s[o + 1] << 16 | (u32)s[o + 2] << 8 | s[o + 3];
        if (!len || len > n - o - 4 || !nal_ok(s[o + 4])) return 0;
        o += 4 + len;
    }
    return n != 0;
}
static int is_annexb(const u8* s, u32 n) {
    u32 i = 0;
    while (i < n && !s[i]) i++;
    return i >= 2 && i + 1 < n && s[i] == 1 && nal_ok(s[i + 1]);
}
/* the job's packet: hn bytes of headers, then the bitstream as Annex B. It comes as Annex B, as length-prefixed NAL
   units (tried first: a length of 256..511 starts 00 00 01) or as one bare NAL unit. 0 or an AVERROR */
static int make_packet(AVPacket* p, const u8* hdr, u32 hn, const u8* s, u32 n) {
    int kind, r;
    u32 size, o, i;
    av_packet_unref(p);
    if (is_avcc(s, n))
        kind = 1;
    else if (is_annexb(s, n))
        kind = 0;
    else if (n && nal_ok(s[0]))
        kind = 2;
    else
        return AVERROR_INVALIDDATA;
    size = hn + n + (kind == 2 ? 3 : 0);
    if ((r = av_new_packet(p, (int)size)) < 0) return r;
    memcpy(p->data, hdr, hn);
    o = hn;
    if (kind == 0)
        memcpy(p->data + o, s, n);
    else if (kind == 2) {
        p->data[o] = p->data[o + 1] = 0;
        p->data[o + 2] = 1;
        memcpy(p->data + o + 3, s, n);
    } else
        for (i = 0; i < n;) {
            u32 len = (u32)s[i] << 24 | (u32)s[i + 1] << 16 | (u32)s[i + 2] << 8 | s[i + 3];
            p->data[o] = p->data[o + 1] = p->data[o + 2] = 0;
            p->data[o + 3] = 1;
            memcpy(p->data + o + 4, s + i + 4, len);
            o += 4 + len;
            i += 4 + len;
        }
    return 0;
}

/* ---- the picture into its surface */
/* NV12 planes, block linear. The layout's offset is separable, offset(x, y) = offset(x, 0) + offset(0, y), so the
   columns of 16-byte runs are walked once and each row once. 1 when written */
static int write_pic(const AVFrame* f, const PicOut* t) {
    u64 col[MAX_DIM / 16], ext_y, ext_c;
    u8 line[MAX_DIM + 2], *py, *pc;
    u32 w, h, pairs, cw, ch, gob_rows, runs, i, x, y;
    if (f->format != AV_PIX_FMT_YUV420P && f->format != AV_PIX_FMT_YUVJ420P) {
        WARN_LIMIT(4, "video: NVDEC picture format %d isn't written (4:2:0 only)\n", f->format);
        return 0;
    }
    if (f->width <= 0 || f->height <= 0 || t->bh > 5) return 0;
    w = min_u32(min_u32((u32)f->width, t->w), min_u32(min_u32(t->pitch_y, t->pitch_c / 2 * 2), MAX_DIM));
    h = min_u32(min_u32((u32)f->height, t->h), MAX_DIM);
    if (!w || !h) return 0;
    pairs = (w + 1) / 2;
    cw = 2 * pairs;
    ch = (h + 1) / 2;
    /* the surfaces mapped for their whole block linear extent: whole GOBs wide, whole blocks high */
    gob_rows = 8u << t->bh;
    ext_y = (u64)((t->pitch_y + 63) / 64 * 64) * ((t->h + gob_rows - 1) / gob_rows * gob_rows);
    ext_c = (u64)((t->pitch_c + 63) / 64 * 64) * (((t->h + 1) / 2 + gob_rows - 1) / gob_rows * gob_rows);
    py = iova_ptr(t->luma, ext_y);
    pc = iova_ptr(t->chroma, ext_c);
    if (!py || !pc) {
        WARN_LIMIT(8, "video: NVDEC surface %llx/%llx isn't mapped, picture dropped\n", (unsigned long long)t->luma,
                   (unsigned long long)t->chroma);
        return 0;
    }
    runs = (cw + 15) / 16;
    for (i = 0; i < runs; i++) col[i] = bl_offset(16 * i, 0, 0, t->pitch_y, t->h, t->bh, 0);
    for (y = 0; y < h; y++) {
        u64 row = bl_offset(0, y, 0, t->pitch_y, t->h, t->bh, 0);
        const u8* src = f->data[0] + (size_t)y * (size_t)f->linesize[0];
        for (i = 0, x = 0; x < w; i++, x += 16) memcpy(py + row + col[i], src + x, min_u32(16, w - x));
    }
    for (y = 0; y < ch; y++) {
        u64 row = bl_offset(0, y, 0, t->pitch_c, (t->h + 1) / 2, t->bh, 0);
        const u8* u = f->data[1] + (size_t)y * (size_t)f->linesize[1];
        const u8* v = f->data[2] + (size_t)y * (size_t)f->linesize[2];
        for (x = 0; x < pairs; x++) {
            line[2 * x] = u[x];
            line[2 * x + 1] = v[x];
        }
        for (i = 0, x = 0; x < cw; i++, x += 16) memcpy(pc + row + col[i], line + x, min_u32(16, cw - x));
    }
    return 1;
}

/* ---- the decoder */
/* FFmpeg's buffer for a picture it starts: kept as this job's */
static int get_buf(AVCodecContext* c, AVFrame* f, int flags) {
    NvdecCtx* d = c->opaque;
    int r = avcodec_default_get_buffer2(c, f, flags);
    if (r < 0) return r;
    av_frame_unref(d->cur);
    d->cur_new = 0;
    if ((r = av_frame_ref(d->cur, f)) < 0) {
        av_frame_unref(f);
        return r;
    }
    d->cur_new = 1;
    return 0;
}
static void dec_close(NvdecCtx* d) {
    if (d->dec) avcodec_free_context(&d->dec);
    snx_vp9_free(d->vp9);
    d->vp9 = NULL;
    av_frame_unref(d->cur);
    av_packet_unref(d->pkt);
    d->cur_new = 0;
    d->last_hdr_n = 0;
    d->prev_invisible = 0;
}
/* a fresh decoder for codec app: 1 when it opened. Slice threads only, since frame threads would still be decoding a
   picture when its surface is written; VP9's tile columns decode side by side on four */
static int dec_open(NvdecCtx* d, u32 app) {
    const AVCodec* codec;
    int r;
    dec_close(d);
    d->app = app;
    ffmpeg_log_init();
    codec = avcodec_find_decoder(app == APP_VP9 ? AV_CODEC_ID_VP9 : AV_CODEC_ID_H264);
    if (!codec) {
        WARN("video: FFmpeg has no %s decoder, so the movies stay black\n", app == APP_VP9 ? "VP9" : "H.264");
        return 0;
    }
    if (app == APP_VP9 && !(d->vp9 = snx_vp9_new())) return 0;
    if (!(d->dec = avcodec_alloc_context3(codec))) goto fail;
    d->dec->opaque = d;
    d->dec->get_buffer2 = get_buf;
    d->dec->thread_type = FF_THREAD_SLICE;
    d->dec->thread_count = app == APP_VP9 ? 4 : 1;
    if ((r = avcodec_open2(d->dec, codec, NULL)) < 0) {
        ff_warn("cannot open its decoder", r);
        avcodec_free_context(&d->dec);
        goto fail;
    }
    return 1;
fail:
    snx_vp9_free(d->vp9);
    d->vp9 = NULL;
    return 0;
}
/* FFmpeg's output, dropped. 0 once it has none left (EAGAIN), -1 on an error (EOF is one: it never flushes) */
static int drain(NvdecCtx* d) {
    for (;;) {
        int r = avcodec_receive_frame(d->dec, d->out);
        if (!r) {
            av_frame_unref(d->out);
            continue;
        }
        if (r == AVERROR(EAGAIN)) return 0;
        ff_warn("decoding failed", r);
        return -1;
    }
}
/* the job's packet into the decoder. A full decoder (EAGAIN) is drained and takes it again, once: a buffer started
   while draining is an older job's, so it is let go */
static int send(NvdecCtx* d) {
    int r = avcodec_send_packet(d->dec, d->pkt);
    if (r == AVERROR(EAGAIN)) {
        d->cur_new = 0;
        if (drain(d)) return -1;
        if (d->cur_new) av_frame_unref(d->cur);
        d->cur_new = 0;
        r = avcodec_send_packet(d->dec, d->pkt);
    }
    av_packet_unref(d->pkt);
    if (r < 0) {
        ff_warn("the decoder refused a picture", r);
        return -1;
    }
    return 0;
}

/* nvdec_status_s: the macroblocks decoded and with errors, the cycle count, the error flag. Players take all zero
   for a failure too */
static void put_status(u8* st, int ok, u32 mbs) {
    if (!st) return;
    wr32(st, 0, ok ? mbs : 0);
    wr32(st, 4, ok ? 0 : mbs);
    wr32(st, 8, STATUS_TIME);
    wr32(st, 12, !ok);
}
/* a failed job: what it captured goes, so a later second field can't publish stale pixels, and the decoder is sent
   the headers again */
static void job_fail(NvdecCtx* d, u8* st, u32 mbs) {
    av_frame_unref(d->cur);
    av_packet_unref(d->pkt);
    d->cur_new = 0;
    d->last_hdr_n = 0;
    put_status(st, 0, mbs);
}
static void h264_job(NvdecCtx* d, const u32* r, u8* st) {
    u8 hdr[HDR_CAP];
    const u8 *s = iova_ptr((u64)r[0x404 / 4] << 8, 0x300), *bits = NULL;
    u32 mbs = 0, len = 0, hn, prefix, lo, idx, tile;
    PicOut t;
    if (s) {
        mbs = rd32(s, 0x64) * rd32(s, 0x68);
        len = rd32(s, 0x48);
    }
    if (len && len <= MAX_STREAM) bits = iova_ptr((u64)r[0x408 / 4] << 8, len);
    if (!bits) {
        WARN_LIMIT(8, "video: an H.264 job's setup or bitstream isn't mapped (length %u)\n", len);
        job_fail(d, st, mbs);
        return;
    }
    if (!(hn = h264_headers(s, hdr, sizeof hdr))) {
        WARN_LIMIT(8, "video: an H.264 job's parameter sets can't be built\n");
        job_fail(d, st, mbs);
        return;
    }
    /* the parameter sets go first only when they changed since the decoder last took them */
    prefix = hn == d->last_hdr_n && !memcmp(hdr, d->last_hdr, hn) ? 0 : hn;
    if (make_packet(d->pkt, hdr, prefix, bits, len) < 0) {
        WARN_LIMIT(8, "video: an H.264 job's bitstream (%u bytes) is not H.264\n", len);
        job_fail(d, st, mbs);
        return;
    }
    d->cur_new = 0;
    if (send(d)) {
        job_fail(d, st, mbs);
        return;
    }
    if (prefix) {
        memcpy(d->last_hdr, hdr, hn);
        d->last_hdr_n = hn;
    }
    if (drain(d)) {
        job_fail(d, st, mbs);
        return;
    }
    /* a second field completes the picture its first field started */
    lo = rd32(s, 0xB0);
    if (!d->cur_new && !((lo & 0xA0) == 0xA0 && d->cur->buf[0])) {
        WARN_LIMIT(8, "video: an H.264 job started no picture\n");
        job_fail(d, st, mbs);
        return;
    }
    idx = (rd32(s, 0xB4) >> 2) & 0x7F; /* CurrPicIdx */
    tile = rd32(s, 0x6C);
    if (idx >= H264_SLOTS || (tile & 3)) {
        WARN_LIMIT(4, "video: an H.264 picture into surface %u, tile format %u, isn't written\n", idx, tile & 3);
        job_fail(d, st, mbs);
        return;
    }
    t.luma = ((u64)r[0x430 / 4 + idx] + rd32(s, 0x9C)) << 8;
    t.chroma = ((u64)r[0x474 / 4 + idx] + rd32(s, 0xA8)) << 8;
    t.pitch_y = rd32(s, 0x8C);
    t.pitch_c = rd32(s, 0x90);
    t.bh = ((tile >> 2) & 7) + 1;
    t.w = rd32(s, 0x64) * 16;
    t.h = rd32(s, 0x68) * 16;
    if (!write_pic(d->cur, &t)) {
        job_fail(d, st, mbs);
        return;
    }
    put_status(st, 1, mbs);
}
static void vp9_job(NvdecCtx* d, const u32* r, u8* st) {
    const u8 *s = iova_ptr((u64)r[0x404 / 4] << 8, 0x100), *probs = iova_ptr((u64)r[0x5C0 / 4] << 8, VP9_PROBS);
    const u8* body = NULL;
    u32 mbs = 0, len = 0;
    SnxVp9Frame f;
    PicOut t;
    if (s) {
        mbs = ((rd16(s, 0x60) + 15) / 16) * ((rd16(s, 0x62) + 15) / 16);
        len = rd32(s, 0x30);
    }
    if (len && len <= MAX_STREAM) body = iova_ptr((u64)r[0x408 / 4] << 8, len);
    if (!probs || !body) {
        WARN_LIMIT(8, "video: a VP9 job's setup, probabilities or frame (%u bytes) aren't mapped\n", len);
        job_fail(d, st, mbs);
        return;
    }
    if (!snx_vp9_compose(d->vp9, s, probs, body, len, r, &f) || !f.size) {
        WARN_LIMIT(8, "video: a VP9 job's frame can't be rebuilt\n");
        job_fail(d, st, mbs);
        return;
    }
    /* what the patched decoder reads through opaque, for this picture */
    d->prev_invisible = !f.previous_visible;
    memcpy(d->probs, probs, VP9_PROBS);
    av_packet_unref(d->pkt);
    if (av_new_packet(d->pkt, (int)f.size) < 0) {
        job_fail(d, st, mbs);
        return;
    }
    memcpy(d->pkt->data, f.data, f.size);
    d->cur_new = 0;
    if (send(d) || drain(d)) {
        job_fail(d, st, mbs);
        return;
    }
    /* a picture shown only later still decodes into its surface; one decoding nothing new (show_existing_frame)
       leaves the surfaces as they are */
    if (d->cur_new) {
        t.luma = f.luma;
        t.chroma = f.chroma;
        t.pitch_y = f.pitch_y;
        t.pitch_c = f.pitch_c;
        t.bh = ((f.layout >> 2) & 7) + 1;
        t.w = f.width;
        t.h = f.height;
        if (f.layout & 3)
            WARN_LIMIT(4, "video: a VP9 picture in tile format %u isn't written\n", f.layout & 3);
        else
            write_pic(d->cur, &t);
    }
    put_status(st, 1, mbs);
}

NvdecCtx* nvdec_new(void) {
    NvdecCtx* d = calloc(1, sizeof *d);
    if (!d) return NULL;
    d->magic = CTX_MAGIC;
    d->pkt = av_packet_alloc();
    d->cur = av_frame_alloc();
    d->out = av_frame_alloc();
    if (!d->pkt || !d->cur || !d->out) {
        nvdec_free(d);
        return NULL;
    }
    return d;
}
void nvdec_free(NvdecCtx* d) {
    if (!d) return;
    if (d->cur && d->pkt) dec_close(d);
    av_packet_free(&d->pkt);
    av_frame_free(&d->cur);
    av_frame_free(&d->out);
    free(d);
}
void nvdec_execute(NvdecCtx* d, const u32* r) {
    u32 app = r[0x200 / 4];
    u8* st = iova_ptr((u64)r[0x424 / 4] << 8, 16);
    if (!d) return;
    /* VP8 (5) and HEVC (7) aren't decoded: the job fails and the decoder stays as it is */
    if (app != APP_H264 && app != APP_VP9) {
        WARN_LIMIT(4, "video: NVDEC jobs of codec %u aren't decoded\n", app);
        put_status(st, 0, 0);
        return;
    }
    /* a channel that changes codec starts over; a decoder that failed to open stays failed */
    if (d->app != app) dec_open(d, app);
    if (!d->dec) {
        job_fail(d, st, 0);
        return;
    }
    if (app == APP_H264)
        h264_job(d, r, st);
    else
        vp9_job(d, r, st);
}
