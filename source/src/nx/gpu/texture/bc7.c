/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* BC7 encoding (Khronos Data Format Specification, BPTC). Every block tries mode 6 (one line through RGBA, 7-bit
   endpoints with a p-bit each, 4-bit indices). A block mode 6 doesn't fit almost exactly also tries, when opaque,
   mode 1 (two subsets of RGB, 6-bit endpoints with a p-bit shared by the subset, 3-bit indices) on the two partitions
   whose subsets lie closest to a line, and when it has alpha, mode 5 (RGB and alpha on lines of their own, 7-bit and
   8-bit endpoints, 2-bit indices each) with each of the four channel rotations. The smallest error wins.

   A line: the texels' principal axis (power iteration on their covariance), their projections on it giving the ends,
   then three rounds of quantizing the ends (each p-bit tried), choosing each texel's nearest index, and solving the
   ends again by least squares from those indices. The error is the sum of squared differences over the channels. */
#include <string.h>
#include "nx/gpu/texture/bc7.h"
#include "nx/gpu/texture/bc7_tables.h"

static const int k_w2[4] = {0, 21, 43, 64};
static const int k_w3[8] = {0, 9, 18, 27, 37, 46, 55, 64};
static const int k_w4[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};
enum { P_EACH, P_SHARED, P_NONE }; /* an endpoint's p-bit: its own, one for both ends, none */
#ifndef M1_TRIES
#define M1_TRIES                                                                                                       \
    1 /* the partitions mode 1 fits, the closest to two lines first: a second gains 0.04 dB for 13% more time */
#endif
#ifndef SKIP_ERR
#define SKIP_ERR 256 /* a mode 6 error this small skips the other modes: 0.3 dB less, twice as fast, as 16 */
#endif

/* a fitted line: its two ends' quantized channels and p-bits, each texel's index, the squared error */
typedef struct {
    int q[2][4], p[2];
    int idx[16];
    i64 err;
} Fit;

static const int* weights(int ib) { return ib == 2 ? k_w2 : ib == 3 ? k_w3 : k_w4; }

/* the 8 bits a channel of `bits` bits (and p-bit p) decodes to */
static int unq(int q, int bits, int p, int pmode) {
    int v = pmode == P_NONE ? q : q << 1 | p, n = bits + (pmode != P_NONE);
    return n == 8 ? v : (v << (8 - n)) | (v >> (2 * n - 8));
}
/* the channel value of `bits` bits with p-bit p nearest x; *got its 8 bits */
static int quant(float x, int bits, int p, int pmode, int* got) {
    int n = bits + (pmode != P_NONE), top = (1 << bits) - 1, best = 0, bd = 1 << 30, k, target = (int)(x + 0.5f);
    int q = (int)(x / 255.0f * (float)((1 << n) - 1) + 0.5f);
    if (pmode != P_NONE) q = (q - p) >> 1;
    for (k = q - 1; k <= q + 1; k++) {
        int c = k < 0 ? 0 : k > top ? top : k, v = unq(c, bits, p, pmode), d = target - v;
        if (d * d < bd) {
            bd = d * d;
            best = c;
            *got = v;
        }
    }
    return best;
}

/* the texels of mask (bit i: texel i), channels 0 to nch - 1, fitted to one line: `bits`-bit ends with p-bits as
   pmode says, ib-bit indices */
