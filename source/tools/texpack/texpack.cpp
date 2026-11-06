// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// texpack: the game's ASTC textures as BC7, the texture pack the runtime reads (src/nx/gpu/texture/texpack.h has the
// format). Run by the game's preparation step, a program of its own: the archives' Kraken streams are decompressed by
// powzix's ooz (GPL-3.0-or-later), which stays out of the game. The archive walk, the record tables and the block linear
// layout follow Witch Time (tools/wtbuild/src/texpack.rs, runtime/tex_decode.c, GPL-3.0-or-later).
//
//   texpack <romfs folder> <pack folder> [threads]
//
// Each family of archives (st3_000.pkz and st3_001.pkz hold the .dat and .dtt of the same textures) is converted in a
// process of its own (`texpack <romfs> <pack> <threads> --family NAME`), so a crash costs only that family, into
// <family>.b7p.part, renamed to .b7p once complete; a family with a .b7p is skipped, so a stopped run resumes. A texture
// an earlier family holds is left out. Lines on stdout, for the game's progress bar:
//   texpack: total <texels>          once, after counting
//   texpack: texels <n>              n more texels done (a skipped family's count included)
//   texpack: family <name> <state>   done, skipped, failed
//   texpack: ok <textures>           the index written
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <tuple>
#include <vector>
#include <windows.h>

extern "C" {
void astc_decode_image(const uint8_t* src, uint32_t w, uint32_t h, uint32_t bw, uint32_t bh, uint8_t* dst, int srgb);
void bc7_encode_image(const uint8_t* rgba, uint32_t w, uint32_t h, uint8_t* dst);
uint64_t texpack_hash(const uint8_t* p, size_t n);
size_t texpack_lz4_bound(size_t n);
size_t texpack_lz4_compress(const uint8_t* src, size_t n, uint8_t* dst);
}
int Kraken_Decompress(const uint8_t* src, size_t src_len, uint8_t* dst, size_t dst_len);

#define TEXPACK_VERSION 1 // texpack.h
#pragma pack(push, 1)
struct Entry {
    uint64_t hash, data_size, offset;
    uint32_t stored, raw, format, width, height;
    uint16_t levels, layers, shard, pad;
    uint32_t pad2;
};
#pragma pack(pop)
static_assert(sizeof(Entry) == 56, "the pack's entry");
typedef std::tuple<uint64_t, uint64_t, uint32_t, uint32_t, uint32_t, uint16_t, uint16_t> Key;
static Key key_of(const Entry& e) { return Key(e.hash, e.data_size, e.format, e.width, e.height, e.levels, e.layers); }

typedef std::vector<uint8_t> Bytes;
static uint32_t u32(const uint8_t* p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}
static uint64_t u64(const uint8_t* p) {
    uint64_t v;
    memcpy(&v, p, 8);
    return v;
}

// ---- the block linear layout of an ASTC texture as its XT1 record describes it (Witch Time's tex_layout)
static int rule_bh(int hrows) {
    int h = hrows + hrows / 2;
    return h >= 128 ? 4 : h >= 64 ? 3 : h >= 32 ? 2 : h >= 16 ? 1 : 0;
}
static int mip_bh(int hrows, int bh0) {
    while (bh0 > 0 && hrows <= (8 << bh0) / 2) bh0--;
    return bh0;
}
static int mipdim(int v, int l) {
    v >>= l;
    return v > 0 ? v : 1;
}
static const uint8_t k_astc[14][2] = {{4, 4}, {5, 4},  {5, 5},  {6, 5},  {6, 6},   {8, 5},   {8, 6},
                                      {8, 8}, {10, 5}, {10, 6}, {10, 8}, {10, 10}, {12, 10}, {12, 12}};
