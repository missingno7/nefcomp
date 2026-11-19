// SPDX-License-Identifier: GPL-2.0-or-later
//
// Embedded NEF skeleton, format v2 (v1 = plain zlib of the skeleton is still read).
//
//   header: "NEFCOMP-SKEL" u32 version=2, u64 original size, sha256(original file),
//           sha256(raw samples LE), u64 skeleton length, u64 residual length,
//           u32 record count, residual (brotli), records
//   record: u8 kind, u64 offset, u64 length, u32 w, u32 h, u32 a, u32 b,
//           u64 count field position, u8 flags (1 = little-endian, 2 = has count),
//           u64 payload length, payload
//
// The residual is the skeleton with every region described by a record zeroed.
// Record kinds:
//   1 JPEG preview, regenerated from the raw on restore (fits the original slot)
//   2 uncompressed RGB thumbnail, regenerated
//   3 makernote, copied back from the DNG's identical copy (DNGPrivateData)
//   4 12-bit packed image (Nikon helper images), lossless JPEG XL of its planes
//   5 6-bit packed CFA image (Z 6 helper image), lossless JPEG XL of its planes
//   6 JPEG preview kept: lossless JPEG recompression (bit-exact)

#include "nef_embed.h"
#include "hash.h"
#include "jxl_util.h"
#include "nef_rebuild.h"
#include "tiff.h"

#include <zlib.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace nefcomp {