static void fit(const float px[16][4], unsigned mask, int nch, int bits, int pmode, int ib, Fit* best) {
    const int* wt = weights(ib);
    int nidx = 1 << ib, n = 0, i, c, k, it;
    float mean[4] = {0, 0, 0, 0}, cov[4][4], axis[4], lo = 1e30f, hi = -1e30f, e[2][4], mn[4], mx[4];
    signed char nearest[65]; /* the index whose weight is nearest each of 0 to 64 */
    best->err = (i64)1 << 62;
    for (k = 0, i = 0; i <= 64; i++) {
        while (k + 1 < nidx && wt[k + 1] - i < i - wt[k]) k++;
        nearest[i] = (signed char)k;
    }
    for (c = 0; c < 4; c++) {
        mn[c] = 255;
        mx[c] = 0;
    }
    for (i = 0; i < 16; i++)
        if (mask >> i & 1) {
            n++;
            for (c = 0; c < nch; c++) {
                mean[c] += px[i][c];
                if (px[i][c] < mn[c]) mn[c] = px[i][c];
                if (px[i][c] > mx[c]) mx[c] = px[i][c];
            }
        }
    if (!n) {
        best->err = 0;
        memset(best->q, 0, sizeof best->q);
        best->p[0] = best->p[1] = 0;
        memset(best->idx, 0, sizeof best->idx);
        return;
    }
    for (c = 0; c < nch; c++) mean[c] /= (float)n;
    memset(cov, 0, sizeof cov);
    for (i = 0; i < 16; i++)
        if (mask >> i & 1)
            for (c = 0; c < nch; c++)
                for (k = c; k < nch; k++) cov[c][k] += (px[i][c] - mean[c]) * (px[i][k] - mean[k]);
    for (c = 0; c < nch; c++)
        for (k = 0; k < c; k++) cov[c][k] = cov[k][c];
    for (c = 0; c < nch; c++) axis[c] = mx[c] - mn[c] + 1e-3f;
    for (it = 0; it < 6 && nch > 1; it++) { /* power iteration: the covariance's largest eigenvector */
        float v[4] = {0, 0, 0, 0}, len = 0;
        for (c = 0; c < nch; c++)
            for (k = 0; k < nch; k++) v[c] += cov[c][k] * axis[k];
        for (c = 0; c < nch; c++) len += v[c] * v[c];
        if (len < 1e-12f) break;
        len = 1.0f / __builtin_sqrtf(len);
        for (c = 0; c < nch; c++) axis[c] = v[c] * len;
    }
    if (nch == 1) axis[0] = 1;
    for (i = 0; i < 16; i++)
        if (mask >> i & 1) {
            float t = 0;
            for (c = 0; c < nch; c++) t += (px[i][c] - mean[c]) * axis[c];
            if (t < lo) lo = t;
            if (t > hi) hi = t;
        }
    for (c = 0; c < 4; c++) {
        e[0][c] = c < nch ? mean[c] + lo * axis[c] : 255.0f;
        e[1][c] = c < nch ? mean[c] + hi * axis[c] : 255.0f;
    }
    for (it = 0; it < 3; it++) {
        Fit f;
        int v[2][4], pal[16][4], dv[4], dd = 0;
        float a = 0, b = 0, d = 0, x[2][4];
        for (k = 0; k < 2; k++)
            for (c = 0; c < nch; c++) e[k][c] = e[k][c] < 0 ? 0 : e[k][c] > 255 ? 255 : e[k][c];
        memset(f.q, 0, sizeof f.q);
        if (pmode == P_SHARED) { /* one p-bit for both ends: the one that quantizes them closer */
            i64 be = (i64)1 << 62;
            int p;
            for (p = 0; p < 2; p++) {
                i64 er = 0;
                int qq[2][4], vv[2][4];
                for (k = 0; k < 2; k++)
                    for (c = 0; c < nch; c++) {
                        float dq;
                        qq[k][c] = quant(e[k][c], bits, p, pmode, &vv[k][c]);
                        dq = e[k][c] - (float)vv[k][c];
                        er += (i64)(dq * dq);
                    }
                if (er < be) {
                    be = er;
                    f.p[0] = f.p[1] = p;
                    for (k = 0; k < 2; k++)
                        for (c = 0; c < nch; c++) {
                            f.q[k][c] = qq[k][c];
                            v[k][c] = vv[k][c];
                        }
                }
            }
        } else
            for (k = 0; k < 2; k++) { /* a p-bit an end, each the closer one; or none */
                float be = 1e30f;
                int p;
                for (p = 0; p < (pmode == P_EACH ? 2 : 1); p++) {
                    float er = 0;
                    int qq[4], vv[4];
                    for (c = 0; c < nch; c++) {
                        float dq;
                        qq[c] = quant(e[k][c], bits, p, pmode, &vv[c]);
                        dq = e[k][c] - (float)vv[c];
                        er += dq * dq;
                    }
                    if (er < be) {
                        be = er;
                        f.p[k] = p;
                        for (c = 0; c < nch; c++) {
                            f.q[k][c] = qq[c];
                            v[k][c] = vv[c];
                        }
                    }
                }
            }
        for (c = 0; c < nch; c++) {
            dv[c] = v[1][c] - v[0][c];
            dd += dv[c] * dv[c];
            for (k = 0; k < nidx; k++) pal[k][c] = (v[0][c] * (64 - wt[k]) + v[1][c] * wt[k] + 32) >> 6;
        }
        f.err = 0;
        memset(x, 0, sizeof x);
        for (i = 0; i < 16; i++) {
            i64 bd = (i64)1 << 62;
            int bi = 0, t = 0, k0, k1, tx[4];
            float w;
            if (!(mask >> i & 1)) continue;
            for (c = 0; c < nch; c++) {
                tx[c] = (int)(px[i][c] + 0.5f);
                t += (tx[c] - v[0][c]) * dv[c];
            }
            /* the index the texel's projection on the line rounds to, and its neighbours */
            t = dd ? (t * 64 + dd / 2) / dd : 0;
            k = nearest[t < 0 ? 0 : t > 64 ? 64 : t];
            k0 = k > 0 ? k - 1 : 0;
            k1 = k + 1 < nidx ? k + 1 : nidx - 1;
            for (k = k0; k <= k1; k++) {
                i64 de = 0;
                for (c = 0; c < nch; c++) {
                    int dt = pal[k][c] - tx[c];
                    de += dt * dt;
                }
                if (de < bd) {
                    bd = de;
                    bi = k;
                }
            }
            f.idx[i] = bi;
            f.err += bd;
            w = (float)wt[bi] / 64.0f;
            a += (1 - w) * (1 - w);
            b += (1 - w) * w;
            d += w * w;
            for (c = 0; c < nch; c++) {
                x[0][c] += (1 - w) * px[i][c];
                x[1][c] += w * px[i][c];
            }
        }
        for (i = 0; i < 16; i++)
            if (!(mask >> i & 1)) f.idx[i] = 0;
        if (f.err < best->err) *best = f;
        if (!f.err) break;
        { /* least squares: the ends that best give the texels at the indices just chosen */
            float det = a * d - b * b;
            if (det < 1e-6f && det > -1e-6f) break;
            for (c = 0; c < nch; c++) {
                e[0][c] = (d * x[0][c] - b * x[1][c]) / det;
                e[1][c] = (a * x[1][c] - b * x[0][c]) / det;
            }
        }
    }
}

