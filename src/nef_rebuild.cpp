// SPDX-License-Identifier: GPL-2.0-or-later
#include "nef_rebuild.h"
#include "he_decode.h"
#include "he_encoder.h"
#include "nikon_lossless.h"
#include "tiff.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace nefcomp {

namespace {

// Huffman/curve metadata: makernote 0x0096, or 0x008c on D1-era cameras (as LibRaw)
const Entry *nef_meta(const NefLayout &L) {
    if (auto *e = L.makernote.find(0x96)) return e;
    return L.makernote.find(0x8c);
}

size_t padded12_stride(unsigned w) { return (size_t(w) * 3 / 2 + 15) / 16 * 16; }

// ---- uncompressed layouts: decode ----
void decode_uncompressed(const uint8_t *s, size_t len, UncompLayout lay, unsigned w, unsigned h, bool file_le,
                         std::vector<uint16_t> &px) {
    px.assign(size_t(w) * h, 0);
    switch (lay) {
    case UncompLayout::Packed14: decode_nikon_packed14(s, w, h, px); return;
    case UncompLayout::PackedMSB12:
        for (size_t i = 0; i + 1 < px.size(); i += 2, s += 3) {
            px[i] = uint16_t(s[0] << 4 | s[1] >> 4);
            px[i + 1] = uint16_t((s[1] & 15) << 8 | s[2]);
        }
        return;
    case UncompLayout::PackedMSB12W32: {
        std::vector<uint8_t> t(s, s + len);
        for (size_t i = 0; i + 3 < t.size(); i += 4) { std::swap(t[i], t[i + 3]); std::swap(t[i + 1], t[i + 2]); }
        decode_uncompressed(t.data(), len, UncompLayout::PackedMSB12, w, h, file_le, px);
        return;
    }
    case UncompLayout::PackedMSB12Interlaced: {
        std::vector<uint16_t> t;
        decode_uncompressed(s, len, UncompLayout::PackedMSB12, w, h, file_le, t);
        const unsigned half = (h + 1) / 2;
        for (unsigned irow = 0; irow < h; irow++) {
            unsigned row = irow % half * 2 + irow / half;
            std::copy_n(&t[size_t(irow) * w], w, &px[size_t(row) * w]);
        }
        return;
    }
    case UncompLayout::PackedMSB12Pad:
        for (size_t i = 0; i + 1 < px.size(); i += 2, s += 3) {
            if (i % 10 == 0 && i) s++;  // pad byte after every 10 samples
            px[i] = uint16_t(s[0] << 4 | s[1] >> 4);
            px[i + 1] = uint16_t((s[1] & 15) << 8 | s[2]);
        }
        return;
    case UncompLayout::PackedLE12:
    case UncompLayout::PackedLE12Padded: {
        size_t stride = lay == UncompLayout::PackedLE12 ? size_t(w) * 3 / 2 : padded12_stride(w);
        for (unsigned y = 0; y < h; y++) {
            const uint8_t *r = s + y * stride;
            for (unsigned x = 0; x + 1 < w; x += 2, r += 3) {
                px[size_t(y) * w + x] = uint16_t(r[0] | (r[1] & 15) << 8);
                px[size_t(y) * w + x + 1] = uint16_t(r[1] >> 4 | r[2] << 4);
            }
        }
        return;
    }
    case UncompLayout::Unpacked16:
        for (size_t i = 0; i < px.size(); i++)
            px[i] = file_le ? uint16_t(s[2 * i] | s[2 * i + 1] << 8) : uint16_t(s[2 * i] << 8 | s[2 * i + 1]);
        return;
    case UncompLayout::Unpacked16Shift4:
        for (size_t i = 0; i < px.size(); i++) px[i] = uint16_t((s[2 * i] << 8 | s[2 * i + 1]) >> 2);
        return;
    default: throw std::runtime_error("unknown uncompressed layout");
    }
    (void)len;
}

// ---- uncompressed layouts: encode (inverse; padding bytes are zero) ----
std::vector<uint8_t> encode_uncompressed(const uint16_t *px, UncompLayout lay, unsigned w, unsigned h, bool file_le,
                                         size_t len) {
    std::vector<uint8_t> out(len, 0);
    uint8_t *s = out.data();
    const size_t n = size_t(w) * h;
    switch (lay) {
    case UncompLayout::Packed14: return encode_nikon_packed14(px, w, h);
    case UncompLayout::PackedMSB12Interlaced: {
        std::vector<uint16_t> t(n);
        const unsigned half = (h + 1) / 2;
        for (unsigned irow = 0; irow < h; irow++) {
            unsigned row = irow % half * 2 + irow / half;
            std::copy_n(&px[size_t(row) * w], w, &t[size_t(irow) * w]);
        }
        out = encode_uncompressed(t.data(), UncompLayout::PackedMSB12, w, h, file_le, len);
        break;
    }
    case UncompLayout::PackedMSB12W32:
        out = encode_uncompressed(px, UncompLayout::PackedMSB12, w, h, file_le, len);
        for (size_t i = 0; i + 3 < out.size(); i += 4) { std::swap(out[i], out[i + 3]); std::swap(out[i + 1], out[i + 2]); }
        break;
    case UncompLayout::PackedMSB12:
    case UncompLayout::PackedMSB12Pad:
        for (size_t i = 0; i + 1 < n; i += 2, s += 3) {
            if (lay == UncompLayout::PackedMSB12Pad && i % 10 == 0 && i) s++;
            s[0] = uint8_t(px[i] >> 4);
            s[1] = uint8_t((px[i] & 15) << 4 | (px[i + 1] >> 8 & 15));
            s[2] = uint8_t(px[i + 1]);
        }
        break;
    case UncompLayout::PackedLE12:
    case UncompLayout::PackedLE12Padded: {
        size_t stride = lay == UncompLayout::PackedLE12 ? size_t(w) * 3 / 2 : padded12_stride(w);
        for (unsigned y = 0; y < h; y++) {
            uint8_t *r = s + y * stride;
            for (unsigned x = 0; x + 1 < w; x += 2, r += 3) {
                uint16_t a = px[size_t(y) * w + x], b = px[size_t(y) * w + x + 1];
                r[0] = uint8_t(a);
                r[1] = uint8_t((a >> 8 & 15) | (b & 15) << 4);
                r[2] = uint8_t(b >> 4);
            }
        }
        break;
    }
    case UncompLayout::Unpacked16:
        for (size_t i = 0; i < n; i++) {
            if (file_le) { s[2 * i] = uint8_t(px[i]); s[2 * i + 1] = uint8_t(px[i] >> 8); }
            else { s[2 * i] = uint8_t(px[i] >> 8); s[2 * i + 1] = uint8_t(px[i]); }
        }
        break;
    case UncompLayout::Unpacked16Shift4:
        for (size_t i = 0; i < n; i++) { uint16_t v = uint16_t(px[i] << 2); s[2 * i] = uint8_t(v >> 8); s[2 * i + 1] = uint8_t(v); }
        break;
    default: throw std::runtime_error("unknown uncompressed layout");
    }
    return out;
}

// Nearest code for every 14-bit value (curve must be non-decreasing).
std::vector<uint16_t> curve_inverse(const std::vector<uint16_t> &curve) {
    std::vector<uint16_t> inv(16384);
    for (int v = 0; v < 16384; v++) {
        auto it = std::lower_bound(curve.begin(), curve.end(), uint16_t(v));
        size_t k = it == curve.end() ? curve.size() - 1 : size_t(it - curve.begin());
        if (k && std::abs(int(curve[k - 1]) - v) <= std::abs(int(curve[k]) - v)) {
            size_t j = k - 1;  // prefer the lower code on ties; step back over equal values
            while (j && curve[j - 1] == curve[j]) j--;
            if (std::abs(int(curve[j]) - v) <= std::abs(int(curve[k]) - v)) k = j;
        }
        inv[v] = uint16_t(k);
    }
    return inv;
}

inline uint16_t curve_value(const std::vector<uint16_t> &curve, uint16_t code) {
    int c = std::clamp<int>(int16_t(code), 0, 0x3fff);  // codes can be slightly negative after a split
    return c < int(curve.size()) ? curve[c] : curve.back();
}

} // namespace