namespace {

constexpr char kMagic[12] = {'N', 'E', 'F', 'C', 'O', 'M', 'P', '-', 'S', 'K', 'E', 'L'};
enum Kind : uint8_t { kJpegRegen = 1, kRgbRegen = 2, kMakernote = 3, kPack12 = 4, kPack6 = 5, kJpegKept = 6 };

struct Record {
    uint8_t kind = 0;
    uint64_t off = 0, len = 0;
    uint32_t w = 0, h = 0, a = 0, b = 0;
    uint64_t count_pos = 0;
    uint8_t flags = 0;
    std::vector<uint8_t> payload;
};

struct Writer {
    std::vector<uint8_t> v;
    void u8(uint8_t x) { v.push_back(x); }
    void u32(uint32_t x) { for (int i = 0; i < 4; i++) v.push_back(uint8_t(x >> (8 * i))); }
    void u64(uint64_t x) { for (int i = 0; i < 8; i++) v.push_back(uint8_t(x >> (8 * i))); }
    void bytes(const uint8_t *p, size_t n) { v.insert(v.end(), p, p + n); }
};
struct Reader {
    const uint8_t *p, *end;
    void need(size_t n) { if (size_t(end - p) < n) throw std::runtime_error("truncated embedded NEF data"); }
    uint8_t u8() { need(1); return *p++; }
    uint32_t u32() { need(4); uint32_t x = 0; for (int i = 0; i < 4; i++) x |= uint32_t(p[i]) << (8 * i); p += 4; return x; }
    uint64_t u64() { need(8); uint64_t x = 0; for (int i = 0; i < 8; i++) x |= uint64_t(p[i]) << (8 * i); p += 8; return x; }
    const uint8_t *take(size_t n) { need(n); const uint8_t *r = p; p += n; return r; }
};

std::vector<uint8_t> zdecompress(const uint8_t *p, size_t n) {
    z_stream s{};
    if (inflateInit(&s) != Z_OK) throw std::runtime_error("zlib init failed");
    std::vector<uint8_t> out(std::max<size_t>(n * 2, 1 << 20));
    s.next_in = const_cast<Bytef *>(p);
    s.avail_in = uInt(n);
    int rc;
    do {
        if (s.total_out == out.size()) {
            if (out.size() > (512u << 20)) { inflateEnd(&s); throw std::runtime_error("skeleton too large"); }
            out.resize(out.size() * 2);
        }
        s.next_out = out.data() + s.total_out;
        s.avail_out = uInt(out.size() - s.total_out);
        rc = inflate(&s, Z_NO_FLUSH);
    } while (rc == Z_OK);
    inflateEnd(&s);
    if (rc != Z_STREAM_END) throw std::runtime_error("corrupt embedded skeleton");
    out.resize(s.total_out);
    return out;
}

std::array<uint8_t, 32> sha_px(const std::vector<uint16_t> &px) {
    std::vector<uint8_t> le(px.size() * 2);
    for (size_t i = 0; i < px.size(); i++) { le[2 * i] = uint8_t(px[i]); le[2 * i + 1] = uint8_t(px[i] >> 8); }
    return sha256(le.data(), le.size());
}

bool jpeg_dims(const uint8_t *j, size_t n, unsigned &w, unsigned &h) {
    if (n < 4 || j[0] != 0xff || j[1] != 0xd8) return false;
    size_t i = 2;
    while (i + 9 < n && j[i] == 0xff) {
        uint8_t m = j[i + 1];
        unsigned L = unsigned(j[i + 2]) << 8 | j[i + 3];
        if (m >= 0xc0 && m <= 0xc3) { h = unsigned(j[i + 5]) << 8 | j[i + 6]; w = unsigned(j[i + 7]) << 8 | j[i + 8]; return true; }
        if (m == 0xda) return false;
        i += 2 + L;
    }
    return false;
}

// ---- 12-bit packing (LE: v0 = b0 | (b1&15)<<8, v1 = b1>>4 | b2<<4) ----
std::vector<uint16_t> unpack12(const uint8_t *d, unsigned w, unsigned h, unsigned spp, unsigned stride) {
    std::vector<uint16_t> planes(size_t(w) * h * spp);
    for (unsigned y = 0; y < h; y++) {
        const uint8_t *r = d + size_t(y) * stride;
        for (unsigned i = 0; i < w * spp; i += 2, r += 3) {
            uint16_t v0 = uint16_t(r[0] | (r[1] & 15) << 8), v1 = uint16_t(r[1] >> 4 | r[2] << 4);
            unsigned c0 = i % spp, x0 = i / spp, c1 = (i + 1) % spp, x1 = (i + 1) / spp;
            planes[(size_t(c0) * h + y) * w + x0] = v0;
            planes[(size_t(c1) * h + y) * w + x1] = v1;
        }
    }
    return planes;
}
void pack12(const std::vector<uint16_t> &planes, unsigned w, unsigned h, unsigned spp, unsigned stride, uint8_t *d) {
    for (unsigned y = 0; y < h; y++) {
        uint8_t *r = d + size_t(y) * stride;
        for (unsigned i = 0; i < w * spp; i += 2, r += 3) {
            unsigned c0 = i % spp, x0 = i / spp, c1 = (i + 1) % spp, x1 = (i + 1) / spp;
            uint16_t v0 = planes[(size_t(c0) * h + y) * w + x0], v1 = planes[(size_t(c1) * h + y) * w + x1];
            r[0] = uint8_t(v0);
            r[1] = uint8_t((v0 >> 8 & 15) | (v1 & 15) << 4);
            r[2] = uint8_t(v1 >> 4);
        }
    }
}
// ---- 6-bit packing (4 samples in 3 bytes, LE), stored as 2x2 CFA planes ----
std::vector<uint8_t> unpack6(const uint8_t *d, unsigned w, unsigned h) {
    std::vector<uint8_t> st(size_t(w) * h);
    const unsigned h2 = h / 2, w2 = w / 2;
    for (unsigned y = 0; y < h; y++) {
        const uint8_t *r = d + size_t(y) * (w * 6 / 8);
        for (unsigned x = 0; x < w; x += 4, r += 3) {
            uint32_t word = r[0] | r[1] << 8 | r[2] << 16;
            for (unsigned k = 0; k < 4; k++) {
                unsigned xx = x + k;
                st[size_t((y & 1) * h2 + y / 2) * w + (xx & 1) * w2 + xx / 2] = uint8_t(word >> (6 * k) & 63);
            }
        }
    }
    return st;
}
void pack6(const std::vector<uint8_t> &st, unsigned w, unsigned h, uint8_t *d) {
    const unsigned h2 = h / 2, w2 = w / 2;
    for (unsigned y = 0; y < h; y++) {
        uint8_t *r = d + size_t(y) * (w * 6 / 8);
        for (unsigned x = 0; x < w; x += 4, r += 3) {
            uint32_t word = 0;
            for (unsigned k = 0; k < 4; k++) {
                unsigned xx = x + k;
                word |= uint32_t(st[size_t((y & 1) * h2 + y / 2) * w + (xx & 1) * w2 + xx / 2] & 63) << (6 * k);
            }
            r[0] = uint8_t(word); r[1] = uint8_t(word >> 8); r[2] = uint8_t(word >> 16);
        }
    }
}

void write_count(std::vector<uint8_t> &d, const Record &r, uint32_t v) {
    if (!(r.flags & 2) || r.count_pos + 4 > d.size()) return;
    bool le = r.flags & 1;
    for (int i = 0; i < 4; i++) d[r.count_pos + i] = uint8_t(v >> (le ? 8 * i : 24 - 8 * i));
}

// Place a w x h picture into a slot of the same aspect (letterbox if needed).
std::vector<uint8_t> render_fitted(const std::vector<uint16_t> &px, unsigned W, unsigned H, const RenderParams &rp,
                                   unsigned ow, unsigned oh) {
    double cw = rp.crop_w ? rp.crop_w : W, ch = rp.crop_h ? rp.crop_h : H;
    double a_img = cw / ch, a_out = double(ow) / oh;
    if (std::abs(a_img - a_out) / a_out < 0.02) return render_preview(px.data(), W, H, rp, ow, oh);
    unsigned iw = ow, ih = oh;
    if (a_img > a_out) ih = std::max(1u, unsigned(std::lround(ow / a_img)));
    else iw = std::max(1u, unsigned(std::lround(oh * a_img)));
    auto img = render_preview(px.data(), W, H, rp, iw, ih);
    std::vector<uint8_t> out(size_t(ow) * oh * 3, 0);
    unsigned ox = (ow - iw) / 2, oy = (oh - ih) / 2;
    for (unsigned y = 0; y < ih; y++) std::memcpy(&out[(size_t(y + oy) * ow + ox) * 3], &img[size_t(y) * iw * 3], size_t(iw) * 3);
    return out;
}

} // namespace