/* how far a subset lies from its principal line in RGB, from its sums (count, 3 of the channels, 6 of their
   products): the covariance's trace less its largest eigenvalue */
static float line_error(const float s[10]) {
    float n = s[0], cov[3][3], v[3] = {1, 1, 1}, lambda = 0;
    int c, k, it, j = 4;
    if (n < 2) return 0;
    for (c = 0; c < 3; c++)
        for (k = c; k < 3; k++, j++) cov[c][k] = cov[k][c] = s[j] - s[1 + c] * s[1 + k] / n;
    for (it = 0; it < 4; it++) {
        float w[3] = {0, 0, 0}, len = 0;
        for (c = 0; c < 3; c++)
            for (k = 0; k < 3; k++) w[c] += cov[c][k] * v[k];
        for (c = 0; c < 3; c++) len += w[c] * w[c];
        if (len < 1e-12f) break;
        len = __builtin_sqrtf(len);
        lambda = len;
        for (c = 0; c < 3; c++) v[c] = w[c] / len;
    }
    return cov[0][0] + cov[1][1] + cov[2][2] - lambda;
}

static void put(u8* o, int* pos, unsigned v, int n) {
    int i;
    for (i = 0; i < n; i++, (*pos)++)
        if (v >> i & 1) o[*pos >> 3] |= (u8)(1u << (*pos & 7));
}