NefRaw describe_raw(const std::vector<uint8_t> &file, bool check_strip) {
    Buf b;
    b.d = file;
    NefLayout L = parse_nef(b, check_strip);
    NefRaw r;
    r.width = L.width; r.height = L.height; r.bps = L.bps;
    r.strip_off = L.strip_off; r.strip_len = L.strip_len; r.strip_count = L.strip_count;
    unsigned code = 0;
    if (L.have_makernote) {
        code = nef_compression_code(b, L);
        if (auto *e = L.makernote.find(0x3d); e && e->count >= 1) r.black = b.uint_val(L.makernote, *e, 0);
    }
    if (auto *cfa = L.raw_ifd.find(0x828e); cfa && cfa->type == 1 && cfa->count <= 16)
        for (unsigned i = 0; i < cfa->count; i++)
            if (b.d[cfa->value_pos + i] > 2) { r.why = "colour filter is not RGB (CMYG sensor)"; return r; }
    if (code == 8) { r.why = "small-size NEF (no raw mosaic)"; return r; }
    if ((L.bps != 14 && L.bps != 12 && L.bps != 16) || (L.width & 1)) return r;
    const size_t n = size_t(L.width) * L.height;
    if (L.compression == 1) {  // TIFF uncompressed (LibRaw: 12-bit packed, 14/16-bit unpacked)
        // some Coolpix pad the packed strip to a multiple of 4096 bytes (P6000, P7000, P7100)
        const size_t packed = n * 3 / 2, padded4k = (packed + 4095) / 4096 * 4096;
        if (L.bps == 12 && (L.strip_len == packed || (L.strip_len == padded4k && L.strip_len - packed < 4096))) {
            std::string model;
            if (auto *e = L.ifd0.find(0x110); e && e->type == 2)
                model.assign(b.d.begin() + e->value_pos, b.d.begin() + e->value_pos + e->count), model = model.c_str();
            // Coolpix NRW bodies pack into 32-bit words (LibRaw load_flags 24; P1000, P950,
            // B700, P6000-P7100); E-series Coolpix interlace the rows (load_flags 2)
            const bool w32 = model.rfind("COOLPIX", 0) == 0 && L.width != 4032 && L.strip_len % 4 == 0;
            const bool interlaced = model.size() == 5 && model[0] == 'E' && std::isdigit(uint8_t(model[1]));
            r.layout = w32 ? UncompLayout::PackedMSB12W32
                           : interlaced ? UncompLayout::PackedMSB12Interlaced : UncompLayout::PackedMSB12;
        }
        else if (L.strip_len == n * 2) r.layout = UncompLayout::Unpacked16;  // 14/16-bit; 12-bit on the A1000
        if (r.layout != UncompLayout::None) r.format = RawFormat::Uncompressed;
        return r;
    }
    if (L.compression != 34713 || L.bps == 16) return r;
    auto *m96 = nef_meta(L);
    if (code == 13) r.format = RawFormat::HE;
    else if (code == 14) r.format = RawFormat::HEStar;
    else if (code == 3 && m96) r.format = RawFormat::Lossless;
    else if ((code == 1 || code == 4) && m96 && m96->size() > 2 && b.d[m96->value_pos] == 0x44) r.format = RawFormat::Lossy;
    else if (L.bps == 14 && L.strip_len == nikon_packed14_stride(L.width) * L.height) r.layout = UncompLayout::Packed14;
    else if (L.bps == 12 && L.strip_len == n * 3 / 2) {
        // LibRaw: Nikon bodies pack LSB-first pairs; Coolpix compacts MSB-first
        std::string model;
        if (auto *e = L.ifd0.find(0x110); e && e->type == 2)
            model.assign(b.d.begin() + e->value_pos, b.d.begin() + e->value_pos + e->count), model = model.c_str();
        r.layout = model.rfind("COOLPIX", 0) == 0 ? UncompLayout::PackedMSB12 : UncompLayout::PackedLE12;
    }
    else if (L.bps == 12 && L.strip_len == padded12_stride(L.width) * L.height) r.layout = UncompLayout::PackedLE12Padded;
    else if (L.strip_len == n * 2) { r.layout = UncompLayout::Unpacked16Shift4; r.bps = 14; }
    else if (L.bps == 12 && L.height && L.strip_len % (size_t(L.height) * 16) == 0) {
        unsigned rw = unsigned(L.strip_len / L.height / 16 * 10);  // D100 (LibRaw: packed, load_flags 1)
        if (rw >= L.width && rw < L.width + 10) { r.layout = UncompLayout::PackedMSB12Pad; r.width = rw; }
    }
    if (r.layout != UncompLayout::None) r.format = RawFormat::Uncompressed;
    if (L.bps != 14 && (r.format == RawFormat::HE || r.format == RawFormat::HEStar)) r.format = RawFormat::Unsupported;
    return r;
}