struct Lay {
    int w, h, levels, layers, bw, bh, srgb;
    int lw[16], lh[16], wb[16], hb[16], lbh[16], gobs[16];
    size_t off[16], layer_stride, data_size;
};
// 2D, 2D array and cube textures; 3D ones (a few colour grading tables) are left to the runtime
static bool layout(Lay* L, int target, int fmt, int width, int height, int depth, int levels, int lay) {
    if (fmt < 0x79 || fmt > 0x94 || target == 2 || target == 0 || target == 3) return false;
    int i = (fmt - 0x79) % 14, bpb = 16, pbh = lay & 255, pbw = lay >> 8 & 255;
    L->bw = k_astc[i][0];
    L->bh = k_astc[i][1];
    L->srgb = fmt >= 0x87;
    L->w = width > 0 ? width : 1;
    L->h = height > 0 ? height : 1;
    int d = depth > 0 ? depth : 1;
    L->layers = target == 4 || target == 6 || target == 9 ? d : target == 8 ? 6 : 1;
    int maxdim = L->w > L->h ? L->w : L->h, maxl;
    for (maxl = 1; (maxdim >> maxl) > 0; maxl++) {}
    L->levels = levels > 0 ? levels : 1;
    if (L->levels > maxl) L->levels = maxl;
    if (L->levels > 16) L->levels = 16;
    int bh0 = pbh <= 4 ? pbh : rule_bh((L->h + L->bh - 1) / L->bh);
    size_t off = 0;
    for (int l = 0; l < L->levels; l++) {
        L->lw[l] = mipdim(L->w, l);
        L->lh[l] = mipdim(L->h, l);
        L->wb[l] = (L->lw[l] + L->bw - 1) / L->bw;
        L->hb[l] = (L->lh[l] + L->bh - 1) / L->bh;
        L->lbh[l] = L->levels == 1 ? bh0 : mip_bh(L->hb[l], bh0);
        int g = (L->wb[l] * bpb + 63) / 64, bwl = pbw > 0 && pbw <= 5 ? pbw : 0;
        L->gobs[l] = bwl && g >= 1 << bwl ? (g + (1 << bwl) - 1) >> bwl << bwl : 0;
        size_t pitch = L->gobs[l] ? (size_t)L->gobs[l] * 64 : ((size_t)L->wb[l] * bpb + 63) / 64 * 64,
               rows = (size_t)8 << L->lbh[l];
        L->off[l] = off;
        off += pitch * (((size_t)L->hb[l] + rows - 1) / rows * rows);
    }
    size_t block = (size_t)512 << mip_bh(L->hb[0], bh0);
    if (L->gobs[0]) block <<= pbw;
    L->layer_stride = (off + block - 1) / block * block;
    L->data_size = L->layers == 1 ? off : L->layer_stride * L->layers;
    return true;
}
// one level's blocks out of block linear memory into rows (Witch Time's bl_copy, one slice)
static void deswizzle(const Lay* L, int l, const uint8_t* bl, uint8_t* lin) {
    int wbytes = L->wb[l] * 16, bh = L->lbh[l];
    size_t gobs_x = L->gobs[l] ? (size_t)L->gobs[l] : ((size_t)wbytes + 63) / 64, block = (size_t)512 << bh,
           rob = block * gobs_x;
    for (int y = 0; y < L->hb[l]; y++) {
        int gy = y >> 3;
        const uint8_t* b = bl + (size_t)(gy >> bh) * rob + (size_t)(gy & ((1 << bh) - 1)) * 512;
        for (int x = 0; x < wbytes;) {
            int n = 16 - (x & 15);
            if (n > wbytes - x) n = wbytes - x;
            int gx = x & 63, yy = y & 7;
            size_t g = (size_t)(x >> 6) * block +
                       (size_t)(((gx & 32) << 3) | ((yy & 6) << 5) | ((gx & 16) << 1) | ((yy & 1) << 4) | (gx & 15));
            memcpy(lin + (size_t)y * wbytes + x, b + g, (size_t)n);
            x += n;
        }
    }
}

