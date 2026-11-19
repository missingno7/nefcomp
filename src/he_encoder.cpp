// SPDX-License-Identifier: GPL-2.0-or-later
//
// Nikon High Efficiency (HE / HE*) RAW codestream encoder.
//
// The transform, entropy-coding and packet syntax are adapted from the
// experimental forward model in WonderLantern/RdWing "z8-qemu"
// (qemu/nikon-he-forward.inc, GPL-2.0-or-later), which was written as the
// counterpart of the public LibRaw-PR-826 HE inverse. This file adds:
//   * the picture header exactly as written by a Nikon Z 8 (C3.11 / FW 4.x),
//     with tone parameters derived from the black level;
//   * camera-style constant-bit-rate packetisation: every precinct gets the
//     camera's byte budget, and (Q, R) are chosen per precinct (JPEG XS
//     quantisation + refinement) to fit it, then zero padded;
//   * slice-parallel encoding.
//
// Codestream layout (JPEG XS / ISO 21122 style, Nikon "TicoRAW" profile):
//   SOC, CAP("CONTACT_INTOPIX_"), PIH (39 bytes incl. vendor tone ext.),
//   CDT, WGT (25 gain/priority pairs), SLH, precincts..., SLH every 16
//   precincts, EOC.  One precinct = 4 sensor rows; 4 components after a
//   Star-Tetrix-like CFA transform; 1 vertical + 5 horizontal 5/3 levels.

#include "he_encoder.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