/* an anchor texel's index must have its top bit clear: if not, the line's ends swap and its indices turn around */
static void anchor(Fit* f, unsigned mask, int at, int ib) {
    int i, c, t, top = (1 << ib) - 1;
    if (f->idx[at] <= top >> 1) return;
    for (c = 0; c < 4; c++) {
        t = f->q[0][c];
        f->q[0][c] = f->q[1][c];
        f->q[1][c] = t;
    }
    t = f->p[0];
    f->p[0] = f->p[1];
    f->p[1] = t;
    for (i = 0; i < 16; i++)
        if (mask >> i & 1) f->idx[i] = top - f->idx[i];
}

/* mode 1 on the two partitions closest to two lines, if it beats err: 1 with s and *part set */
static int try_mode1(const float px[16][4], i64 err, Fit s[2], int* part) {
    float mom[16][10], tot[10] = {0}, b0 = 1e30f, b1 = 1e30f;
    int i, j, k, p0 = 0, p1 = 0, found = 0;
    for (i = 0; i < 16; i++) {
        const float* p = px[i];
        float m[10] = {1,           p[0],        p[1],        p[2],        p[0] * p[0],
                       p[0] * p[1], p[0] * p[2], p[1] * p[1], p[1] * p[2], p[2] * p[2]};
        memcpy(mom[i], m, sizeof m);
        for (j = 0; j < 10; j++) tot[j] += m[j];
    }
    for (k = 0; k < 64; k++) {
        unsigned m = k_bc7_part2[k];
        float s1[10] = {0}, s0[10], e;
        for (i = 0; i < 16; i++)
            if (m >> i & 1)
                for (j = 0; j < 10; j++) s1[j] += mom[i][j];
        for (j = 0; j < 10; j++) s0[j] = tot[j] - s1[j];
        e = line_error(s0) + line_error(s1);
        if (e < b0) {
            b1 = b0;
            p1 = p0;
            b0 = e;
            p0 = k;
        } else if (e < b1) {
            b1 = e;
            p1 = k;
        }
    }
    for (k = 0; k < M1_TRIES; k++) {
        int pk = k ? p1 : p0;
        unsigned m = k_bc7_part2[pk];
        Fit a, b;
        fit(px, ~m & 0xffff, 3, 6, P_SHARED, 3, &a);
        fit(px, m, 3, 6, P_SHARED, 3, &b);
        if (a.err + b.err < err) {
            err = a.err + b.err;
            *part = pk;
            s[0] = a;
            s[1] = b;
            found = 1;
        }
    }
    return found;
}

/* mode 5 with each rotation (alpha swapped with one of R, G, B, or none), if it beats err: 1 with col, alp and *rot */
static int try_mode5(const float px[16][4], i64 err, Fit* col, Fit* alp, int* rot) {
    int r, i, found = 0;
    for (r = 0; r < 4; r++) {
        float rp[16][4], al[16][4];
        Fit a, b;
        for (i = 0; i < 16; i++) {
            memcpy(rp[i], px[i], sizeof rp[i]);
            if (r) {
                rp[i][r - 1] = px[i][3];
                rp[i][3] = px[i][r - 1];
            }
            al[i][0] = rp[i][3];
        }
        fit(rp, 0xffff, 3, 7, P_NONE, 2, &a);
        if (a.err >= err) continue;
        fit(al, 0xffff, 1, 8, P_NONE, 2, &b);
        if (a.err + b.err < err) {
            err = a.err + b.err;
            *col = a;
            *alp = b;
            *rot = r;
            found = 1;
        }
    }
    return found;
}

