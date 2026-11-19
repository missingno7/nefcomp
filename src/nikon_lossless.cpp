#include "nikon_lossless.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
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
    // curve (LibRaw nikon_read_curve)
    unsigned table_codes = 0, split = 0;
    unsigned cmax = (1u << bps) & 0x7fff, step = cmax;
    unsigned csize = rd16(meta + mp, le); mp += 2;
    if (csize > 1) step = cmax / (csize - 1);
    std::vector<uint16_t> curve(0x10000);
    for (unsigned i = 0; i < curve.size(); i++) curve[i] = uint16_t(i);
    if (ver0 == 0x44 && (ver1 == 0x20 || (ver1 == 0x40 && step > 3)) && step > 0) {
        // lossy: sampled curve, linearly interpolated
        if (ver1 == 0x40) { step /= 4; cmax /= 4; }
        if (mp + 2 * csize > meta_len) { err = "curve truncated"; return false; }
        for (unsigned i = 0; i < csize; i++) curve[i * step] = uint16_t(rd16(meta + mp + 2 * i, le));
        for (unsigned i = 0; i < cmax; i++)
            curve[i] = uint16_t((curve[i - i % step] * (step - i % step) + curve[i - i % step + step] * (i % step)) / step);
        if (meta_len >= 564) split = rd16(meta + 562, le);
    } else if (ver0 != 0x46 && csize <= 0x4001) {
        if (mp + 2 * csize > meta_len) { err = "curve truncated"; return false; }
        for (unsigned i = 0; i < csize; i++) curve[i] = uint16_t(rd16(meta + mp + 2 * i, le));
        table_codes = csize;  // valid codes are those the table defines
    }
    // code range and split row (LibRaw nikon_load_raw)
    unsigned maxv = table_codes ? table_codes : cmax;
    while (maxv > 2 && curve[maxv - 2] == curve[maxv - 1]) maxv--;
    m.tree = tree;
    m.lossy = ver0 == 0x44;
    // curve over every code the stream can hold (after a split row: up to maxv + 15)
    const unsigned domain = split ? std::min(maxv + 16, 0x4000u) : maxv;
    m.curve.assign(curve.begin(), curve.begin() + domain);
    for (unsigned i = maxv; i < domain; i++) m.curve[i] = curve[std::min(i, 0x3fffu)];
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
    const unsigned maxv = m.maxv, split = m.split;
    const std::vector<uint16_t> &curve = m.curve;
    if (curve_out) *curve_out = m.curve;
    if (maxv_out) *maxv_out = maxv;
    if (split_out) *split_out = split;
    Huff huff0(kTrees[tree]), huff1(kTrees[std::min(tree + 1, 5)]);
    const Huff *hp = &huff0;
    bool after_split = false;
    BitReader br(strip, strip_len);
    out.assign(size_t(width) * height, 0);
    uint16_t hpred[2] = {0, 0};
    for (unsigned row = 0; row < height; row++) {
        uint16_t *dst = out.data() + size_t(row) * width;
        if (split && row == split) { hp = &huff1; after_split = true; }
        for (unsigned col = 0; col < width; col++) {
            unsigned e = hp->lut[br.peek16()];
            unsigned l = e >> 8;
            if (!l) { err = "bad huffman code"; return false; }
            br.skip(int(l));
            int i = e & 0xff;
            int len = i & 15, shl = i >> 4;
            int diff = ((int(br.get(len - shl)) << 1) + 1) << shl >> 1;
            if (len > 0 && (diff & (1 << (len - 1))) == 0) diff -= (1 << len) - !shl;
            int v = int(col < 2 ? vpred[row & 1][col] : hpred[col & 1]) + diff;
            if (after_split) {
                // coarse symbols can overshoot: like Adobe, a value below zero is
                // stored (and predicts the next one) as zero
                v = std::max(v, 0);
                if (v > 0x7fff) { err = "predictor out of range"; return false; }
            } else if (v < 0 || unsigned(v) >= maxv) {
                err = "predictor out of range";
                return false;
            }
            if (col < 2) vpred[row & 1][col] = uint16_t(v);
            hpred[col & 1] = uint16_t(v);
            dst[col] = want_codes ? uint16_t(v) : curve[std::min<size_t>(unsigned(v), curve.size() - 1)];
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
    return decode_impl(strip, strip_len, meta, meta_len, le, width, height, bps, codes, err, true, &curve, &maxv, &split);
}

namespace {
// Every difference a Huffman tree can express, with its complete bit pattern
// (Huffman code + extra bits). A symbol is (shl << 4) | len: `len` is the
// magnitude class, and `shl` low bits are implied, so only some differences
// of that class exist (the trees used after a lossy "split" row).
struct HuffEnc {
    static constexpr int kOff = 1 << 16;
    std::vector<uint32_t> bits;
    std::vector<uint8_t> nbits;  // 0: not representable
    std::vector<int> diffs;      // sorted representable differences
    explicit HuffEnc(const uint8_t *tree) : bits(2 * kOff, 0), nbits(2 * kOff, 0) {
        const uint8_t *counts = tree, *sym = tree + 16;
        unsigned c = 0, pos = 0;
        bool seen[16] = {false};
        for (unsigned l = 1; l <= 16; l++) {
            for (unsigned i = 0; i < counts[l - 1]; i++, c++, pos++) {
                int len = sym[pos] & 15, shl = sym[pos] >> 4;
                if (seen[len] || shl > len) continue;  // the decoder's first symbol of a class wins
                seen[len] = true;
                for (unsigned x = 0; x < (1u << (len - shl)) || (len == shl && x == 0); x++) {
                    int diff = ((int(x) << 1) + 1) << shl >> 1;  // same arithmetic as the decoder
                    if (len > 0 && (diff & (1 << (len - 1))) == 0) diff -= (1 << len) - !shl;
                    if (len == 0) diff = 0;
                    bits[diff + kOff] = (c << (len - shl)) | x;
                    nbits[diff + kOff] = uint8_t(l + len - shl);
                    diffs.push_back(diff);
                    if (len == shl) break;
                }
            }
            c <<= 1;
        }
        std::sort(diffs.begin(), diffs.end());
    }
    bool exact(int d) const { return d > -kOff && d < kOff && nbits[d + kOff]; }
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

// px: samples (lossless) or int16 codes (lossy). `lossy`: out-of-range values
// are clamped and differences the tree cannot express are quantized to the
// nearest representable one (closed loop, like the camera); the codes that
// result are written back to px. Lossless input must be exactly representable.
static bool encode_huffman(uint16_t *px, unsigned width, unsigned height, int tree, unsigned maxv, unsigned split,
                           const uint16_t seeds[2][2], bool lossy, std::vector<uint8_t> &out, std::string &err) {
    HuffEnc h0(kTrees[tree]), h1(kTrees[std::min(tree + 1, 5)]);
    const HuffEnc *h = &h0;
    out.clear();
    out.reserve(size_t(width) * height * 3 / 2);
    BitWriter bw(out);
    int vpred[2][2] = {{int16_t(seeds[0][0]), int16_t(seeds[0][1])}, {int16_t(seeds[1][0]), int16_t(seeds[1][1])}};
    bool after_split = false;
    for (unsigned row = 0; row < height; row++) {
        uint16_t *src = px + size_t(row) * width;
        if (split && row == split) { h = &h1; after_split = true; }
        // stored values: [0, maxv) before the split; after it a value is
        // max(0, prediction + difference) and may exceed maxv slightly
        const int lo = 0, hi = after_split ? 0x3fff : int(maxv) - 1;
        int hpred[2] = {0, 0};
        for (unsigned col = 0; col < width; col++) {
            int v = lossy ? int(int16_t(src[col])) : int(src[col]);
            if (v < lo || v > hi) {
                if (!lossy) { err = "sample exceeds bit depth"; return false; }
                v = std::clamp(v, lo, hi);
            }
            const int pred = col < 2 ? vpred[row & 1][col] : hpred[col & 1];
            int diff = v - pred;
            if (!h->exact(diff) || (!after_split && pred + diff != v)) {
                if (!lossy) { err = "difference out of Huffman range"; return false; }
                // nearest representable difference (the camera quantizes the same way)
                auto stored = [&](int k) { return std::max(pred + k, 0); };
                auto better = [&](int a, int b) {  // a closer than b?
                    int ea = std::abs(stored(a) - v), eb = std::abs(stored(b) - v);
                    return ea != eb ? ea < eb : std::abs(pred + a - v) < std::abs(pred + b - v);
                };
                const auto it = std::lower_bound(h->diffs.begin(), h->diffs.end(), diff);
                bool found = false;
                int best = 0;
                for (auto k = it; k != h->diffs.end() && stored(*k) <= hi; ++k)
                    if (pred + *k >= lo || after_split) { best = *k; found = true; break; }
                for (auto k = it; k != h->diffs.begin();) {
                    --k;
                    if (pred + *k < lo && !after_split) break;
                    if (stored(*k) <= hi) {
                        if (!found || better(*k, best)) best = *k;
                        found = true;
                        break;
                    }
                }
                if (!found) { err = "no representable difference"; return false; }
                diff = best;
            }
            v = after_split ? std::max(pred + diff, 0) : pred + diff;
            if (col < 2) vpred[row & 1][col] = v;
            hpred[col & 1] = v;
            if (lossy) src[col] = uint16_t(v);
            bw.put(h->bits[diff + HuffEnc::kOff], h->nbits[diff + HuffEnc::kOff]);
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
    return encode_huffman(const_cast<uint16_t *>(px), width, height, bps == 14 ? 5 : 2, 1u << bps, 0, seeds, false,
                          out, err);  // lossless: px is not modified
}

bool nikon_lossy_curve(const uint8_t *meta, size_t meta_len, bool le, unsigned bps, std::vector<uint16_t> &curve,
                       std::string &err) {
    NefMeta m;
    if (!parse_meta(meta, meta_len, le, bps, m, err)) return false;
    if (!m.lossy) { err = "not a lossy NEF table"; return false; }
    curve = m.curve;
    return true;
}

bool encode_nikon_lossy(std::vector<uint16_t> &codes, unsigned width, unsigned height, unsigned bps, const uint8_t *meta,
                        size_t meta_len, bool le, std::vector<uint8_t> &out, std::string &err) {
    NefMeta m;
    if (!parse_meta(meta, meta_len, le, bps, m, err)) return false;
    if (!m.lossy) { err = "encoder needs a lossy (0x44) linearization table"; return false; }
    if (codes.size() != size_t(width) * height) { err = "code count mismatch"; return false; }
    return encode_huffman(codes.data(), width, height, m.tree, m.maxv, m.split, m.vpred, true, out, err);
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