// ---- the archives
struct PkzEntry {
    std::string name, codec;
    uint64_t size, off, csize;
};
static bool pkz_list(FILE* f, std::vector<PkzEntry>& out) {
    uint8_t d[0x20];
    if (fread(d, 1, 0x20, f) != 0x20 || memcmp(d, "pkzl", 4) || u32(d + 4) != 0x20000) return false;
    uint32_t count = u32(d + 0x10), toff = u32(d + 0x14), nsize = u32(d + 0x18);
    Bytes t((size_t)count * 0x20 + nsize + 1);
    _fseeki64(f, toff, SEEK_SET);
    if (fread(t.data(), 1, t.size() - 1, f) != t.size() - 1) return false;
    const char* names = (const char*)t.data() + (size_t)count * 0x20;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t* e = t.data() + (size_t)i * 0x20;
        if (u32(e) >= nsize || u32(e + 4) >= nsize) return false;
        out.push_back({names + u32(e), names + u32(e + 4), u64(e + 8), u64(e + 16), u64(e + 24)});
    }
    return true;
}
static bool wanted(const std::string& n) {
    static const char* ext[] = {".dat", ".dtt", ".eff", ".eft", ".wta", ".wtb", ".wtp", ".evn"};
    for (const char* x : ext) {
        size_t l = strlen(x);
        if (n.size() >= l && !n.compare(n.size() - l, l, x)) return true;
    }
    return false;
}
struct Leaf {
    std::string dir, name;
    size_t off, size;
};
static void walk(const Bytes& b, size_t base, const std::string& dir, std::vector<Leaf>& out, int depth = 0) {
    auto u = [&](size_t o, size_t& v) {
        if (o + 4 > b.size()) return false;
        v = u32(&b[o]);
        return true;
    };
    size_t count, off_t, name_t, size_t_, width;
    if (depth > 16 || !u(base + 4, count) || !u(base + 8, off_t) || !u(base + 16, name_t) || !u(base + 20, size_t_) ||
        !u(base + name_t, width))
        return;
    for (size_t i = 0; i < count; i++) {
        size_t off, size;
        if (!u(base + off_t + 4 * i, off) || !u(base + size_t_ + 4 * i, size)) break;
        size_t s = base + name_t + 4 + width * i;
        if (s + width > b.size()) break;
        std::string name((const char*)&b[s], strnlen((const char*)&b[s], width));
        if (base + off + size > b.size()) continue;
        if (size >= 4 && base + off > base && !memcmp(&b[base + off], "DAT\0", 4))
            walk(b, base + off, dir + "/" + name, out, depth + 1);
        else
            out.push_back({dir, name, base + off, size});
    }
}
static std::string stem(const std::string& n) {
    size_t s = n.rfind('/');
    std::string t = s == std::string::npos ? n : n.substr(s + 1);
    size_t d = t.rfind('.');
    return d == std::string::npos ? t : t.substr(0, d);
}
struct Rec {
    size_t to;
    int target, fmt, w, h, d, mips, layout;
};
static std::vector<Rec> records(const uint8_t* b, size_t n) {
    std::vector<Rec> out;
    if (n < 32) return out;
    uint32_t cnt = u32(b + 8), ot = u32(b + 12), xt = u32(b + 28);
    for (uint32_t i = 0; i < cnt && i < 1u << 20; i++) {
        size_t x = (size_t)xt + 56 * i;
        if (x + 56 > n || (size_t)ot + 4 * i + 4 > n || memcmp(b + x, "XT1\0", 4)) continue;
        Rec r = {u32(b + ot + 4 * i),  (int)u32(b + x + 24), (int)u32(b + x + 28), (int)u32(b + x + 32),
                 (int)u32(b + x + 36), (int)u32(b + x + 40), (int)u32(b + x + 20), (int)u32(b + x + 48)};
        if (r.fmt >= 0x79 && r.fmt <= 0x94) out.push_back(r);
    }
    return out;
}
struct Job {
    const uint8_t* p;
    Lay L;
    int fmt;
};