std::vector<uint16_t> decode_raw(const std::vector<uint8_t> &file, NefRaw *info) {
    NefRaw r = describe_raw(file);
    if (info) *info = r;
    Buf b;
    b.d = file;
    NefLayout L = parse_nef(b);
    std::vector<uint16_t> px;
    std::string err;
    const uint8_t *strip = &b.d[L.strip_off];
    switch (r.format) {
    case RawFormat::Lossless:
    case RawFormat::Lossy: {
        auto *m = nef_meta(L);
        if (!decode_nikon_lossless(strip, L.strip_len, &b.d[m->value_pos], m->size(), L.makernote.le, L.width,
                                   L.height, L.bps, px, err))
            throw std::runtime_error(std::string(r.format == RawFormat::Lossy ? "lossy" : "lossless") + " NEF decode: " + err);
        break;
    }
    case RawFormat::Uncompressed: decode_uncompressed(strip, L.strip_len, r.layout, r.width, L.height, L.le, px); break;
    case RawFormat::HE:
    case RawFormat::HEStar:
        if (!he_decode(strip, L.strip_len, L.width, L.height, px, err)) throw std::runtime_error("HE decode: " + err);
        break;
    default: throw std::runtime_error("unsupported raw format");
    }
    return px;
}

void decode_raw_codes(const std::vector<uint8_t> &file, std::vector<uint16_t> &codes, std::vector<uint16_t> &curve) {
    NefRaw r = describe_raw(file);
    if (r.format != RawFormat::Lossy) throw std::runtime_error("not a lossy NEF");
    Buf b;
    b.d = file;
    NefLayout L = parse_nef(b);
    auto *m = nef_meta(L);
    std::string err;
    if (!decode_nikon_codes(&b.d[L.strip_off], L.strip_len, &b.d[m->value_pos], m->size(), L.makernote.le, L.width,
                            L.height, L.bps, codes, curve, err))
        throw std::runtime_error("lossy NEF decode: " + err);
}