void bc7_encode_block(const u8 px8[16][4], u8 out[16]) {
    float px[16][4];
    Fit m6, s[2], col, alp;
    int i, c, opaque = 1, part = -1, rot = -1, pos = 0;
    for (i = 0; i < 16; i++) {
        for (c = 0; c < 4; c++) px[i][c] = (float)px8[i][c];
        opaque &= px8[i][3] == 255;
    }
    fit(px, 0xffff, 4, 7, P_EACH, 4, &m6);
    if (m6.err > SKIP_ERR) {
        if (opaque) {
            if (!try_mode1(px, m6.err, s, &part)) part = -1;
        } else if (!try_mode5(px, m6.err, &col, &alp, &rot))
            rot = -1;
    }
    memset(out, 0, 16);
    if (part >= 0) {
        unsigned m = k_bc7_part2[part];
        int idx[16];
        anchor(&s[0], ~m & 0xffff, 0, 3);
        anchor(&s[1], m, k_bc7_anchor2[part], 3);
        for (i = 0; i < 16; i++) idx[i] = m >> i & 1 ? s[1].idx[i] : s[0].idx[i];
        put(out, &pos, 2, 2); /* mode 1 */
        put(out, &pos, (unsigned)part, 6);
        for (c = 0; c < 3; c++) {
            put(out, &pos, (unsigned)s[0].q[0][c], 6);
            put(out, &pos, (unsigned)s[0].q[1][c], 6);
            put(out, &pos, (unsigned)s[1].q[0][c], 6);
            put(out, &pos, (unsigned)s[1].q[1][c], 6);
        }
        put(out, &pos, (unsigned)s[0].p[0], 1);
        put(out, &pos, (unsigned)s[1].p[0], 1);
        for (i = 0; i < 16; i++) put(out, &pos, (unsigned)idx[i], i == 0 || i == k_bc7_anchor2[part] ? 2 : 3);
        return;
    }
    if (rot >= 0) {
        anchor(&col, 0xffff, 0, 2);
        anchor(&alp, 0xffff, 0, 2);
        put(out, &pos, 32, 6); /* mode 5 */
        put(out, &pos, (unsigned)rot, 2);
        for (c = 0; c < 3; c++) {
            put(out, &pos, (unsigned)col.q[0][c], 7);
            put(out, &pos, (unsigned)col.q[1][c], 7);
        }
        put(out, &pos, (unsigned)alp.q[0][0], 8);
        put(out, &pos, (unsigned)alp.q[1][0], 8);
        for (i = 0; i < 16; i++) put(out, &pos, (unsigned)col.idx[i], i ? 2 : 1);
        for (i = 0; i < 16; i++) put(out, &pos, (unsigned)alp.idx[i], i ? 2 : 1);
        return;
    }
    anchor(&m6, 0xffff, 0, 4);
    put(out, &pos, 64, 7); /* mode 6 */
    for (c = 0; c < 4; c++) {
        put(out, &pos, (unsigned)m6.q[0][c], 7);
        put(out, &pos, (unsigned)m6.q[1][c], 7);
    }
    put(out, &pos, (unsigned)m6.p[0], 1);
    put(out, &pos, (unsigned)m6.p[1], 1);
    for (i = 0; i < 16; i++) put(out, &pos, (unsigned)m6.idx[i], i ? 4 : 3);
}

void bc7_encode_image(const u8* rgba, u32 w, u32 h, u8* dst) {
    u8 px[16][4];
    u32 bx, by, i;
    for (by = 0; by < h; by += 4)
        for (bx = 0; bx < w; bx += 4, dst += 16) {
            for (i = 0; i < 16; i++) {
                u32 x = bx + (i & 3), y = by + (i >> 2);
                memcpy(px[i], rgba + ((size_t)(y < h ? y : h - 1) * w + (x < w ? x : w - 1)) * 4, 4);
            }
            bc7_encode_block((const u8(*)[4])px, dst);
        }
}
