#include "tiff.h"
#include <cstdio>
#include <cstring>

namespace nefcomp {

static const unsigned kTypeSize[14] = {0, 1, 1, 2, 4, 8, 1, 1, 2, 4, 8, 4, 8, 4};

size_t Entry::size() const { return size_t(type < 14 ? kTypeSize[type] : 1) * count; }

const Entry *Ifd::find(uint16_t tag) const {
    for (auto &e : entries) if (e.tag == tag) return &e;
    return nullptr;
}

uint16_t Buf::u16(size_t o, bool le) const {
    need(o, 2);
    return le ? uint16_t(d[o] | d[o + 1] << 8) : uint16_t(d[o] << 8 | d[o + 1]);
}
uint32_t Buf::u32(size_t o, bool le) const {
    need(o, 4);
    return le ? uint32_t(d[o]) | uint32_t(d[o + 1]) << 8 | uint32_t(d[o + 2]) << 16 | uint32_t(d[o + 3]) << 24
              : uint32_t(d[o]) << 24 | uint32_t(d[o + 1]) << 16 | uint32_t(d[o + 2]) << 8 | uint32_t(d[o + 3]);
}
void Buf::w16(size_t o, uint16_t v, bool le) {
    need(o, 2);
    if (le) { d[o] = uint8_t(v); d[o + 1] = uint8_t(v >> 8); }
    else { d[o] = uint8_t(v >> 8); d[o + 1] = uint8_t(v); }
}
void Buf::w32(size_t o, uint32_t v, bool le) {
    need(o, 4);
    for (int i = 0; i < 4; i++) d[o + (le ? i : 3 - i)] = uint8_t(v >> (8 * i));
}

Ifd Buf::read_ifd(size_t pos, size_t base, bool le) const {
    Ifd ifd; ifd.pos = pos; ifd.base = base; ifd.le = le;
    unsigned n = u16(pos, le);
    need(pos + 2, size_t(n) * 12 + 4);
    for (unsigned i = 0; i < n; i++) {
        size_t e = pos + 2 + 12 * i;
        Entry en{u16(e, le), u16(e + 2, le), u32(e + 4, le), e, 0};
        en.value_pos = en.size() <= 4 ? e + 8 : base + u32(e + 8, le);
        ifd.entries.push_back(en);
    }
    return ifd;
}

uint32_t Buf::uint_val(const Ifd &ifd, const Entry &e, unsigned i) const {
    if (i >= e.count) throw std::runtime_error("tag index out of range");
    if (e.type == 3) return u16(e.value_pos + 2 * i, ifd.le);
    if (e.type == 4 || e.type == 13) return u32(e.value_pos + 4 * i, ifd.le);
    if (e.type == 1 || e.type == 7) { need(e.value_pos + i, 1); return d[e.value_pos + i]; }
    throw std::runtime_error("unexpected tag type");
}

void Buf::remove_entry(Ifd &ifd, uint16_t tag) {
    size_t idx = 0;
    for (; idx < ifd.entries.size(); idx++) if (ifd.entries[idx].tag == tag) break;
    if (idx == ifd.entries.size()) return;
    size_t n = ifd.entries.size();
    size_t first = ifd.pos + 2;
    need(first, 12 * n + 4);
    // Shift the following entries and the 4-byte next-IFD pointer down by one slot.
    std::memmove(&d[first + 12 * idx], &d[first + 12 * (idx + 1)], 12 * (n - idx - 1) + 4);
    std::memset(&d[first + 12 * (n - 1) + 4], 0, 12);
    w16(ifd.pos, uint16_t(n - 1), ifd.le);
    ifd = read_ifd(ifd.pos, ifd.base, ifd.le);
}

NefLayout parse_nef(const Buf &b, bool check_strip) {
    NefLayout L;
    b.need(0, 8);
    if (b.d[0] == 'I' && b.d[1] == 'I') L.le = true;
    else if (b.d[0] == 'M' && b.d[1] == 'M') L.le = false;
    else throw std::runtime_error("not a TIFF/NEF file");
    if (b.u16(2, L.le) != 42) throw std::runtime_error("not a TIFF/NEF file");
    L.ifd0 = b.read_ifd(b.u32(4, L.le), 0, L.le);
    bool have_raw = false;
    if (auto *e = L.ifd0.find(0x14a)) {
        for (unsigned i = 0; i < e->count && !have_raw; i++) {
            Ifd s = b.read_ifd(b.uint_val(L.ifd0, *e, i), 0, L.le);
            auto *nst = s.find(0xfe), *cmp = s.find(0x103), *w = s.find(0x100);
            if (nst && cmp && w && b.uint_val(s, *nst) == 0) { L.raw_ifd = s; have_raw = true; }
        }
    }
    if (!have_raw) throw std::runtime_error("no raw image IFD found");
    const Ifd &r = L.raw_ifd;
    auto req = [&](uint16_t t) {
        auto *e = r.find(t);
        if (!e) throw std::runtime_error("raw IFD missing a required tag");
        return b.uint_val(r, *e);
    };
    L.width = req(0x100); L.height = req(0x101); L.bps = req(0x102); L.compression = req(0x103);
    auto *so = r.find(0x111), *sc = r.find(0x117);
    if (!so || !sc || so->count != 1 || sc->count != 1) throw std::runtime_error("raw IFD must have exactly one strip");
    L.strip_off = b.uint_val(r, *so); L.strip_len = b.uint_val(r, *sc);
    if (check_strip) b.need(L.strip_off, L.strip_len);
    if (auto *e = L.ifd0.find(0x8769)) {
        L.exif = b.read_ifd(b.uint_val(L.ifd0, *e), 0, L.le);
        if (auto *mn = L.exif.find(0x927c)) {
            size_t p = mn->value_pos;
            b.need(p, 18);
            if (std::memcmp(&b.d[p], "Nikon\0", 6) == 0) {
                L.mn_tiff = p + 10;
                bool mle = b.d[L.mn_tiff] == 'I';
                L.makernote = b.read_ifd(L.mn_tiff + b.u32(L.mn_tiff + 4, mle), L.mn_tiff, mle);
                L.have_makernote = true;
            }
        }
    }
    return L;
}

unsigned nef_compression_code(const Buf &b, const NefLayout &L) {
    if (!L.have_makernote) return 0;
    if (auto *e = L.makernote.find(0x51); e && e->size() > 10) return b.d[e->value_pos + 10];
    if (auto *e = L.makernote.find(0x93); e && e->count >= 1 && e->type == 3) return b.uint_val(L.makernote, *e);
    return 0;
}

bool set_nef_compression_code(Buf &b, const NefLayout &L, unsigned code) {
    bool done = false;
    if (!L.have_makernote) return false;
    if (auto *e = L.makernote.find(0x51); e && e->size() > 10) { b.d[e->value_pos + 10] = uint8_t(code); done = true; }
    if (auto *e = L.makernote.find(0x93); e && e->count >= 1 && e->type == 3) { b.w16(e->value_pos, uint16_t(code), L.makernote.le); done = true; }
    return done;
}

std::string read_file(const std::string &path, std::vector<uint8_t> &out) {
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) return "cannot open " + path;
    std::fseek(f, 0, SEEK_END);
    long long n = _ftelli64(f);
    std::fseek(f, 0, SEEK_SET);
    if (n < 0) { std::fclose(f); return "cannot size " + path; }
    out.resize(size_t(n));
    size_t got = n ? std::fread(out.data(), 1, out.size(), f) : 0;
    std::fclose(f);
    return got == out.size() ? "" : "short read " + path;
}

void write_file(const std::string &path, const std::vector<uint8_t> &data) {
    FILE *f = std::fopen(path.c_str(), "wb");
    if (!f) throw std::runtime_error("cannot create " + path);
    bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
    if (std::fclose(f) != 0 || !ok) throw std::runtime_error("write failed " + path);
}

} // namespace nefcomp