std::vector<uint8_t> dng_private_makernote(const std::vector<uint8_t> &pd) {
    if (pd.size() < 20 || std::memcmp(pd.data(), "Adobe\0", 6) != 0) return {};
    size_t i = 6;
    while (i + 8 <= pd.size()) {
        uint32_t cnt = uint32_t(pd[i + 4]) << 24 | uint32_t(pd[i + 5]) << 16 | uint32_t(pd[i + 6]) << 8 | pd[i + 7];
        if (std::memcmp(&pd[i], "MakN", 4) == 0) {
            if (cnt < 6 || i + 8 + cnt > pd.size()) return {};
            return std::vector<uint8_t>(pd.begin() + i + 8 + 6, pd.begin() + i + 8 + cnt);  // skip byte order + offset
        }
        i += 8 + cnt;
    }
    return {};
}

std::vector<uint8_t> embed_build(const std::vector<uint8_t> &nef, const std::vector<uint16_t> &px,
                                 const std::vector<uint8_t> &dng_mn, bool keep_previews, EmbedStats *st) {
    EmbedStats s;
    Buf b;
    b.d = nef;
    NefLayout L = parse_nef(b);
    const size_t skel_len = L.strip_off;
    s.skeleton = skel_len;
    std::vector<uint8_t> resid(nef.begin(), nef.begin() + skel_len);
    std::vector<Record> recs;
    auto zero = [&](size_t off, size_t len) { std::memset(&resid[off], 0, len); };

    // makernote: the DNG already holds an identical copy
    if (auto *mn = L.exif.find(0x927c); mn && !dng_mn.empty() && mn->value_pos + mn->size() <= skel_len &&
                                         dng_mn.size() == mn->size() &&
                                         std::memcmp(&nef[mn->value_pos], dng_mn.data(), dng_mn.size()) == 0) {
        Record r;
        r.kind = kMakernote; r.off = mn->value_pos; r.len = mn->size();
        recs.push_back(r);
        zero(r.off, r.len);
        s.makernote_shared = true;
    }

    // image blocks in IFD0 and the SubIFDs (except the raw strip)
    std::vector<Ifd> ifds{L.ifd0};
    if (auto *e = L.ifd0.find(0x14a))
        for (unsigned i = 0; i < e->count; i++) ifds.push_back(b.read_ifd(b.uint_val(L.ifd0, *e, i), 0, L.le));
    for (const Ifd &ifd : ifds) {
        auto get = [&](uint16_t t, uint32_t def = 0) { auto *e = ifd.find(t); return e ? b.uint_val(ifd, *e) : def; };
        const Entry *eo = nullptr, *ec = nullptr;
        bool jpeg_tags = false;
        if (ifd.find(0x201) && ifd.find(0x202)) { eo = ifd.find(0x201); ec = ifd.find(0x202); jpeg_tags = true; }
        else if (ifd.find(0x111) && ifd.find(0x117) && ifd.find(0x111)->count == 1) { eo = ifd.find(0x111); ec = ifd.find(0x117); }
        if (!eo) continue;
        size_t off = b.uint_val(ifd, *eo), len = b.uint_val(ifd, *ec);
        if (off == L.strip_off || len == 0 || off + len > skel_len) continue;
        Record r;
        r.off = off; r.len = len;
        if (ec->count == 1 && ec->type == 4) { r.count_pos = ec->value_pos; r.flags = uint8_t(2 | (ifd.le ? 1 : 0)); }
        unsigned w = get(0x100), h = get(0x101), spp = get(0x115, 1), comp = get(0x103, 1), phot = get(0x106, 0);
        unsigned bps = ifd.find(0x102) ? b.uint_val(ifd, *ifd.find(0x102)) : 8;
        bool is_jpeg = (jpeg_tags || comp == 6 || comp == 7) && len > 4 && nef[off] == 0xff && nef[off + 1] == 0xd8;
        if (is_jpeg) {
            unsigned jw = 0, jh = 0;
            if (!jpeg_dims(&nef[off], len, jw, jh)) continue;
            r.w = jw; r.h = jh;
            if (keep_previews) {
                auto jx = jxl_from_jpeg(&nef[off], len, 7);
                std::vector<uint8_t> back;
                if (jx.empty() || !jxl_to_jpeg(jx.data(), jx.size(), back) || back.size() != len ||
                    std::memcmp(back.data(), &nef[off], len) != 0)
                    continue;  // stays in the residual
                r.kind = kJpegKept;
                r.payload = std::move(jx);
                s.previews_kept += len;
            } else {
                r.kind = kJpegRegen;
                s.previews_dropped += len;
            }
            recs.push_back(std::move(r));
            zero(off, len);
        } else if (comp == 1 && bps == 8 && spp == 3 && phot == 2 && len == size_t(w) * h * 3) {
            if (keep_previews) continue;
            r.kind = kRgbRegen; r.w = w; r.h = h;
            s.previews_dropped += len;
            recs.push_back(std::move(r));
            zero(off, len);
        } else if (bps == 12 && (spp == 1 || spp == 2) && w && h && (w * spp) % 2 == 0 &&
                   len == size_t((w * spp * 3 / 2 + 15) / 16 * 16) * h) {
            unsigned stride = (w * spp * 3 / 2 + 15) / 16 * 16;
            auto planes = unpack12(&nef[off], w, h, spp, stride);
            auto jx = jxl_encode_gray16(planes.data(), w, h * spp, 0, true, 9);
            std::vector<uint16_t> back;
            if (!jxl_decode_gray16(jx.data(), jx.size(), w, h * spp, back) || back != planes) continue;
            r.kind = kPack12; r.w = w; r.h = h; r.a = spp; r.b = stride;
            r.payload = std::move(jx);
            s.helpers += len; s.helpers_stored += r.payload.size();
            for (unsigned y = 0; y < h; y++) zero(off + size_t(y) * stride, w * spp * 3 / 2);  // row padding stays
            recs.push_back(std::move(r));
        } else if (bps == 12 && spp == 1 && w % 4 == 0 && h % 2 == 0 && len == size_t(w) * h * 6 / 8) {
            auto planes = unpack6(&nef[off], w, h);
            auto jx = jxl_encode_gray8_lossless(planes.data(), w, h, 9);
            std::vector<uint8_t> back;
            if (!jxl_decode_gray8(jx.data(), jx.size(), w, h, back) || back != planes) continue;
            r.kind = kPack6; r.w = w; r.h = h;
            r.payload = std::move(jx);
            s.helpers += len; s.helpers_stored += r.payload.size();
            zero(off, len);
            recs.push_back(std::move(r));
        }
    }

    auto rz = brotli_compress(resid.data(), resid.size());
    Writer w;
    w.bytes(reinterpret_cast<const uint8_t *>(kMagic), 12);
    w.u32(2);
    w.u64(nef.size());
    auto fh = sha256(nef.data(), nef.size());
    w.bytes(fh.data(), 32);
    auto ph = sha_px(px);
    w.bytes(ph.data(), 32);
    w.u64(skel_len);
    w.u64(rz.size());
    w.u32(uint32_t(recs.size()));
    w.bytes(rz.data(), rz.size());
    for (auto &r : recs) {
        w.u8(r.kind); w.u64(r.off); w.u64(r.len); w.u32(r.w); w.u32(r.h); w.u32(r.a); w.u32(r.b);
        w.u64(r.count_pos); w.u8(r.flags); w.u64(r.payload.size()); w.bytes(r.payload.data(), r.payload.size());
    }
    s.blob = w.v.size();
    if (st) *st = s;
    return std::move(w.v);
}

