// Minimal TIFF/NEF structure reader with in-place patch helpers.
#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <stdexcept>

namespace nefcomp {

struct Entry {
    uint16_t tag, type;
    uint32_t count;
    size_t entry_pos;   // absolute file position of the 12-byte entry
    size_t value_pos;   // absolute file position of the value bytes
    size_t size() const;
};

struct Ifd {
    size_t pos = 0;     // absolute position of the entry-count field
    size_t base = 0;    // offsets inside this IFD are relative to base
    bool le = true;
    std::vector<Entry> entries;
    const Entry *find(uint16_t tag) const;
};

class Buf {
public:
    std::vector<uint8_t> d;
    uint16_t u16(size_t o, bool le) const;
    uint32_t u32(size_t o, bool le) const;
    void w16(size_t o, uint16_t v, bool le);
    void w32(size_t o, uint32_t v, bool le);
    void need(size_t o, size_t n) const {
        if (o > d.size() || n > d.size() - o) throw std::runtime_error("truncated file / bad offset");
    }
    Ifd read_ifd(size_t pos, size_t base, bool le) const;
    // Integer value of a BYTE/SHORT/LONG entry (index i).
    uint32_t uint_val(const Ifd &ifd, const Entry &e, unsigned i = 0) const;
    // Remove one entry from an IFD in place (later entries shift down, count-1).
    // The value bytes of the removed entry are left as unreferenced data.
    void remove_entry(Ifd &ifd, uint16_t tag);
};

struct NefLayout {
    bool le = true;
    Ifd ifd0, raw_ifd, exif, makernote;
    bool have_makernote = false;
    size_t mn_tiff = 0;   // absolute position of the makernote's embedded TIFF header
    unsigned width = 0, height = 0, bps = 0, compression = 0;
    size_t strip_off = 0, strip_len = 0;
    unsigned strip_count = 1;  // several contiguous strips are treated as one block
};

// check_strip=false accepts a "skeleton" (file truncated at the raw strip).
NefLayout parse_nef(const Buf &b, bool check_strip = true);

// NEF compression code: makernote 0x0051 byte 10 (Z cameras), else tag 0x0093
// (older bodies). 1/4 lossy, 2/10 uncompressed, 3 lossless, 13 HE, 14 HE*; 0 = unknown.
unsigned nef_compression_code(const Buf &b, const NefLayout &L);
// Set it (in 0x0051 and/or 0x0093, whichever exist). Returns false if neither exists.
bool set_nef_compression_code(Buf &b, const NefLayout &L, unsigned code);
// Paths are UTF-8 (nefcomp keeps every path as UTF-8 internally).
std::string read_file(const std::string &path, std::vector<uint8_t> &out);
void write_file(const std::string &path, const std::vector<uint8_t> &data);

} // namespace nefcomp