// the family's files, decompressed; false when an archive can't be read
static bool load_family(const std::string& romfs, const std::vector<std::string>& arcs,
                        std::map<std::string, Bytes>& files) {
    for (auto& a : arcs) {
        FILE* f = fopen((romfs + "\\" + a).c_str(), "rb");
        std::vector<PkzEntry> es;
        if (!f || !pkz_list(f, es)) {
            if (f) fclose(f);
            return false;
        }
        for (auto& e : es) {
            if (!wanted(e.name) || e.size > (1ull << 32) || e.csize > (1ull << 32)) continue;
            Bytes c(e.csize + 64), d(e.size + 64);
            _fseeki64(f, (long long)e.off, SEEK_SET);
            if (fread(c.data(), 1, e.csize, f) != e.csize) continue;
            if (e.codec == "None")
                memcpy(d.data(), c.data(), e.size < e.csize ? e.size : e.csize);
            else if (Kraken_Decompress(c.data(), e.csize, d.data(), e.size) != (int)e.size)
                continue;
            d.resize(e.size);
            files[e.name] = std::move(d);
        }
        fclose(f);
    }
    return true;
}
// every ASTC texture the family's record tables point at, with its data where the game's data lies (texpack.rs find_wtp)
static void jobs_of(std::map<std::string, Bytes>& files, std::vector<Job>& jobs) {
    struct FileIndex {
        std::vector<std::pair<Leaf, std::vector<Rec>>> tables;
        std::vector<Leaf> data;
    };
    std::map<std::string, FileIndex> idx;
    for (auto& [name, b] : files) {
        FileIndex& fi = idx[name];
        std::vector<Leaf> leaves;
        if (b.size() >= 4 && !memcmp(b.data(), "DAT\0", 4))
            walk(b, 0, name, leaves);
        else
            leaves.push_back({"", name, 0, b.size()});
        for (auto& l : leaves) {
            if (l.name.size() > 4 && !l.name.compare(l.name.size() - 4, 4, ".wtp"))
                fi.data.push_back(l);
            else if (l.size >= 4 && !memcmp(&b[l.off], "WTB\0", 4)) {
                auto r = records(&b[l.off], l.size);
                if (!r.empty()) fi.tables.push_back({l, r});
            }
        }
    }
    auto of = [&](const std::string& f) -> const std::vector<Leaf>* {
        auto it = idx.find(f);
        return it == idx.end() ? nullptr : &it->second.data;
    };
    auto by_stem = [&](const std::string& f, const std::string& st, std::pair<std::string, Leaf>& r) {
        auto v = of(f);
        if (!v) return false;
        for (auto& w : *v)
            if (stem(w.name) == st) {
                r = {f, w};
                return true;
            }
        return false;
    };
    for (auto& [name, fi] : idx)
        for (auto& [l, recs] : fi.tables) {
            std::pair<std::string, Leaf> w;
            bool found = false;
            if (l.name.size() > 4 && !l.name.compare(l.name.size() - 4, 4, ".wtb")) {
                w = {name, l};
                found = true;
            }
            std::string st = stem(l.name), base = name.substr(0, name.rfind('.')), dtt = base + ".dtt",
                        eft = base + ".eft";
            if (!found) {
                auto v = of(name);
                if (v)
                    for (auto& x : *v)
                        if (stem(x.name) == st && x.dir == l.dir) {
                            w = {name, x};
                            found = true;
                            break;
                        }
            }
            if (!found) found = by_stem(name, st, w) || by_stem(dtt, st, w);
            if (!found) {
                auto v = of(dtt);
                if (v && v->size() == 1) {
                    w = {dtt, (*v)[0]};
                    found = true;
                }
            }
            if (!found) {
                auto v = of(eft);
                if (v)
                    for (auto& x : *v)
                        if (stem(x.name) == st && stem(x.dir) == stem(l.dir)) {
                            w = {eft, x};
                            found = true;
                            break;
                        }
            }
            if (!found) found = by_stem(eft, st, w) || by_stem(base + ".wtp", st, w);
            if (!found) {
                auto v = of(name);
                if (v && v->size() == 1) {
                    w = {name, (*v)[0]};
                    found = true;
                }
            }
            if (!found) continue;
            const Bytes& fb = files[w.first];
            for (auto& r : recs) {
                Job j;
                if (r.to >= w.second.size || !layout(&j.L, r.target, r.fmt, r.w, r.h, r.d, r.mips, r.layout)) continue;
                if (j.L.data_size > w.second.size - r.to) continue;
                j.p = &fb[w.second.off + r.to];
                j.fmt = r.fmt;
                jobs.push_back(j);
            }
        }
}
static uint64_t texels(const Lay& L) {
    uint64_t n = 0;
    for (int l = 0; l < L.levels; l++) n += (uint64_t)L.lw[l] * L.lh[l];
    return n * L.layers;
}
// the entries of a complete shard; false for a file that isn't one of this version
static bool read_shard(const std::string& path, std::vector<Entry>& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    uint8_t foot[16];
    _fseeki64(f, 0, SEEK_END);
    long long len = _ftelli64(f);
    bool ok = len >= 16 && !_fseeki64(f, len - 16, SEEK_SET) && fread(foot, 1, 16, f) == 16 &&
              !memcmp(foot + 12, "B7P1", 4) && u32(foot + 8) == TEXPACK_VERSION;
    if (ok) {
        uint64_t n = u64(foot);
        ok = n <= (1u << 24) && (long long)(n * sizeof(Entry) + 16) <= len;
        if (ok) {
            out.resize((size_t)n);
            ok = !_fseeki64(f, len - 16 - (long long)(n * sizeof(Entry)), SEEK_SET) &&
                 fread(out.data(), sizeof(Entry), (size_t)n, f) == n;
        }
    }
    fclose(f);
    return ok;
}