std::vector<uint8_t> embed_restore(const std::vector<uint8_t> &blob, const std::vector<uint16_t> &px,
                                   const std::vector<uint8_t> &dng_mn, const RenderParams &rp, EmbedRestore *info) {
    EmbedRestore ri;
    Reader rd{blob.data(), blob.data() + blob.size()};
    if (std::memcmp(rd.take(12), kMagic, 12) != 0) throw std::runtime_error("unknown embedded NEF format");
    uint32_t ver = rd.u32();
    ri.original_size = rd.u64();
    const uint8_t *fh = rd.take(32), *ph = rd.take(32);
    ri.pixels_exact = std::memcmp(sha_px(px).data(), ph, 32) == 0;
    std::vector<uint8_t> skel;
    if (ver == 1) {
        skel = zdecompress(rd.p, size_t(rd.end - rd.p));
    } else if (ver == 2) {
        uint64_t skel_len = rd.u64(), rz = rd.u64();
        uint32_t n = rd.u32();
        if (skel_len > (512u << 20)) throw std::runtime_error("embedded skeleton too large");
        if (!brotli_decompress(rd.take(size_t(rz)), size_t(rz), size_t(skel_len), skel))
            throw std::runtime_error("corrupt embedded skeleton");
        std::vector<Record> recs(n);
        for (auto &r : recs) {
            r.kind = rd.u8(); r.off = rd.u64(); r.len = rd.u64(); r.w = rd.u32(); r.h = rd.u32(); r.a = rd.u32(); r.b = rd.u32();
            r.count_pos = rd.u64(); r.flags = rd.u8();
            uint64_t pl = rd.u64();
            const uint8_t *p = rd.take(size_t(pl));
            r.payload.assign(p, p + pl);
            if (r.off + r.len > skel.size()) throw std::runtime_error("embedded record out of range");
        }
        // data records first (makernote, helper images, kept previews), previews last
        for (auto &r : recs) {
            uint8_t *dst = &skel[r.off];
            if (r.kind == kMakernote) {
                if (dng_mn.size() != r.len) throw std::runtime_error("DNG makernote copy missing or changed");
                std::memcpy(dst, dng_mn.data(), r.len);
            } else if (r.kind == kPack12) {
                std::vector<uint16_t> planes;
                if (!jxl_decode_gray16(r.payload.data(), r.payload.size(), r.w, r.h * r.a, planes))
                    throw std::runtime_error("embedded image decode failed");
                pack12(planes, r.w, r.h, r.a, r.b, dst);
            } else if (r.kind == kPack6) {
                std::vector<uint8_t> planes;
                if (!jxl_decode_gray8(r.payload.data(), r.payload.size(), r.w, r.h, planes))
                    throw std::runtime_error("embedded image decode failed");
                pack6(planes, r.w, r.h, dst);
            } else if (r.kind == kJpegKept) {
                std::vector<uint8_t> j;
                if (!jxl_to_jpeg(r.payload.data(), r.payload.size(), j) || j.size() != r.len)
                    throw std::runtime_error("embedded preview decode failed");
                std::memcpy(dst, j.data(), r.len);
            }
        }
        NefRaw raw = describe_raw(skel, false);
        for (auto &r : recs) {
            if (r.kind == kJpegRegen) {
                auto rgb = render_fitted(px, raw.width, raw.height, rp, r.w, r.h);
                auto j = encode_jpeg_fit(rgb.data(), r.w, r.h, size_t(r.len));
                std::memcpy(&skel[r.off], j.data(), j.size());
                std::memset(&skel[r.off + j.size()], 0, size_t(r.len - j.size()));
                write_count(skel, r, uint32_t(j.size()));
                ri.previews_regenerated++;
            } else if (r.kind == kRgbRegen) {
                if (size_t(r.w) * r.h * 3 != r.len) continue;
                auto rgb = render_fitted(px, raw.width, raw.height, rp, r.w, r.h);
                std::memcpy(&skel[r.off], rgb.data(), rgb.size());
                ri.previews_regenerated++;
            }
        }
    } else {
        throw std::runtime_error("unsupported embedded NEF version " + std::to_string(ver));
    }
    std::vector<uint8_t> nef = nef_assemble(skel, px);
    ri.file_exact = std::memcmp(sha256(nef.data(), nef.size()).data(), fh, 32) == 0;
    if (info) *info = ri;
    return nef;
}

} // namespace nefcomp