namespace nefcomp {

namespace {

using V = std::vector<int32_t>;
using Bytes = std::vector<uint8_t>;
using Bands = std::array<V, 26>;
using History = std::array<std::vector<uint8_t>, 26>;

constexpr unsigned kSliceHeight = 16;          // precincts per slice (PIH Hsl)
constexpr unsigned kWgtBands = 25;
constexpr unsigned kLbBounds[9] = {0, 6, 12, 13, 19, 21, 23, 24, 26};
constexpr unsigned kScale[15] = {87381, 74898, 69905, 67650, 66576, 66052, 65793, 65664,
                                 65600, 65568, 65552, 65544, 65540, 65538, 65537};

// Gain / priority pairs of the WGT marker written by the Z 8.
constexpr uint8_t kCameraWgt[kWgtBands][2] = {
    {3, 2},  {3, 19}, {2, 9},  {2, 22}, {1, 10}, {1, 21}, {2, 5},  {1, 3},  {1, 17},
    {0, 1},  {0, 7},  {0, 13}, {0, 11}, {2, 6},  {1, 4},  {1, 16}, {0, 0},  {0, 8},
    {0, 14}, {1, 20}, {0, 18}, {0, 15}, {0, 23}, {0, 12}, {0, 24}};

inline int32_t floor_div(int64_t v, unsigned shift) {
    return v >= 0 ? int32_t(v >> shift) : -int32_t(((-v) + (INT64_C(1) << shift) - 1) >> shift);
}
void put(Bytes &b, uint64_t v, unsigned n) {
    for (unsigned i = n; i; i--) b.push_back(uint8_t(v >> ((i - 1) * 8)));
}
void store(Bytes &b, size_t at, uint64_t v, unsigned n) {
    if (at + n > b.size()) throw std::runtime_error("header store out of range");
    for (unsigned i = 0; i < n; i++) b[at + i] = uint8_t(v >> ((n - i - 1) * 8));
}
void marker(Bytes &b, unsigned tag, const Bytes &body) {
    put(b, tag, 2);
    put(b, body.size() + 2, 2);
    b.insert(b.end(), body.begin(), body.end());
}

struct Tone { unsigned A, B, C; };

Tone tone_for_black(unsigned black) {
    if (black == 1008) return {84429, 1008, 16255};   // exact Z 8 values
    if (black == 0 || black >= 8192) throw std::runtime_error("unsupported black level");
    double c = std::floor(std::sqrt(double(black) * 262144.0));
    double a = std::round(65536.0 * std::sqrt((16384.0 - black) * 262144.0) / (65535.0 - c));
    return {unsigned(a), black, unsigned(c)};
}

// Picture header (everything up to and including the first SLH), as written by the camera.
Bytes camera_header(unsigned w, unsigned h, uint32_t lcod, const Tone &t, size_t &pih_pos) {
    Bytes out;
    put(out, 0xff10, 2);
    const char sig[] = "CONTACT_INTOPIX_";
    Bytes cap(sig, sig + 16);
    put(cap, 0xefc0, 2);
    cap.resize(32, 0);
    marker(out, 0xff50, cap);
    pih_pos = out.size();
    Bytes pih;
    put(pih, lcod, 4);
    put(pih, 0, 2);            // Ppih
    put(pih, 0, 2);            // Plev
    put(pih, w, 2);
    put(pih, h, 2);
    put(pih, 0, 2);            // Cw (full width precincts)
    put(pih, kSliceHeight, 2); // Hsl
    for (unsigned v : {4u, 4u, 8u, 18u, 0x44u, 0x10u, 0x51u, 0x14u}) put(pih, v, 1);
    for (unsigned v : {0x50u, 0x88u, 0x70u, 0x80u | (t.B >> 8), t.B & 255u, 0x15u, 0x23u,
                       0xd0u | (t.A >> 16), (t.A >> 8) & 255u, t.A & 255u, t.C >> 8, t.C & 255u, 7u})
        put(pih, v, 1);
    marker(out, 0xff12, pih);
    Bytes cdt;
    for (unsigned c = 0; c < 4; c++) { put(cdt, 14, 1); put(cdt, 0x11, 1); put(cdt, c == 2 ? 0 : 0x51, 1); }
    marker(out, 0xff13, cdt);
    Bytes wgt;
    for (auto &gp : kCameraWgt) { put(wgt, gp[0], 1); put(wgt, gp[1], 1); }
    marker(out, 0xff14, wgt);
    marker(out, 0xff20, Bytes{0, 0});
    return out;
}

struct Bits {
    Bytes b;
    uint64_t pending = 0;
    unsigned count = 0;
    void write(uint32_t value, unsigned n) {
        pending = (pending << n) | (value & ((UINT64_C(1) << n) - 1));
        count += n;
        while (count >= 8) { count -= 8; b.push_back(uint8_t(pending >> count)); }
    }
    void end() { if (count) write(0, 8 - count); }
    void clear() { b.clear(); pending = 0; count = 0; }
};

std::array<unsigned, 6> group_counts(unsigned w) {
    unsigned g = (w + 7) / 8, n = (g + 1) / 2, l = (n + 1) / 2;
    return {(l + 3) / 4, (n - l + 3) / 4, (g + 7) / 8, (g + 3) / 4, (g + 1) / 2, g};
}

// Whole-sample symmetric, reversible 5/3 predict then update.
void forward53(const V &in, V &low, V &high) {
    low.assign((in.size() + 1) / 2, 0);
    high.assign(in.size() / 2, 0);
    const size_t last_even = in.size() - 1 - (in.size() % 2 == 0);
    for (size_t i = 0; i < high.size(); i++)
        high[i] = in[2 * i + 1] - floor_div(int64_t(in[2 * i]) + in[std::min(2 * i + 2, last_even)], 1);
    for (size_t i = 0; i < low.size(); i++) {
        int32_t left = high.empty() ? 0 : high[i ? i - 1 : 0];
        int32_t right = high.empty() ? 0 : high[std::min(i, high.size() - 1)];
        low[i] = in[2 * i] + floor_div(int64_t(left) + right + 2, 2);
    }
}

// Returns [LL_n, H_n, ..., H_1].
std::vector<V> horizontal(V line, unsigned levels) {
    std::vector<V> high;
    V lo, hi;
    for (unsigned k = 0; k < levels; k++) {
        forward53(line, lo, hi);
        line.swap(lo);
        high.push_back(hi);
    }
    std::vector<V> result;
    result.push_back(std::move(line));
    for (auto i = high.rbegin(); i != high.rend(); ++i) result.push_back(std::move(*i));
    return result;
}

// CFA -> four half-resolution component planes in the companded code domain.
std::array<V, 4> mosaic_transform(const uint16_t *raw, unsigned w, unsigned h, const Tone &t) {
    std::vector<int32_t> transfer(65536);
    for (unsigned i = 0; i < 65536; i++) {
        int64_t d = int64_t(i) - t.C, q;
        if (d < 0) q = -d * d;
        else { int64_t z = (d * t.A + 32768) >> 16; q = z * z; }
        transfer[i] = int32_t(std::clamp<int64_t>(int64_t(t.B) + floor_div(q + 163840, 18), 0, 16383));
    }
    std::array<int32_t, 16384> inverse{};
    for (unsigned r = 0; r < 16384; r++) {
        auto it = std::lower_bound(transfer.begin(), transfer.end(), int(r));
        size_t k = it == transfer.end() ? 65535 : size_t(it - transfer.begin());
        if (k && std::abs(transfer[k - 1] - int(r)) < std::abs(transfer[k] - int(r))) --k;
        inverse[r] = int(k) - 32768;
    }
    const unsigned cols = w / 2, rows = h / 2;
    const size_t n = size_t(cols) * rows;
    V gu(n), gd(n), ll(n), hl(n), pred(n), hh(n), lh(n);
    auto sample = [&](unsigned x, unsigned y) { return inverse[std::min<unsigned>(raw[size_t(y) * w + x], 16383)]; };
    auto at = [&](unsigned x, unsigned y) { return size_t(y) * cols + x; };
    for (unsigned y = 0; y < rows; y++)
        for (unsigned x = 0; x < cols; x++) {
            gu[at(x, y)] = sample(2 * x + 1, 2 * y);
            gd[at(x, y)] = sample(2 * x, 2 * y + 1);
        }
    for (unsigned y = 0; y < rows; y++)
        for (unsigned x = 0; x < cols; x++) {
            unsigned l = x ? x - 1 : 0, r = std::min(x + 1, cols - 1), up = y ? y - 1 : 0, dn = std::min(y + 1, rows - 1);
            size_t i = at(x, y);
            ll[i] = sample(2 * x, 2 * y) - floor_div(int64_t(gd[at(x, up)]) + gu[i] + gd[i] + gu[at(l, y)], 2);
            hl[i] = sample(2 * x + 1, 2 * y + 1) - floor_div(int64_t(gd[at(r, y)]) + gd[i] + gu[i] + gu[at(x, dn)], 2);
        }
    for (unsigned y = 0; y < rows; y++)
        for (unsigned x = 0; x < cols; x++) {
            unsigned r = std::min(x + 1, cols - 1), up = y ? y - 1 : 0;
            size_t i = at(x, y);
            pred[i] = gu[i] + floor_div(int64_t(ll[i]) + hl[i] + ll[at(r, y)] + hl[at(x, up)], 3);
        }
    for (unsigned y = 0; y < rows; y++)
        for (unsigned x = 0; x < cols; x++) {
            unsigned l = x ? x - 1 : 0, dn = std::min(y + 1, rows - 1);
            size_t i = at(x, y);
            hh[i] = gd[i] + floor_div(int64_t(ll[i]) + hl[i] + hl[at(l, y)] + ll[at(x, dn)], 3) -
                    floor_div(int64_t(pred[at(l, y)]) + pred[i] + pred[at(l, dn)] + pred[at(x, dn)], 2);
        }
    for (unsigned y = 0; y < rows; y++)
        for (unsigned x = 0; x < cols; x++) {
            unsigned r = std::min(x + 1, cols - 1), up = y ? y - 1 : 0;
            size_t i = at(x, y);
            lh[i] = pred[i] + floor_div(int64_t(hh[i]) + hh[at(r, y)] + hh[at(x, up)] + hh[at(r, up)], 3);
        }
    return {std::move(lh), std::move(ll), std::move(hh), std::move(hl)};
}

// Wavelet bands of one precinct (two component-plane rows).
Bands precinct_bands(const std::array<V, 4> &planes, unsigned cols, unsigned rows, unsigned p) {
    Bands result;
    const unsigned n = cols;
    const auto ng = group_counts(cols);
    const unsigned offsets[4] = {0, 6, 12, 13}, b_offsets[4] = {19, 21, 23, 24};
    const unsigned even = p * 2, odd = std::min(even + 1, rows - 1), next = even + 2 < rows ? even + 2 : even;
    V low(n), high(n);
    for (unsigned c = 0; c < 4; c++) {
        const V &pl = planes[c];
        if (c == 2) {  // G-difference component bypasses the wavelet
            result[12] = V(pl.begin() + size_t(even) * cols, pl.begin() + size_t(even) * cols + n);
            result[23] = V(pl.begin() + size_t(odd) * cols, pl.begin() + size_t(odd) * cols + n);
            result[12].resize((n + 3) / 4 * 4);
            result[23].resize((n + 3) / 4 * 4);
            continue;
        }
        for (unsigned x = 0; x < n; x++) {
            auto v = [&](unsigned y) { return pl[size_t(y) * cols + x]; };
            unsigned previous = even ? even - 1 : 1;
            int32_t prev_h = rows < 2 ? 0 : v(previous) - floor_div(int64_t(v(even ? even - 2 : 0)) + v(even), 1);
            int32_t current_h = even + 1 < rows ? v(odd) - floor_div(int64_t(v(even)) + v(next), 1) : prev_h;
            if (!even) prev_h = current_h;
            high[x] = current_h;
            low[x] = v(even) + floor_div(int64_t(prev_h) + current_h + 2, 2);
        }
        auto a = horizontal(low, 5), b = horizontal(high, 1);
        for (unsigned j = 0; j < 6; j++) { a[j].resize(ng[j] * 4); result[offsets[c] + j] = std::move(a[j]); }
        for (unsigned j = 0; j < 2; j++) { b[j].resize(((cols + 7) / 8) * 4); result[b_offsets[c] + j] = std::move(b[j]); }
    }
    return result;
}

unsigned bit_width(unsigned v) { unsigned n = 0; while (v) { v >>= 1; ++n; } return n; }

struct BandScratch {
    std::vector<std::array<uint32_t, 4>> codes;
};

void encode_band(const V &v, unsigned gtli, const std::vector<uint8_t> &previous, std::vector<uint8_t> &target,
                 Bits &sig, Bits &gcli, Bits &data, Bits &sign, BandScratch &scratch) {
    if (v.size() % 4) throw std::runtime_error("coefficient group shape");
    auto &codes = scratch.codes;
    codes.resize(v.size() / 4);
    target.resize(v.size() / 4);
    if (previous.size() != target.size()) throw std::runtime_error("prediction group shape");
    for (size_t i = 0; i < v.size(); i += 4) {
        uint32_t magnitude[4] = {0, 0, 0, 0};
        unsigned maximum = 0;
        for (unsigned k = 0; k < 4; k++) maximum = std::max(maximum, unsigned(std::abs(v[i + k])));
        unsigned g = std::max(gtli, bit_width((maximum + 8) / 16));
        if (g > 15) throw std::runtime_error("coefficient precision overflow");
        for (unsigned iteration = 0; iteration < 4; iteration++) {
            unsigned bc = g > gtli ? g - gtli : 0, max_code = bc ? ((1u << bc) - 1) : 0, observed = 0;
            uint64_t denominator = bc ? (uint64_t(kScale[bc - 1]) * 16 << gtli) : 1;
            for (unsigned k = 0; k < 4; k++) {
                uint64_t t = uint64_t(std::abs(v[i + k])) * 65536;
                magnitude[k] = bc ? uint32_t(std::min<uint64_t>(max_code, (t + denominator / 2) / denominator)) : 0;
                observed = std::max(observed, magnitude[k]);
            }
            unsigned actual = observed ? bit_width(observed) + gtli : gtli;
            if (actual == g) break;
            g = actual;
        }
        target[i / 4] = uint8_t(g);
        for (unsigned k = 0; k < 4; k++) codes[i / 4][k] = magnitude[k];
    }
    for (size_t base = 0; base < target.size(); base += 8) {
        size_t n = std::min(size_t(8), target.size() - base);
        bool baseline = true;
        for (size_t i = 0; i < n; i++)
            if (target[base + i] != std::max(gtli, unsigned(previous[base + i]))) baseline = false;
        sig.write(baseline, 1);
        if (!baseline)
            for (size_t i = 0; i < n; i++) {
                unsigned g = target[base + i], m = std::max(gtli, unsigned(previous[base + i])), threshold = m - gtli, u;
                if (g == m) u = 0;
                else if (g < m) u = 2 * (m - g) - 1;
                else if (g - m <= threshold) u = 2 * (g - m);
                else u = g - gtli;
                if (u > 31) throw std::runtime_error("prediction unary bound");
                // unary code: u ones followed by a zero (u <= 31)
                if (u > 16) { gcli.write(0xffff, 16); u -= 16; }
                gcli.write(((1u << u) - 1) << 1, u + 1);
            }
    }
    for (size_t group = 0; group < codes.size(); group++) {
        unsigned bc = target[group] - gtli;
        const auto &magnitude = codes[group];
        for (unsigned plane = bc; plane; plane--) {
            unsigned nibble = 0;
            for (unsigned k = 0; k < 4; k++) nibble = (nibble << 1) | ((magnitude[k] >> (plane - 1)) & 1);
            data.write(nibble, 4);
        }
        for (unsigned k = 0; k < 4; k++)
            if (magnitude[k]) sign.write(v[group * 4 + k] < 0, 1);
    }
}

inline unsigned wgt_index(unsigned sb) { return sb < 23 ? sb : sb == 23 ? 12 : sb - 1; }

struct PrecinctCoder {
    unsigned width;
    Bits sig, gcli, data, sign;
    BandScratch scratch;

