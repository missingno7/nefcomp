// SPDX-License-Identifier: GPL-2.0-or-later
#include "nef_rebuild.h"
#include "he_decode.h"
#include "he_encoder.h"
#include "nikon_lossless.h"
#include "tiff.h"

#include <algorithm>
#include <cstdlib>
#include <stdexcept>

namespace nefcomp {

NefRaw describe_raw(const std::vector<uint8_t> &file, bool check_strip) {
    Buf b;
    b.d = file;
    NefLayout L = parse_nef(b, check_strip);
    NefRaw r;
    r.width = L.width; r.height = L.height; r.bps = L.bps;
    r.strip_off = L.strip_off; r.strip_len = L.strip_len;
    unsigned code = 0;
    if (L.have_makernote) {
        if (auto *e = L.makernote.find(0x51); e && e->size() > 10) code = b.d[e->value_pos + 10];
        if (auto *e = L.makernote.find(0x3d); e && e->count >= 1) r.black = b.uint_val(L.makernote, *e, 0);
    }
    if (L.compression != 34713 || L.bps != 14) return r;
    if (code == 13) r.format = RawFormat::HE;
    else if (code == 14) r.format = RawFormat::HEStar;
    else if (code == 3 && L.makernote.find(0x96)) r.format = RawFormat::Lossless;
    else if ((code == 1 || code == 4) && L.makernote.find(0x96) && L.makernote.find(0x96)->size() > 2 &&
             b.d[L.makernote.find(0x96)->value_pos] == 0x44)
        r.format = RawFormat::Lossy;
    else if (L.strip_len == nikon_packed14_stride(L.width) * L.height) r.format = RawFormat::Packed14;
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
    case RawFormat::Lossless: {
        auto *m = L.makernote.find(0x96);
        if (!decode_nikon_lossless(strip, L.strip_len, &b.d[m->value_pos], m->size(), L.makernote.le, L.width,
                                   L.height, L.bps, px, err))
            throw std::runtime_error("lossless decode: " + err);
        break;
    }
    case RawFormat::Lossy: {
        auto *m = L.makernote.find(0x96);
        if (!decode_nikon_lossless(strip, L.strip_len, &b.d[m->value_pos], m->size(), L.makernote.le, L.width,
                                   L.height, L.bps, px, err))
            throw std::runtime_error("lossy NEF decode: " + err);
        break;
    }
    case RawFormat::Packed14: decode_nikon_packed14(strip, L.width, L.height, px); break;
    case RawFormat::HE:
    case RawFormat::HEStar:
        if (!he_decode(strip, L.strip_len, L.width, L.height, px, err)) throw std::runtime_error("HE decode: " + err);
        break;
    default: throw std::runtime_error("unsupported raw format (lossy NEF or unknown)");
    }
    return px;
}

void decode_raw_codes(const std::vector<uint8_t> &file, std::vector<uint16_t> &codes, std::vector<uint16_t> &curve) {
    NefRaw r = describe_raw(file);
    if (r.format != RawFormat::Lossy) throw std::runtime_error("not a lossy NEF");
    Buf b;
    b.d = file;
    NefLayout L = parse_nef(b);
    auto *m = L.makernote.find(0x96);
    std::string err;
    if (!decode_nikon_codes(&b.d[L.strip_off], L.strip_len, &b.d[m->value_pos], m->size(), L.makernote.le, L.width,
                            L.height, L.bps, codes, curve, err))
        throw std::runtime_error("lossy NEF decode: " + err);
}

// Nearest code for every 14-bit value (curve must be non-decreasing).
static std::vector<uint16_t> curve_inverse(const std::vector<uint16_t> &curve) {
    std::vector<uint16_t> inv(16384);
    for (int v = 0; v < 16384; v++) {
        auto it = std::lower_bound(curve.begin(), curve.end(), uint16_t(v));
        size_t k = it == curve.end() ? curve.size() - 1 : size_t(it - curve.begin());
        if (k && std::abs(int(curve[k - 1]) - v) <= std::abs(int(curve[k]) - v)) {
            // prefer the lower code on ties; step back over equal values
            size_t j = k - 1;
            while (j && curve[j - 1] == curve[j]) j--;
            if (std::abs(int(curve[j]) - v) <= std::abs(int(curve[k]) - v)) k = j;
        }
        inv[v] = uint16_t(k);
    }
    return inv;
}

std::vector<uint8_t> nef_skeleton(const std::vector<uint8_t> &file) {
    NefRaw r = describe_raw(file);
    if (r.strip_off + r.strip_len != file.size()) throw std::runtime_error("raw strip is not at the end of the file");
    if (r.format == RawFormat::Unsupported) throw std::runtime_error("unsupported raw format");
    return std::vector<uint8_t>(file.begin(), file.begin() + r.strip_off);
}

std::vector<uint8_t> nef_assemble(const std::vector<uint8_t> &skeleton, const std::vector<uint16_t> &px,
                                  const std::vector<uint16_t> *lossy_curve) {
    NefRaw r = describe_raw(skeleton, false);
    if (r.strip_off != skeleton.size()) throw std::runtime_error("skeleton does not end at the raw strip offset");
    if (px.size() != size_t(r.width) * r.height) throw std::runtime_error("pixel count does not match the skeleton");
    Buf b;
    b.d = skeleton;
    NefLayout L = parse_nef(b, false);
    std::vector<uint8_t> strip;
    std::vector<uint16_t> lossy_codes;
    std::string err;
    switch (r.format) {
    case RawFormat::Lossless: {
        auto *m = L.makernote.find(0x96);
        if (!encode_nikon_lossless(px.data(), r.width, r.height, r.bps, &b.d[m->value_pos], m->size(), L.makernote.le,
                                   strip, err))
            throw std::runtime_error("lossless encode: " + err);
        break;
    }
    case RawFormat::Lossy: {
        auto *m = L.makernote.find(0x96);
        std::vector<uint16_t> curve;
        if (lossy_curve && !lossy_curve->empty()) curve = *lossy_curve;
        else if (!nikon_lossy_curve(&b.d[m->value_pos], m->size(), L.makernote.le, r.bps, curve, err))
            throw std::runtime_error("lossy NEF curve: " + err);
        auto inv = curve_inverse(curve);
        lossy_codes.resize(px.size());
        for (size_t i = 0; i < px.size(); i++) lossy_codes[i] = inv[std::min<uint16_t>(px[i], 16383)];
        if (!encode_nikon_lossy(lossy_codes.data(), r.width, r.height, r.bps, &b.d[m->value_pos], m->size(),
                                L.makernote.le, strip, err))
            throw std::runtime_error("lossy NEF encode: " + err);
        break;
    }
    case RawFormat::Packed14: strip = encode_nikon_packed14(px.data(), r.width, r.height); break;
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
    auto *sc = L.raw_ifd.find(0x117);
    b.w32(sc->value_pos, uint32_t(strip.size()), L.raw_ifd.le);
    b.d.insert(b.d.end(), strip.begin(), strip.end());

    // verify
    std::vector<uint16_t> back = decode_raw(b.d);
    if (r.format == RawFormat::Lossless || r.format == RawFormat::Packed14) {
        if (back != px) throw std::runtime_error("internal: rebuilt strip does not decode to the same pixels");
    } else if (r.format == RawFormat::Lossy) {
        std::vector<uint16_t> codes2, curve2;
        decode_raw_codes(b.d, codes2, curve2);
        if (codes2 != lossy_codes) throw std::runtime_error("internal: rebuilt lossy strip does not decode to the same codes");
    }
    return std::move(b.d);
}

} // namespace nefcomp
