// SPDX-License-Identifier: GPL-2.0-or-later
//
// Lossless NEF -> HE / HE* NEF container conversion.
//
// The changes mirror what a Nikon Z 8 writes for HE files (derived by diffing
// lossless, HE and HE* files shot back to back on firmware 4.06):
//   raw IFD   0x0117 StripByteCounts      -> codestream size
//   raw IFD   0xC7D5 (Nikon private TIFF) -> 0x000D mode, 0x000F size (+CRC16),
//                                            0x000E (lossless table copy) removed
//   makernote 0x0051 byte 10              -> 13 (HE) / 14 (HE*)   (3 = lossless)
//   makernote 0x0058                      -> codestream size, 1
//   makernote 0x0096 (linearization tbl)  -> removed
// The raw strip is replaced in place (it is the last thing in a camera NEF).
// Everything else (previews, Exif, encrypted blocks) is left untouched.

#include "nef_convert.h"
#include "he_decode.h"
#include "nikon_lossless.h"
#include "tiff.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace nefcomp {

namespace {

uint16_t crc16_ccitt(const uint8_t *p, size_t n) {
    uint16_t c = 0xffff;
    for (size_t i = 0; i < n; i++) {
        c ^= uint16_t(p[i] << 8);
        for (int k = 0; k < 8; k++) c = (c & 0x8000) ? uint16_t((c << 1) ^ 0x1021) : uint16_t(c << 1);
    }
    return c;
}

std::string ascii_tag(const Buf &b, const Ifd &ifd, uint16_t tag) {
    auto *e = ifd.find(tag);
    if (!e || e->type != 2) return {};
    b.need(e->value_pos, e->count);
    std::string s(reinterpret_cast<const char *>(&b.d[e->value_pos]), e->count);
    s = s.substr(0, s.find('\0'));
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

// A "Nikon\0" + version + TIFF block as used by raw-IFD tag 0xC7D5.
struct NikonBlock {
    uint8_t prefix[10];
    struct Item { uint16_t tag, type; uint32_t count; std::vector<uint8_t> data; };
    std::vector<Item> items;

    static NikonBlock parse(const Buf &b, size_t pos, size_t len) {
        b.need(pos, len);
        if (len < 18 || std::memcmp(&b.d[pos], "Nikon\0", 6) != 0) throw std::runtime_error("0xC7D5 block not recognised");
        NikonBlock nb;
        std::memcpy(nb.prefix, &b.d[pos], 10);
        size_t base = pos + 10;
        if (b.d[base] != 'I' || b.d[base + 1] != 'I') throw std::runtime_error("0xC7D5 block not little-endian");
        Ifd ifd = b.read_ifd(base + b.u32(base + 4, true), base, true);
        for (auto &e : ifd.entries) {
            b.need(e.value_pos, e.size());
            if (e.value_pos + e.size() > pos + len) throw std::runtime_error("0xC7D5 entry out of range");
            nb.items.push_back({e.tag, e.type, e.count,
                                std::vector<uint8_t>(b.d.begin() + e.value_pos, b.d.begin() + e.value_pos + e.size())});
        }
        return nb;
    }
    Item *find(uint16_t tag) {
        for (auto &i : items) if (i.tag == tag) return &i;
        return nullptr;
    }
    void remove(uint16_t tag) {
        items.erase(std::remove_if(items.begin(), items.end(), [&](const Item &i) { return i.tag == tag; }), items.end());
    }
    // Serialise the same way the camera does: IFD at 8, values in entry order, 4-byte aligned.
    std::vector<uint8_t> serialize() const {
        std::vector<uint8_t> out(prefix, prefix + 10);
        auto p16 = [&](uint16_t v) { out.push_back(uint8_t(v)); out.push_back(uint8_t(v >> 8)); };
        auto p32 = [&](uint32_t v) { for (int i = 0; i < 4; i++) out.push_back(uint8_t(v >> (8 * i))); };
        out.insert(out.end(), {'I', 'I', 42, 0});
        p32(8);
        p16(uint16_t(items.size()));
        uint32_t data_off = 8 + 2 + 12 * uint32_t(items.size()) + 4;
        std::vector<uint8_t> data;
        for (auto &it : items) {
            p16(it.tag); p16(it.type); p32(it.count);
            if (it.data.size() <= 4) {
                for (size_t k = 0; k < 4; k++) out.push_back(k < it.data.size() ? it.data[k] : 0);
            } else {
                p32(data_off + uint32_t(data.size()));
                data.insert(data.end(), it.data.begin(), it.data.end());
                while (data.size() & 3) data.push_back(0);  // camera aligns values to 4 bytes
            }
        }
        p32(0);
        out.insert(out.end(), data.begin(), data.end());
        return out;
    }
};

void set_le32(std::vector<uint8_t> &v, size_t at, uint32_t x) {
    if (at + 4 > v.size()) throw std::runtime_error("field out of range");
    for (int i = 0; i < 4; i++) v[at + i] = uint8_t(x >> (8 * i));
}
void fix_crc(std::vector<uint8_t> &v) {
    if (v.size() < 3) return;
    uint16_t c = crc16_ccitt(v.data(), v.size() - 2);
    v[v.size() - 2] = uint8_t(c);
    v[v.size() - 1] = uint8_t(c >> 8);
}
bool crc_ok(const std::vector<uint8_t> &v) {
    return v.size() >= 3 && crc16_ccitt(v.data(), v.size() - 2) == uint16_t(v[v.size() - 2] | v[v.size() - 1] << 8);
}

} // namespace

static const char *const kHeModels[] = {"NIKON Z 9", "NIKON Z 8", "NIKON Z f", "NIKON Z6_3",
                                        "NIKON Z50_2", "NIKON Z5_2", "NIKON ZR"};
bool he_capable_model(const std::string &model) {
    for (auto *m : kHeModels) if (model == m) return true;
    return false;
}
bool he_verified_model(const std::string &model) { return model == "NIKON Z 8"; }

const char *compression_name(NefCompression c) {
    switch (c) {
    case NefCompression::Lossless: return "lossless compressed";
    case NefCompression::HE: return "High Efficiency";
    case NefCompression::HEStar: return "High Efficiency*";
    case NefCompression::Other: return "other (lossy/uncompressed)";
    default: return "unknown";
    }
}

NefInfo inspect_nef(const std::vector<uint8_t> &file, bool check_strip) {
    Buf b;
    b.d = file;
    NefLayout L = parse_nef(b, check_strip);
    NefInfo info;
    info.make = ascii_tag(b, L.ifd0, 0x10f);
    info.model = ascii_tag(b, L.ifd0, 0x110);
    info.firmware = ascii_tag(b, L.ifd0, 0x131);
    info.width = L.width; info.height = L.height; info.bps = L.bps;
    info.strip_off = L.strip_off; info.strip_len = L.strip_len; info.file_size = file.size();
    if (L.have_makernote) {
        if ((info.compression_code = nef_compression_code(b, L)) != 0) {
            switch (info.compression_code) {
            case 3: info.compression = NefCompression::Lossless; break;
            case 13: info.compression = NefCompression::HE; break;
            case 14: info.compression = NefCompression::HEStar; break;
            default: info.compression = NefCompression::Other;
            }
        }
        if (auto *e = L.makernote.find(0x3d); e && e->count >= 1) info.black = b.uint_val(L.makernote, *e, 0);
    }
    // Fall back to the codestream itself.
    if (info.compression == NefCompression::Unknown && L.strip_len > 4 && L.strip_off + 2 <= file.size() &&
        file[L.strip_off] == 0xff && file[L.strip_off + 1] == 0x10)
        info.compression = NefCompression::HE;
    return info;
}

ConvertResult convert_lossless_to_he(const std::vector<uint8_t> &file, const HeEncodeOptions &opt_in,
                                     bool allow_untested_model) {
    ConvertResult res;
    Buf b;
    b.d = file;
    NefLayout L = parse_nef(b);
    const std::string make = ascii_tag(b, L.ifd0, 0x10f), model = ascii_tag(b, L.ifd0, 0x110);

    if (make.rfind("NIKON", 0) != 0) throw std::runtime_error("not a Nikon NEF (Make: " + make + ")");
    if (!L.have_makernote) throw std::runtime_error("Nikon makernote not found");
    if (L.compression != 34713) throw std::runtime_error("raw IFD is not Nikon-compressed");
    if (L.bps != 14) throw std::runtime_error("only 14-bit raw files are supported (file is " + std::to_string(L.bps) + "-bit)");
    // Adobe Camera Raw / DNG Converter and LibRaw only enable their HE decoder
    // for models that shoot HE in camera (verified: a Z 8 HE file relabelled
    // "NIKON Z 6" is rejected, Z 6 data relabelled "NIKON Z 8" is accepted).
    bool he_model = he_capable_model(model);
    if (model != "NIKON Z 8") {
        if (!allow_untested_model) {
            if (!he_model)
                throw std::runtime_error("the " + model + " has no HE mode, and raw converters (Adobe, LibRaw) only "
                                         "decode HE for cameras that offer it, so the result would not open");
            throw std::runtime_error("camera '" + model + "' is untested (HE layout verified for NIKON Z 8 only); "
                                     "use --allow-untested to try anyway");
        }
        res.warnings.push_back(he_model ? "camera '" + model + "' is not a Z 8: output is experimental"
                                        : "camera '" + model + "' has no HE mode: Adobe/LibRaw will NOT open the output");
    }
    auto *e51 = L.makernote.find(0x51);
    if (!e51 || e51->size() < 11) throw std::runtime_error("makernote tag 0x51 (NEF compression) missing");
    unsigned code = b.d[e51->value_pos + 10];
    const unsigned target = unsigned(opt_in.mode);
    if (code == target) throw std::runtime_error(code == 14 ? "file is already High Efficiency*" : "file is already High Efficiency");
    if (code == 13) throw std::runtime_error("file is already High Efficiency (smaller than HE*); HE* would only be larger");
    const bool from_he = code == 14;  // HE* -> HE re-encode
    if (code != 3 && !from_he) throw std::runtime_error("file is not lossless compressed (code " + std::to_string(code) + ")");
    auto *e96 = L.makernote.find(0x96);
    if (!from_he && !e96) throw std::runtime_error("makernote tag 0x96 (linearization table) missing");
    if (auto *cfa = L.raw_ifd.find(0x828e)) {
        if (cfa->count != 4 || b.d[cfa->value_pos] != 0 || b.d[cfa->value_pos + 1] != 1 ||
            b.d[cfa->value_pos + 2] != 1 || b.d[cfa->value_pos + 3] != 2)
            throw std::runtime_error("only RGGB CFA layout is supported");
    }
    if (L.strip_off + L.strip_len != file.size())
        throw std::runtime_error("raw strip is not at the end of the file (file was edited?)");

    unsigned black = 1008;
    if (auto *bl = L.makernote.find(0x3d); bl && bl->count >= 4) {
        unsigned v[4];
        for (unsigned i = 0; i < 4; i++) v[i] = b.uint_val(L.makernote, *bl, i);
        black = v[0];
        if (v[1] != black || v[2] != black || v[3] != black)
            res.warnings.push_back("per-channel black levels differ; using " + std::to_string(black));
    } else {
        res.warnings.push_back("black level tag missing; assuming 1008");
    }

    // 1. decode the source raw data
    std::string err;
    if (from_he) {
        if (!he_decode(&b.d[L.strip_off], L.strip_len, L.width, L.height, res.source_cfa, err))
            throw std::runtime_error("HE* decode failed: " + err);
    } else if (!decode_nikon_lossless(&b.d[L.strip_off], L.strip_len, &b.d[e96->value_pos], e96->size(), L.makernote.le,
                                      L.width, L.height, L.bps, res.source_cfa, err)) {
        throw std::runtime_error("lossless decode failed: " + err);
    }
    res.width = L.width; res.height = L.height; res.black = black;

    // 2. encode HE
    HeEncodeOptions opt = opt_in;
    opt.black = black;
    std::vector<uint8_t> cs = he_encode(res.source_cfa.data(), L.width, L.height, opt, &res.stats);
    const uint32_t lcod = uint32_t(cs.size());
    const uint8_t mode = uint8_t(opt.mode);

    // 3. patch metadata in place (all positions lie before the strip)
    auto *sc = L.raw_ifd.find(0x117);
    if (sc->type == 4) b.w32(sc->value_pos, lcod, L.raw_ifd.le);
    else throw std::runtime_error("StripByteCounts has unexpected type");

    b.d[e51->value_pos + 10] = mode;

    if (auto *e58 = L.makernote.find(0x58)) {
        if (e58->size() != 12 || std::memcmp(&b.d[e58->value_pos], "0100", 4) != 0)
            throw std::runtime_error("makernote tag 0x58 has an unknown layout");
        b.w32(e58->value_pos + 4, lcod, L.makernote.le);
        b.w32(e58->value_pos + 8, 1, L.makernote.le);
    }
    b.remove_entry(L.makernote, 0x96);

    if (auto *c7 = L.raw_ifd.find(0xc7d5)) {
        NikonBlock nb = NikonBlock::parse(b, c7->value_pos, c7->size());
        if (auto *m = nb.find(0x0d)) {
            if (m->data.size() != 8 || std::memcmp(m->data.data(), "0100", 4) != 0 || !crc_ok(m->data))
                throw std::runtime_error("0xC7D5/0x000D has an unknown layout");
            if (m->data[4] != code) throw std::runtime_error("0xC7D5 compression field disagrees with makernote");
            m->data[4] = mode;
            m->data[5] = 0;
            fix_crc(m->data);
        }
        else
            res.warnings.push_back("0xC7D5 block has no compression entry (0x000D); Adobe software needs it to "
                                   "recognise HE, so the output will likely not open there");
        nb.remove(0x0e);
        if (auto *m = nb.find(0x0f)) {
            if (m->data.size() != 14 || std::memcmp(m->data.data(), "0100", 4) != 0 || !crc_ok(m->data))
                throw std::runtime_error("0xC7D5/0x000F has an unknown layout");
            set_le32(m->data, 4, lcod);
            set_le32(m->data, 8, 1);
            fix_crc(m->data);
        }
        std::vector<uint8_t> blk = nb.serialize();
        if (blk.size() > c7->size()) throw std::runtime_error("internal: 0xC7D5 block grew");
        std::memcpy(&b.d[c7->value_pos], blk.data(), blk.size());
        std::memset(&b.d[c7->value_pos + blk.size()], 0, c7->size() - blk.size());
        b.w32(c7->entry_pos + 4, uint32_t(blk.size()), L.raw_ifd.le);
    }

    // 4. replace the strip
    b.d.resize(L.strip_off);
    b.d.insert(b.d.end(), cs.begin(), cs.end());
    res.output = std::move(b.d);

    // 5. sanity re-parse
    Buf chk;
    chk.d = res.output;
    NefLayout L2 = parse_nef(chk);
    if (L2.strip_len != lcod || L2.strip_off != L.strip_off || L2.makernote.find(0x96))
        throw std::runtime_error("internal: output re-parse mismatch");
    return res;
}

// Nikon's lossless linearization table (makernote 0x0096), identical on every
// lossless NEF examined (Z 8 and Z 6): "F0", four 2048 predictor seeds, ...
static const uint8_t kLossless96[46] = {
    0x46, 0x30, 0x00, 0x08, 0x00, 0x08, 0x00, 0x08, 0x00, 0x08, 0x22, 0x00, 0xb7, 0x2a, 0xaa, 0x9d,
    0xe0, 0x72, 0x5e, 0x91, 0x18, 0xf5, 0x1c, 0x99, 0x65, 0x82, 0xf0, 0xaf, 0xbf, 0x20, 0xd2, 0xd2,
    0x2f, 0xc6, 0xc1, 0x02, 0xa7, 0x86, 0xc5, 0x5a, 0x21, 0x58, 0xd8, 0xa6, 0xca, 0x36};

std::vector<uint8_t> skeleton_to_lossless(const std::vector<uint8_t> &skel_in) {
    Buf b;
    b.d = skel_in;
    NefLayout L = parse_nef(b, false);
    if (!L.have_makernote) throw std::runtime_error("makernote missing; cannot convert to lossless");
    if (L.bps != 14 || L.compression != 34713)  // the lossless table is verified for 14-bit files
        throw std::runtime_error("only 14-bit Nikon raw can be converted to lossless");
    const unsigned code = nef_compression_code(b, L);
    if (code == 3) return skel_in;
    const bool mle = L.makernote.le;
    std::vector<uint8_t> tail;  // blocks appended after the old skeleton end
    auto append = [&](const std::vector<uint8_t> &blk) {
        while ((b.d.size() + tail.size()) & 3) tail.push_back(0);
        size_t pos = b.d.size() + tail.size();
        tail.insert(tail.end(), blk.begin(), blk.end());
        return pos;
    };

    // 1. makernote: compression code, size field, lossless table
    if (!set_nef_compression_code(b, L, 3)) throw std::runtime_error("NEF compression tag missing");
    if (auto *e58 = L.makernote.find(0x58); e58 && e58->size() == 12) std::memset(&b.d[e58->value_pos + 4], 0, 8);
    if (auto *e96 = L.makernote.find(0x96); e96 && e96->size() >= sizeof kLossless96) {
        std::memcpy(&b.d[e96->value_pos], kLossless96, sizeof kLossless96);   // lossy table slot is larger
        std::memset(&b.d[e96->value_pos + sizeof kLossless96], 0, e96->size() - sizeof kLossless96);
        b.w32(e96->entry_pos + 4, uint32_t(sizeof kLossless96), mle);
    } else {
        // No table (HE, uncompressed): move the makernote to the end of the
        // skeleton and give it a new IFD that includes 0x0096. Makernote
        // offsets are relative to its own header, so its data stays valid.
        auto *mn = L.exif.find(0x927c);
        if (!mn) throw std::runtime_error("makernote entry missing");
        const size_t mpos = mn->value_pos, mlen = mn->size();
        std::vector<uint8_t> blob(b.d.begin() + mpos, b.d.begin() + mpos + mlen);
        const size_t base = 10;  // "Nikon\0" + version, then the TIFF header
        while (blob.size() & 3) blob.push_back(0);
        const uint32_t t96 = uint32_t(blob.size() - base);
        blob.insert(blob.end(), kLossless96, kLossless96 + sizeof kLossless96);
        while (blob.size() & 3) blob.push_back(0);
        const uint32_t ifd_rel = uint32_t(blob.size() - base);
        auto p16 = [&](uint16_t v) { blob.push_back(uint8_t(mle ? v : v >> 8)); blob.push_back(uint8_t(mle ? v >> 8 : v)); };
        auto p32 = [&](uint32_t v) { for (int i = 0; i < 4; i++) blob.push_back(uint8_t(v >> (mle ? 8 * i : 24 - 8 * i))); };
        std::vector<std::pair<uint16_t, std::vector<uint8_t>>> ents;
        for (auto &e : L.makernote.entries)
            ents.push_back({e.tag, std::vector<uint8_t>(b.d.begin() + e.entry_pos, b.d.begin() + e.entry_pos + 12)});
        {
            std::vector<uint8_t> e96(12);
            Buf t; t.d.assign(12, 0);
            t.w16(0, 0x96, mle); t.w16(2, 7, mle); t.w32(4, sizeof kLossless96, mle); t.w32(8, t96, mle);
            ents.push_back({0x96, t.d});
        }
        std::sort(ents.begin(), ents.end(), [](auto &a, auto &c) { return a.first < c.first; });
        p16(uint16_t(ents.size()));
        for (auto &e : ents) blob.insert(blob.end(), e.second.begin(), e.second.end());
        p32(b.u32(L.makernote.pos + 2 + 12 * L.makernote.entries.size(), mle));  // next IFD
        Buf hb; hb.d = blob;
        hb.w32(base + 4, ifd_rel, mle);
        blob = std::move(hb.d);
        std::memset(&b.d[mpos], 0, mlen);
        size_t npos = append(blob);
        b.w32(mn->entry_pos + 4, uint32_t(blob.size()), L.exif.le);
        b.w32(mn->entry_pos + 8, uint32_t(npos), L.exif.le);
    }

    // 2. raw IFD 0xC7D5: mode 3, size zero, lossless table copy 0x000E
    if (auto *c7 = L.raw_ifd.find(0xc7d5)) {
        NikonBlock nb = NikonBlock::parse(b, c7->value_pos, c7->size());
        if (auto *m = nb.find(0x0d); m && m->data.size() == 8 && crc_ok(m->data)) {
            m->data[4] = 3; m->data[5] = 0; fix_crc(m->data);
        }
        if (auto *m = nb.find(0x0f); m && m->data.size() == 14 && crc_ok(m->data)) {
            std::memset(&m->data[4], 0, 8); fix_crc(m->data);
        }
        if (nb.find(0x0d) && !nb.find(0x0e)) {
            std::vector<uint8_t> v{'0', '1', '0', '0'};
            v.insert(v.end(), kLossless96, kLossless96 + sizeof kLossless96);
            v.resize(v.size() + 2);
            fix_crc(v);
            nb.items.push_back({0x0e, 7, uint32_t(v.size()), v});
            std::sort(nb.items.begin(), nb.items.end(), [](auto &a, auto &c) { return a.tag < c.tag; });
        }
        std::vector<uint8_t> blk = nb.serialize();
        if (blk.size() <= c7->size()) {
            std::memcpy(&b.d[c7->value_pos], blk.data(), blk.size());
            std::memset(&b.d[c7->value_pos + blk.size()], 0, c7->size() - blk.size());
            b.w32(c7->entry_pos + 4, uint32_t(blk.size()), L.raw_ifd.le);
        } else {
            std::memset(&b.d[c7->value_pos], 0, c7->size());
            size_t npos = append(blk);
            b.w32(c7->entry_pos + 4, uint32_t(blk.size()), L.raw_ifd.le);
            b.w32(c7->entry_pos + 8, uint32_t(npos), L.raw_ifd.le);
        }
    }

    // 3. new skeleton end = raw strip offset (512-aligned like the camera)
    b.d.insert(b.d.end(), tail.begin(), tail.end());
    while (b.d.size() % 512) b.d.push_back(0);
    b.w32(L.raw_ifd.find(0x111)->value_pos, uint32_t(b.d.size()), L.raw_ifd.le);
    return std::move(b.d);
}

} // namespace nefcomp
