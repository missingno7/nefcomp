// SPDX-License-Identifier: GPL-2.0-or-later
#include "nef_rebuild.h"
#include "he_decode.h"
#include "he_encoder.h"
#include "nikon_lossless.h"
#include "tiff.h"

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
    case RawFormat::Packed14: decode_nikon_packed14(strip, L.width, L.height, px); break;
    case RawFormat::HE:
    case RawFormat::HEStar:
        if (!he_decode(strip, L.strip_len, L.width, L.height, px, err)) throw std::runtime_error("HE decode: " + err);
        break;
    default: throw std::runtime_error("unsupported raw format (lossy NEF or unknown)");
    }
    return px;
}

std::vector<uint8_t> nef_skeleton(const std::vector<uint8_t> &file) {
    NefRaw r = describe_raw(file);
    if (r.strip_off + r.strip_len != file.size()) throw std::runtime_error("raw strip is not at the end of the file");
    if (r.format == RawFormat::Unsupported) throw std::runtime_error("unsupported raw format");
    return std::vector<uint8_t>(file.begin(), file.begin() + r.strip_off);
}

std::vector<uint8_t> nef_assemble(const std::vector<uint8_t> &skeleton, const std::vector<uint16_t> &px) {
    NefRaw r = describe_raw(skeleton, false);
    if (r.strip_off != skeleton.size()) throw std::runtime_error("skeleton does not end at the raw strip offset");
    if (px.size() != size_t(r.width) * r.height) throw std::runtime_error("pixel count does not match the skeleton");
    Buf b;
    b.d = skeleton;
    NefLayout L = parse_nef(b, false);
    std::vector<uint8_t> strip;
    std::string err;
    switch (r.format) {
    case RawFormat::Lossless: {
        auto *m = L.makernote.find(0x96);
        if (!encode_nikon_lossless(px.data(), r.width, r.height, r.bps, &b.d[m->value_pos], m->size(), L.makernote.le,
                                   strip, err))
            throw std::runtime_error("lossless encode: " + err);
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
    }
    return std::move(b.d);
}

} // namespace nefcomp
