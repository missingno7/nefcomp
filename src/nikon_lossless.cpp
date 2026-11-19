#include "nikon_lossless.h"
#include <algorithm>
#include <cstring>

namespace nefcomp {

namespace {
// Huffman tables: 16 code-length counts followed by symbols.
const uint8_t kTrees[6][32] = {
    {0,1,5,1,1,1,1,1,1,2,0,0,0,0,0,0, 5,4,3,6,2,7,1,0,8,9,11,10,12},
    {0,1,5,1,1,1,1,1,1,2,0,0,0,0,0,0, 0x39,0x5a,0x38,0x27,0x16,5,4,3,2,1,0,11,12,12},
    {0,1,4,2,3,1,2,0,0,0,0,0,0,0,0,0, 5,4,6,3,7,2,8,1,9,0,10,11,12},
    {0,1,4,3,1,1,1,1,1,2,0,0,0,0,0,0, 5,6,4,7,8,3,9,2,1,0,10,11,12,13,14},
    {0,1,5,1,1,1,1,1,1,1,2,0,0,0,0,0, 8,0x5c,0x4b,0x3a,0x29,7,6,5,4,3,2,1,0,13,14},
    {0,1,4,2,2,3,1,2,0,0,0,0,0,0,0,0, 7,6,8,5,9,4,10,3,11,12,2,0,1,13,14},
};

struct Huff {
    // lookup by 16-bit peek: (length<<8)|symbol
    std::vector<uint16_t> lut;
    explicit Huff(const uint8_t *tree) : lut(1 << 16, 0) {
        const uint8_t *counts = tree, *sym = tree + 16;
        unsigned code = 0, pos = 0;
        for (unsigned len = 1; len <= 16; len++) {
            for (unsigned i = 0; i < counts[len - 1]; i++) {
                unsigned first = code << (16 - len), n = 1u << (16 - len);
                for (unsigned k = 0; k < n; k++) lut[first + k] = uint16_t((len << 8) | sym[pos]);
                code++; pos++;
            }
            code <<= 1;
        }
    }
};

struct BitReader {
    const uint8_t *p, *end;
    uint64_t buf = 0; int cnt = 0;
    BitReader(const uint8_t *b, size_t n) : p(b), end(b + n) {}
    inline void fill() {
        while (cnt <= 56) { buf |= uint64_t(p < end ? *p : 0) << (56 - cnt); if (p < end) ++p; else if (cnt > 200) break; cnt += 8; }
    }
    inline unsigned peek16() { if (cnt < 16) fill(); return unsigned(buf >> 48); }
    inline void skip(int n) { buf <<= n; cnt -= n; }
    inline unsigned get(int n) {
        if (!n) return 0;
        if (cnt < n) fill();
        unsigned v = unsigned(buf >> (64 - n)); skip(n); return v;
    }
};
inline unsigned rd16(const uint8_t *p, bool le) { return le ? p[0] | (p[1] << 8) : (p[0] << 8) | p[1]; }
} // namespace

struct NefMeta {
    int tree = 0;
    uint16_t vpred[2][2] = {{0, 0}, {0, 0}};
    std::vector<uint16_t> curve;
    unsigned maxv = 0, split = 0;
    bool lossy = false;
};

// Parse makernote 0x0096 (version, predictor seeds, curve) like dcraw/LibRaw.
static bool parse_meta(const uint8_t *meta, size_t meta_len, bool le, unsigned bps, NefMeta &m, std::string &err) {
    if (meta_len < 12) { err = "linearization table too short"; return false; }
    unsigned ver0 = meta[0], ver1 = meta[1];
    size_t mp = 2;
    if (ver0 == 0x49 || ver1 == 0x58) mp += 2110;
    int tree = 0;
    if (ver0 == 0x46) tree = 2;
    if (bps == 14) tree += 3;
    if (mp + 10 > meta_len) { err = "linearization table too short"; return false; }
    uint16_t vpred[2][2] = {{uint16_t(rd16(meta + mp, le)), uint16_t(rd16(meta + mp + 2, le))},
                            {uint16_t(rd16(meta + mp + 4, le)), uint16_t(rd16(meta + mp + 6, le))}};
    std::memcpy(m.vpred, vpred, sizeof vpred);
    mp += 8;
    unsigned maxv = (1u << bps) & 0x7fff;
    unsigned csize = rd16(meta + mp, le); mp += 2;
    std::vector<uint16_t> curve(0x10000);
    for (unsigned i = 0; i < curve.size(); i++) curve[i] = uint16_t(i);
    unsigned step = csize > 1 ? maxv / (csize - 1) : 0, split = 0;
    if (ver0 == 0x44 && (ver1 == 0x20 || (ver1 == 0x40 && bps == 14)) && step > 0) {
        // lossy: sampled curve, linearly interpolated
        if (ver1 == 0x40) { step /= 4; maxv /= 4; }
        if (mp + 2 * csize > meta_len || meta_len < 564) { err = "curve truncated"; return false; }
        for (unsigned i = 0; i < csize; i++) curve[i * step] = uint16_t(rd16(meta + mp + 2 * i, le));
        for (unsigned i = 0; i < maxv; i++)
            curve[i] = uint16_t((curve[i - i % step] * (step - i % step) + curve[i - i % step + step] * (i % step)) / step);
        split = rd16(meta + 562, le);
    } else if (ver0 != 0x46 && csize > 1 && csize <= 0x4001) {
        if (mp + 2 * csize > meta_len) { err = "curve truncated"; return false; }
        for (unsigned i = 0; i < csize; i++) curve[i] = uint16_t(rd16(meta + mp + 2 * i, le));
        maxv = csize;
    } else if (ver0 != 0x46) {
        err = "unsupported NEF compression variant"; return false;
    }
    while (maxv > 2 && curve[maxv - 2] == curve[maxv - 1]) maxv--;
    m.tree = tree;
    m.lossy = ver0 == 0x44;
    m.curve.assign(curve.begin(), curve.begin() + maxv);
    m.maxv = maxv;
    m.split = split;
    return true;
}

// Shared decoder. want_codes: output the raw (pre-curve) codes instead of
// linear values; the curve and the code limit are returned when requested.
static bool decode_impl(const uint8_t *strip, size_t strip_len, const uint8_t *meta, size_t meta_len, bool le,
                        unsigned width, unsigned height, unsigned bps, std::vector<uint16_t> &out, std::string &err,
                        bool want_codes, std::vector<uint16_t> *curve_out, unsigned *maxv_out, unsigned *split_out) {
    NefMeta m;
    if (!parse_meta(meta, meta_len, le, bps, m, err)) return false;
    const int tree = m.tree;
    uint16_t vpred[2][2];
    std::memcpy(vpred, m.vpred, sizeof vpred);
    unsigned maxv = m.maxv, split = m.split;
    std::vector<uint16_t> curve(0x10000);
    for (unsigned i = 0; i < curve.size(); i++) curve[i] = uint16_t(i);
    std::copy(m.curve.begin(), m.curve.end(), curve.begin());
    if (curve_out) *curve_out = m.curve;
    if (maxv_out) *maxv_out = maxv;
    if (split_out) *split_out = split;
    unsigned minv = 0;
    Huff huff0(kTrees[tree]), huff1(kTrees[std::min(tree + 1, 5)]);
    const Huff *hp = &huff0;
    BitReader br(strip, strip_len);
    out.assign(size_t(width) * height, 0);
    uint16_t hpred[2] = {0, 0};
    for (unsigned row = 0; row < height; row++) {
        uint16_t *dst = out.data() + size_t(row) * width;
        if (split && row == split) { hp = &huff1; minv = 16; maxv += minv << 1; }
        for (unsigned col = 0; col < width; col++) {
            unsigned e = hp->lut[br.peek16()];
            unsigned l = e >> 8;
            if (!l) { err = "bad huffman code"; return false; }
            br.skip(int(l));
            int i = e & 0xff;
            int len = i & 15, shl = i >> 4;
            int diff = ((int(br.get(len - shl)) << 1) + 1) << shl >> 1;
            if (len > 0 && (diff & (1 << (len - 1))) == 0) diff -= (1 << len) - !shl;
            if (col < 2) hpred[col] = vpred[row & 1][col] = uint16_t(vpred[row & 1][col] + diff);
            else hpred[col & 1] = uint16_t(hpred[col & 1] + diff);
            if (uint16_t(hpred[col & 1] + minv) >= maxv) { err = "predictor out of range"; return false; }
            int v = std::clamp<int>(int16_t(hpred[col & 1]), 0, 0x3fff);
            dst[col] = want_codes ? uint16_t(v) : curve[v];
        }
        if (br.p >= br.end && br.cnt < 0) { err = "strip truncated"; return false; }
    }
    return true;
}

bool decode_nikon_lossless(const uint8_t *strip, size_t strip_len, const uint8_t *meta, size_t meta_len, bool le,
                           unsigned width, unsigned height, unsigned bps, std::vector<uint16_t> &out, std::string &err) {
    return decode_impl(strip, strip_len, meta, meta_len, le, width, height, bps, out, err, false, nullptr, nullptr, nullptr);
}

bool decode_nikon_codes(const uint8_t *strip, size_t strip_len, const uint8_t *meta, size_t meta_len, bool le,
                        unsigned width, unsigned height, unsigned bps, std::vector<uint16_t> &codes,
                        std::vector<uint16_t> &curve, std::string &err) {
    unsigned maxv = 0, split = 0;
    if (!decode_impl(strip, strip_len, meta, meta_len, le, width, height, bps, codes, err, true, &curve, &maxv, &split))
        return false;
    if (split) { err = "split lossy NEF streams are not supported"; return false; }
    return true;
}

namespace {
struct HuffEnc {
    uint16_t code[16] = {0};
    uint8_t len[16] = {0};
    explicit HuffEnc(const uint8_t *tree) {
        const uint8_t *counts = tree, *sym = tree + 16;
        unsigned c = 0, pos = 0;
        for (unsigned l = 1; l <= 16; l++) {
            for (unsigned i = 0; i < counts[l - 1]; i++) {
                if (sym[pos] < 16 && !len[sym[pos]]) { code[sym[pos]] = uint16_t(c); len[sym[pos]] = uint8_t(l); }
                c++; pos++;
            }
            c <<= 1;
        }
    }
};
struct BitWriter {
    std::vector<uint8_t> &out;
    uint64_t acc = 0; unsigned n = 0;
    explicit BitWriter(std::vector<uint8_t> &o) : out(o) {}
    inline void put(uint32_t v, unsigned bits) {
        if (!bits) return;
        acc = (acc << bits) | (v & ((1u << bits) - 1));
        n += bits;
        while (n >= 8) { n -= 8; out.push_back(uint8_t(acc >> n)); }
    }
    void flush() { if (n) put(0xff, 8 - n); }  // camera pads with 1-bits
};
} // namespace

static bool encode_huffman(const uint16_t *px, unsigned width, unsigned height, int tree, unsigned maxv,
                           const uint16_t seeds[2][2], std::vector<uint8_t> &out, std::string &err) {
    HuffEnc h(kTrees[tree]);
    out.clear();
    out.reserve(size_t(width) * height * 3 / 2);
    BitWriter bw(out);
    int vpred[2][2] = {{seeds[0][0], seeds[0][1]}, {seeds[1][0], seeds[1][1]}};
    for (unsigned row = 0; row < height; row++) {
        const uint16_t *src = px + size_t(row) * width;
        int hpred[2] = {0, 0};
        for (unsigned col = 0; col < width; col++) {
            int v = src[col];
            if (v >= int(maxv)) { err = "sample exceeds bit depth"; return false; }
            int pred = col < 2 ? vpred[row & 1][col] : hpred[col & 1];
            int diff = v - pred;
            if (col < 2) vpred[row & 1][col] = v;
            hpred[col & 1] = v;
            unsigned a = unsigned(diff < 0 ? -diff : diff), len = 0;
            while (a >> len) len++;
            if (len > 15 || !h.len[len]) { err = "difference out of Huffman range"; return false; }
            bw.put(h.code[len], h.len[len]);
            if (len) bw.put(diff < 0 ? unsigned(diff + (1 << len) - 1) : unsigned(diff), len);
        }
    }
    bw.flush();
    return true;
}

bool encode_nikon_lossless(const uint16_t *px, unsigned width, unsigned height, unsigned bps,
                           const uint8_t *meta, size_t meta_len, bool le,
                           std::vector<uint8_t> &out, std::string &err) {
    if (meta_len < 12 || meta[0] != 0x46) { err = "encoder needs a lossless (F0) linearization table"; return false; }
    if (bps != 14 && bps != 12) { err = "unsupported bit depth"; return false; }
    uint16_t seeds[2][2] = {{uint16_t(rd16(meta + 2, le)), uint16_t(rd16(meta + 4, le))},
                            {uint16_t(rd16(meta + 6, le)), uint16_t(rd16(meta + 8, le))}};
    return encode_huffman(px, width, height, bps == 14 ? 5 : 2, 1u << bps, seeds, out, err);
}

bool nikon_lossy_curve(const uint8_t *meta, size_t meta_len, bool le, unsigned bps, std::vector<uint16_t> &curve,
                       std::string &err) {
    NefMeta m;
    if (!parse_meta(meta, meta_len, le, bps, m, err)) return false;
    if (!m.lossy) { err = "not a lossy NEF table"; return false; }
    curve = m.curve;
    return true;
}

bool encode_nikon_lossy(const uint16_t *codes, unsigned width, unsigned height, unsigned bps, const uint8_t *meta,
                        size_t meta_len, bool le, std::vector<uint8_t> &out, std::string &err) {
    NefMeta m;
    if (!parse_meta(meta, meta_len, le, bps, m, err)) return false;
    if (!m.lossy) { err = "encoder needs a lossy (0x44) linearization table"; return false; }
    if (m.split) { err = "split lossy NEF streams are not supported"; return false; }
    return encode_huffman(codes, width, height, m.tree, m.maxv, m.vpred, out, err);
}

size_t nikon_packed14_stride(unsigned width) { return (size_t(width) * 7 / 4 + 15) / 16 * 16; }

void decode_nikon_packed14(const uint8_t *src, unsigned width, unsigned height, std::vector<uint16_t> &out) {
    size_t stride = nikon_packed14_stride(width);
    out.assign(size_t(width) * height, 0);
    for (unsigned y = 0; y < height; y++) {
        const uint8_t *s = src + y * stride;
        uint16_t *d = out.data() + size_t(y) * width;
        for (unsigned x = 0; x + 3 < width; x += 4, s += 7) {
            d[x] = uint16_t(((s[1] & 0x3f) << 8) | s[0]);
            d[x + 1] = uint16_t((s[3] & 0xf) << 10 | (s[2] << 2) | (s[1] >> 6));
            d[x + 2] = uint16_t(((s[5] & 0x3) << 12) | (s[4] << 4) | (s[3] >> 4));
            d[x + 3] = uint16_t((s[6] << 6) | (s[5] >> 2));
        }
    }
}

std::vector<uint8_t> encode_nikon_packed14(const uint16_t *px, unsigned width, unsigned height) {
    size_t stride = nikon_packed14_stride(width);
    std::vector<uint8_t> out(stride * height, 0);
    for (unsigned y = 0; y < height; y++) {
        uint8_t *s = out.data() + y * stride;
        const uint16_t *d = px + size_t(y) * width;
        for (unsigned x = 0; x + 3 < width; x += 4, s += 7) {
            uint64_t v = uint64_t(d[x] & 0x3fff) | uint64_t(d[x + 1] & 0x3fff) << 14 |
                         uint64_t(d[x + 2] & 0x3fff) << 28 | uint64_t(d[x + 3] & 0x3fff) << 42;
            for (int k = 0; k < 7; k++) s[k] = uint8_t(v >> (8 * k));
        }
    }
    return out;
}

} // namespace nefcomp