// the keys a count left for the families after it (no offsets)
static bool read_keys(const std::string& path, std::vector<Entry>& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    Entry e;
    while (fread(&e, sizeof e, 1, f) == 1) out.push_back(e);
    fclose(f);
    return true;
}

static std::map<std::string, std::vector<std::string>> families(const std::string& romfs) {
    std::map<std::string, std::vector<std::string>> fam;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((romfs + "\\*.pkz").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return fam;
    do {
        std::string a = fd.cFileName;
        size_t u = a.rfind('_');
        fam[u == std::string::npos ? a : a.substr(0, u)].push_back(a);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return fam;
}

// a family in this process: count prints its texels; else its new textures into <pack>/<family>.b7p
static int run_family(const std::string& romfs, const std::string& pack, int threads, const std::string& name,
                      bool count) {
    auto fam = families(romfs);
    auto it = fam.find(name);
    if (it == fam.end()) return 2;
    std::map<std::string, Bytes> files;
    if (!load_family(romfs, it->second, files)) return 3;
    std::vector<Job> jobs;
    jobs_of(files, jobs);
    std::set<Key> seen; // the textures earlier families hold, and this one's so far
    for (auto& [fname, arcs] : fam) {
        if (fname >= name) break;
        std::vector<Entry> es;
        /* a count leaves out what earlier counts found; a conversion only what earlier shards hold, as a family that
           failed holds nothing */
        if (read_shard(pack + "\\" + fname + ".b7p", es) || (count && read_keys(pack + "\\" + fname + ".keys", es)))
            for (auto& e : es) seen.insert(key_of(e));
    }
    std::vector<Job> todo;
    std::vector<Entry> keys;
    for (auto& j : jobs) {
        Entry e = {};
        e.hash = texpack_hash(j.p, j.L.data_size);
        e.data_size = j.L.data_size;
        e.format = (uint32_t)j.fmt;
        e.width = (uint32_t)j.L.w;
        e.height = (uint32_t)j.L.h;
        e.levels = (uint16_t)j.L.levels;
        e.layers = (uint16_t)j.L.layers;
        if (!seen.insert(key_of(e)).second) continue;
        todo.push_back(j);
        keys.push_back(e);
    }
    if (count) {
        uint64_t n = 0;
        for (auto& j : todo) n += texels(j.L);
        FILE* k = fopen((pack + "\\" + name + ".keys").c_str(), "wb"); // for the families after it to leave out
        if (k) {
            fwrite(keys.data(), sizeof(Entry), keys.size(), k);
            fclose(k);
        }
        printf("texpack: count %s %llu\n", name.c_str(), (unsigned long long)n);
        return 0;
    }
    std::string part = pack + "\\" + name + ".b7p.part", done = pack + "\\" + name + ".b7p";
    FILE* out = fopen(part.c_str(), "wb");
    if (!out) return 4;
    std::mutex lock;
    std::atomic<size_t> next(0);
    std::atomic<bool> failed(false);
    uint64_t at = 0, pending = 0;
    std::vector<Entry> written;
    auto work = [&]() {
        for (size_t k; !failed && (k = next++) < todo.size();) {
            const Job& j = todo[k];
            const Lay& L = j.L;
            size_t raw = 0;
            for (int l = 0; l < L.levels; l++) raw += (size_t)((L.lw[l] + 3) / 4) * ((L.lh[l] + 3) / 4) * 16;
            raw *= L.layers;
            Bytes bc(raw), lin, rgba;
            size_t o = 0;
            for (int layer = 0; layer < L.layers; layer++)
                for (int l = 0; l < L.levels; l++) {
                    lin.resize((size_t)L.wb[l] * L.hb[l] * 16);
                    rgba.resize((size_t)L.lw[l] * L.lh[l] * 4);
                    deswizzle(&L, l, j.p + (size_t)layer * L.layer_stride + L.off[l], lin.data());
                    astc_decode_image(lin.data(), (uint32_t)L.lw[l], (uint32_t)L.lh[l], (uint32_t)L.bw, (uint32_t)L.bh,
                                      rgba.data(), L.srgb);
                    bc7_encode_image(rgba.data(), (uint32_t)L.lw[l], (uint32_t)L.lh[l], bc.data() + o);
                    o += (size_t)((L.lw[l] + 3) / 4) * ((L.lh[l] + 3) / 4) * 16;
                }
            Bytes lz(texpack_lz4_bound(raw));
            size_t ln = texpack_lz4_compress(bc.data(), raw, lz.data());
            const Bytes& body = ln < raw ? lz : bc;
            size_t bn = ln < raw ? ln : raw;
            std::lock_guard<std::mutex> g(lock);
            if (fwrite(body.data(), 1, bn, out) != bn) {
                failed = true;
                return;
            }
            Entry e = keys[k];
            e.offset = at;
            e.stored = (uint32_t)bn;
            e.raw = (uint32_t)raw;
            at += bn;
            written.push_back(e);
            pending += texels(L);
            if (pending >= 4000000 || written.size() == todo.size()) {
                printf("texpack: texels %llu\n", (unsigned long long)pending);
                fflush(stdout);
                pending = 0;
            }
        }
    };
    std::vector<std::thread> pool;
    for (int t = 0; t < threads; t++) pool.emplace_back(work);
    for (auto& t : pool) t.join();
    if (pending) printf("texpack: texels %llu\n", (unsigned long long)pending);
    uint8_t foot[16];
    uint64_t n = written.size();
    uint32_t v = TEXPACK_VERSION;
    memcpy(foot, &n, 8);
    memcpy(foot + 8, &v, 4);
    memcpy(foot + 12, "B7P1", 4);
    bool ok = !failed && fwrite(written.data(), sizeof(Entry), written.size(), out) == written.size() &&
              fwrite(foot, 1, 16, out) == 16;
    ok = !fclose(out) && ok;
    if (!ok || !MoveFileExA(part.c_str(), done.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileA(part.c_str());
        return 5;
    }
    return 0;
}

static DWORD child(const std::string& args, std::string* captured) {
    char self[MAX_PATH];
    GetModuleFileNameA(NULL, self, MAX_PATH);
    std::string cmd = std::string("\"") + self + "\" " + args;
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    HANDLE rd = NULL, wr = NULL;
    STARTUPINFOA si = {sizeof si};
    PROCESS_INFORMATION pi;
    DWORD code = 1;
    si.dwFlags = STARTF_USESTDHANDLES; /* the child's lines go where ours go: the game reads them through a pipe */
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    if (captured) {
        if (!CreatePipe(&rd, &wr, &sa, 0)) return 1;
        SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
        si.hStdOutput = wr;
    }
    if (!CreateProcessA(NULL, &cmd[0], NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        if (rd) {
            CloseHandle(rd);
            CloseHandle(wr);
        }
        return 1;
    }
    if (captured) {
        CloseHandle(wr);
        char buf[4096];
        DWORD got;
        while (ReadFile(rd, buf, sizeof buf, &got, NULL) && got) captured->append(buf, got);
        CloseHandle(rd);
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return code;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "texpack <romfs folder> <pack folder> [threads]\n");
        return 2;
    }
    std::string romfs = argv[1], pack = argv[2];
    int threads = argc > 3 ? atoi(argv[3]) : 0;
    if (threads <= 0) threads = (int)std::thread::hardware_concurrency();
    if (threads <= 0) threads = 4;
    if (argc > 5 && (!strcmp(argv[4], "--family") || !strcmp(argv[4], "--count")))
        return run_family(romfs, pack, threads, argv[5], !strcmp(argv[4], "--count"));
    CreateDirectoryA(pack.c_str(), NULL);
    DeleteFileA((pack + "\\index.b7i").c_str()); // the runtime's sign of a whole pack; written again at the end
    auto fam = families(romfs);
    if (fam.empty()) {
        fprintf(stderr, "texpack: no .pkz archives in %s\n", romfs.c_str());
        return 1;
    }
    std::string q = "\"" + romfs + "\" \"" + pack + "\" " + std::to_string(threads);
    std::map<std::string, uint64_t> count;
    uint64_t total = 0;
    for (auto& [name, arcs] : fam) DeleteFileA((pack + "\\" + name + ".keys").c_str());
    for (auto& [name, arcs] : fam) { // texels to convert, counted family by family (a skipped one too, for the bar)
        std::string out;
        unsigned long long n = 0;
        char nm[256];
        if (!child(q + " --count " + name, &out) && sscanf(out.c_str(), "texpack: count %255s %llu", nm, &n) == 2)
            count[name] = n;
        total += count[name];
    }
    printf("texpack: total %llu\n", (unsigned long long)total);
    fflush(stdout);
    for (auto& [name, arcs] : fam) {
        std::vector<Entry> es;
        if (read_shard(pack + "\\" + name + ".b7p", es)) {
            printf("texpack: texels %llu\ntexpack: family %s skipped\n", (unsigned long long)count[name], name.c_str());
        } else if (!count[name]) {
            printf("texpack: family %s none\n", name.c_str());
        } else {
            DWORD code = child(q + " --family " + name, NULL);
            printf("texpack: family %s %s\n", name.c_str(), code ? "failed" : "done");
        }
        fflush(stdout);
    }
    for (auto& [name, arcs] : fam) DeleteFileA((pack + "\\" + name + ".keys").c_str());
    // the index: every complete shard's entries, by hash
    std::vector<std::string> names;
    std::vector<Entry> all;
    for (auto& [name, arcs] : fam) {
        std::vector<Entry> es;
        if (!read_shard(pack + "\\" + name + ".b7p", es)) continue;
        for (auto& e : es) {
            e.shard = (uint16_t)names.size();
            all.push_back(e);
        }
        names.push_back(name + ".b7p");
    }
    std::sort(all.begin(), all.end(), [](const Entry& a, const Entry& b) { return a.hash < b.hash; });
    std::string tmp = pack + "\\index.b7i.part";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return 1;
    uint32_t head[4] = {0, TEXPACK_VERSION, (uint32_t)names.size(), (uint32_t)all.size()};
    memcpy(head, "B7PI", 4);
    bool ok = fwrite(head, 4, 4, f) == 4;
    for (auto& n : names) {
        char b[32] = {0};
        snprintf(b, sizeof b, "%s", n.c_str());
        ok = ok && fwrite(b, 1, 32, f) == 32;
    }
    ok = ok && fwrite(all.data(), sizeof(Entry), all.size(), f) == all.size();
    ok = !fclose(f) && ok;
    if (!ok || !MoveFileExA(tmp.c_str(), (pack + "\\index.b7i").c_str(), MOVEFILE_REPLACE_EXISTING)) return 1;
    printf("texpack: ok %zu\n", all.size());
    return 0;
}