    // Encode one precinct with quantisation Q and refinement R. `history`
    // holds the previous precinct's GCLIs and is updated. Returns the packet
    // (unpadded; Lprc not yet stored).
    Bytes encode(const Bands &bands, unsigned Q, unsigned R, History &history) {
        Bytes result(12, 0xff);
        result[3] = uint8_t(Q);
        result[4] = uint8_t(R);
        result[11] = 0xc0;  // 25 x Dpb=3 (vertical prediction), 2-bit padding
        const unsigned sig_bytes = (width / 2 + 255) / 256;
        std::vector<uint8_t> target;
        for (unsigned lb = 0; lb < 8; lb++) {
            sig.clear(); gcli.clear(); data.clear(); sign.clear();
            for (unsigned sb = kLbBounds[lb]; sb < kLbBounds[lb + 1]; sb++) {
                unsigned prev_band = sb == 12 ? 23 : sb == 23 ? 12 : sb;
                if (history[prev_band].empty()) history[prev_band].assign(bands[sb].size() / 4, 0);
                const auto &gp = kCameraWgt[wgt_index(sb)];
                int t = int(Q) - gp[0] - (gp[1] < R ? 1 : 0);
                unsigned gtli = unsigned(std::clamp(t, 0, 15));
                encode_band(bands[sb], gtli, history[prev_band], target, sig, gcli, data, sign, scratch);
                history[sb] = target;
            }
            sig.end(); gcli.end(); data.end(); sign.end();
            if (sig.b.size() > sig_bytes) throw std::runtime_error("significance layout exceeds typed width");
            sig.b.resize(sig_bytes, 0);
            if (data.b.size() >= 1u << 20 || gcli.b.size() >= 1u << 20 || sign.b.size() >= 1u << 15)
                throw std::runtime_error("substream length overflow");
            uint64_t mini = (uint64_t(data.b.size()) << 35) | (uint64_t(gcli.b.size()) << 15) | sign.b.size();
            put(result, mini, 7);
            result.insert(result.end(), sig.b.begin(), sig.b.end());
            result.insert(result.end(), gcli.b.begin(), gcli.b.end());
            result.insert(result.end(), data.b.begin(), data.b.end());
            result.insert(result.end(), sign.b.begin(), sign.b.end());
        }
        return result;
    }
};

constexpr unsigned kMaxQuality = 16 * 25;  // quality index s = Q*25 + (24-R)
inline void quality_to_qr(unsigned s, unsigned &Q, unsigned &R) { Q = s / 25; R = 24 - s % 25; }

} // namespace

uint32_t he_codestream_size(unsigned width, unsigned height, HeMode mode) {
    uint64_t bpp = mode == HeMode::HEStar ? 5 : 3;
    uint64_t bytes = uint64_t(width) * height * bpp / 8;
    return uint32_t(bytes / 512 * 512);
}

std::vector<uint8_t> he_encode(const uint16_t *cfa, unsigned w, unsigned h, const HeEncodeOptions &opt,
                               HeEncodeStats *stats) {
    if (w < 64 || h < 8 || ((w | h) & 3) || w > 16384 || h > 16384)
        throw std::runtime_error("unsupported raw geometry " + std::to_string(w) + "x" + std::to_string(h));
    const Tone tone = tone_for_black(opt.black);
    const uint32_t lcod = he_codestream_size(w, h, opt.mode);
    size_t pih_pos = 0;
    Bytes header = camera_header(w, h, lcod, tone, pih_pos);

    const unsigned cols = w / 2, rows = h / 2;
    const unsigned n_prec = (rows + 1) / 2;
    const unsigned n_slices = (n_prec + kSliceHeight - 1) / kSliceHeight;
    const size_t overhead = header.size() + size_t(n_slices - 1) * 6 + 2;
    if (lcod <= overhead) throw std::runtime_error("codestream budget too small");
    const uint64_t payload = lcod - overhead;
    auto budget = [&](unsigned k) {
        return size_t((uint64_t(k + 1) * payload) / n_prec - (uint64_t(k) * payload) / n_prec);
    };

    const auto planes = mosaic_transform(cfa, w, h, tone);

    std::vector<Bytes> packets(n_prec);
    std::vector<unsigned> chosen(n_prec, 0);
    std::vector<size_t> used(n_prec, 0);
    std::atomic<unsigned> next_slice{0};
    std::mutex err_mutex;
    std::string error;

    auto worker = [&]() {
        PrecinctCoder coder{w, {}, {}, {}, {}, {}};
        try {
            for (;;) {
                unsigned s = next_slice.fetch_add(1);
                if (s >= n_slices) break;
                History history;
                unsigned guess = 3 * 25;  // start near the camera's typical choice
                for (unsigned p = s * kSliceHeight; p < std::min(n_prec, (s + 1) * kSliceHeight); p++) {
                    const Bands bands = precinct_bands(planes, cols, rows, p);
                    const size_t limit = budget(p);
                    // Find the best (lowest) quality index whose packet fits,
                    // assuming packet size is non-increasing in the index.
                    Bytes best_pkt;
                    History best_hist;
                    unsigned best = kMaxQuality;
                    auto eval = [&](unsigned q) {
                        unsigned Q, R;
                        quality_to_qr(q, Q, R);
                        History th = history;
                        Bytes b = coder.encode(bands, Q, R, th);
                        bool ok = b.size() + opt.min_pad <= limit;
                        if (ok && q < best) { best = q; best_pkt = std::move(b); best_hist = std::move(th); }
                        return ok;
                    };
                    unsigned lo, hi;  // the answer lies in [lo, hi]; hi fits
                    if (eval(guess)) {
                        hi = guess; lo = 0;
                        for (unsigned step = 1; hi > 0; step *= 2) {
                            unsigned cand = hi > step ? hi - step : 0;
                            if (eval(cand)) hi = cand;
                            else { lo = cand + 1; break; }
                        }
                    } else {
                        lo = guess + 1;
                        for (unsigned step = 1;; step *= 2) {
                            unsigned cand = std::min(kMaxQuality - 1, guess + step);
                            if (eval(cand)) { hi = cand; break; }
                            lo = cand + 1;
                            if (cand == kMaxQuality - 1) throw std::runtime_error("precinct cannot meet the rate budget");
                        }
                    }
                    while (lo < hi) {
                        unsigned mid = (lo + hi) / 2;
                        if (eval(mid)) hi = mid; else lo = mid + 1;
                    }
                    history = std::move(best_hist);
                    used[p] = best_pkt.size();
                    best_pkt.resize(limit, 0);
                    store(best_pkt, 0, best_pkt.size() - 12, 3);
                    packets[p] = std::move(best_pkt);
                    chosen[p] = best;
                    guess = best;
                }
            }
        } catch (const std::exception &e) {
            std::lock_guard<std::mutex> g(err_mutex);
            if (error.empty()) error = e.what();
            next_slice = n_slices;
        }
    };
    unsigned nt = opt.threads ? opt.threads : std::max(1u, std::thread::hardware_concurrency());
    nt = std::min(nt, n_slices);
    std::vector<std::thread> pool;
    for (unsigned i = 1; i < nt; i++) pool.emplace_back(worker);
    worker();
    for (auto &t : pool) t.join();
    if (!error.empty()) throw std::runtime_error("HE encode failed: " + error);

    Bytes out = header;
    out.reserve(lcod);
    for (unsigned p = 0; p < n_prec; p++) {
        out.insert(out.end(), packets[p].begin(), packets[p].end());
        if ((p % kSliceHeight) == kSliceHeight - 1 && p + 1 < n_prec) {
            put(out, 0xff200004, 4);
            put(out, (p + 1) / kSliceHeight, 2);
        }
    }
    put(out, 0xff11, 2);
    if (out.size() != lcod) throw std::runtime_error("internal: codestream size mismatch");

    if (stats) {
        *stats = HeEncodeStats{};
        stats->precincts = n_prec;
        double sum = 0, pads = 0;
        stats->min_pad = SIZE_MAX;
        for (unsigned p = 0; p < n_prec; p++) {
            sum += chosen[p];
            stats->q_histogram[chosen[p] / 25]++;
            size_t pad = packets[p].size() - used[p];
            pads += double(pad);
            stats->min_pad = std::min(stats->min_pad, pad);
            stats->max_pad = std::max(stats->max_pad, pad);
        }
        stats->mean_quality_index = sum / n_prec;
        stats->mean_pad = pads / n_prec;
    }
    return out;
}

} // namespace nefcomp