std::vector<CodeOverride> lossy_code_overrides(const std::vector<uint16_t> &codes, const std::vector<uint16_t> &curve) {
    auto inv = curve_inverse(curve);
    std::vector<CodeOverride> out;
    for (size_t i = 0; i < codes.size(); i++) {
        uint16_t c = codes[i];
        if (inv[std::min<uint16_t>(curve_value(curve, c), 16383)] != c) out.push_back({uint32_t(i), c});
    }
    return out;
}

std::vector<uint8_t> nef_skeleton(const std::vector<uint8_t> &file) {
    NefRaw r = describe_raw(file);
    if (r.format == RawFormat::Unsupported) throw std::runtime_error("unsupported raw format");
    if (r.strip_off + r.strip_len == file.size())
        return std::vector<uint8_t>(file.begin(), file.begin() + r.strip_off);
    std::vector<uint8_t> s = file;  // data follows the strip: keep everything, zero the strip
    std::fill(s.begin() + r.strip_off, s.begin() + r.strip_off + r.strip_len, 0);
    return s;
}

std::vector<uint8_t> nef_assemble(const std::vector<uint8_t> &skeleton, const std::vector<uint16_t> &px_in,
                                  const std::vector<uint16_t> *lossy_curve, const std::vector<CodeOverride> *overrides) {
    NefRaw r = describe_raw(skeleton, false);
    const bool slot = r.strip_off + r.strip_len <= skeleton.size() && r.strip_off < skeleton.size();
    if (!slot && r.strip_off != skeleton.size()) throw std::runtime_error("skeleton does not match its raw strip offset");
    if (px_in.size() != size_t(r.width) * r.height) throw std::runtime_error("pixel count does not match the skeleton");
    // values beyond the bit depth can come out of lossy JPEG XL (lossy NEFs map
    // values through their curve, which may use the 14-bit range)
    std::vector<uint16_t> px = px_in;
    const uint16_t vmax = uint16_t(r.format == RawFormat::Lossy ? 16383 : (1u << r.bps) - 1);
    for (auto &v : px) v = std::min(v, vmax);
    Buf b;
    b.d = skeleton;
    NefLayout L = parse_nef(b, false);
    std::vector<uint8_t> strip;
    std::vector<uint16_t> lossy_codes;
    std::string err;
    switch (r.format) {
    case RawFormat::Lossless: {
        auto *m = nef_meta(L);
        if (!encode_nikon_lossless(px.data(), r.width, r.height, r.bps, &b.d[m->value_pos], m->size(), L.makernote.le,
                                   strip, err))
            throw std::runtime_error("lossless encode: " + err);
        break;
    }
    case RawFormat::Lossy: {
        auto *m = nef_meta(L);
        std::vector<uint16_t> curve;
        if (lossy_curve && !lossy_curve->empty()) curve = *lossy_curve;
        else if (!nikon_lossy_curve(&b.d[m->value_pos], m->size(), L.makernote.le, r.bps, curve, err))
            throw std::runtime_error("lossy NEF curve: " + err);
        auto inv = curve_inverse(curve);
        lossy_codes.resize(px.size());
        for (size_t i = 0; i < px.size(); i++) lossy_codes[i] = inv[std::min<uint16_t>(px[i], 16383)];
        if (lossy_codes.empty() || curve.empty()) throw std::runtime_error("empty lossy curve");
        if (overrides)
            for (const auto &o : *overrides)
                if (o.index < lossy_codes.size()) lossy_codes[o.index] = o.code;
        // the encoder may adjust codes after a split row to values its tables can represent
        if (!encode_nikon_lossy(lossy_codes, r.width, r.height, r.bps, &b.d[m->value_pos], m->size(),
                                L.makernote.le, strip, err))
            throw std::runtime_error("lossy NEF encode: " + err);
        break;
    }
    case RawFormat::Uncompressed:
        strip = encode_uncompressed(px.data(), r.layout, r.width, r.height, L.le, r.strip_len);
        if (strip.size() != r.strip_len) throw std::runtime_error("internal: uncompressed size mismatch");
        break;
    case RawFormat::HE:
    case RawFormat::HEStar: {
        HeEncodeOptions opt;
        opt.mode = r.format == RawFormat::HE ? HeMode::HE : HeMode::HEStar;
        opt.black = r.black ? r.black : 1008;
        strip = he_encode(px.data(), r.width, r.height, opt);
        if (strip.size() != r.strip_len) throw std::runtime_error("HE size differs from skeleton");
        break;
    }
    default: throw std::runtime_error("unsupported raw format in skeleton");
    }

    auto *so = L.raw_ifd.find(0x111), *sc = L.raw_ifd.find(0x117);
    if (strip.size() != r.strip_len && r.strip_count != 1) throw std::runtime_error("cannot resize a multi-strip raw");
    if (slot && strip.size() <= r.strip_len) {
        // original slot (data follows it, or same size): write in place
        std::memcpy(&b.d[r.strip_off], strip.data(), strip.size());
        std::fill(b.d.begin() + r.strip_off + strip.size(), b.d.begin() + r.strip_off + r.strip_len, 0);
        if (strip.size() != r.strip_len) b.w32(sc->value_pos, uint32_t(strip.size()), L.raw_ifd.le);
    } else {
        if (slot) {  // grown: leave the zeroed slot, append at the end
            while (b.d.size() & 15) b.d.push_back(0);
            b.w32(so->value_pos, uint32_t(b.d.size()), L.raw_ifd.le);
        }
        if (strip.size() != r.strip_len) b.w32(sc->value_pos, uint32_t(strip.size()), L.raw_ifd.le);
        b.d.insert(b.d.end(), strip.begin(), strip.end());
    }

    // verify
    if (r.format == RawFormat::Lossy) {
        std::vector<uint16_t> codes2, curve2;
        decode_raw_codes(b.d, codes2, curve2);
        if (codes2 != lossy_codes) throw std::runtime_error("internal: rebuilt lossy strip does not decode to the same codes");
    } else {
        std::vector<uint16_t> back = decode_raw(b.d);
        if ((r.format == RawFormat::Lossless || r.format == RawFormat::Uncompressed) && back != px)
            throw std::runtime_error("internal: rebuilt strip does not decode to the same pixels");
    }
    return std::move(b.d);
}

} // namespace nefcomp
