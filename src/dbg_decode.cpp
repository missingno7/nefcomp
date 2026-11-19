// Debug: decode a lossless NEF raw strip to little-endian uint16 samples.
#include "tiff.h"
#include "nikon_lossless.h"
#include <cstdio>
#include <cstring>
using namespace nefcomp;
int main(int argc, char **argv) {
    if (argc != 3) return 2;
    Buf b;
    auto e = read_file(argv[1], b.d);
    if (!e.empty()) { puts(e.c_str()); return 1; }
    auto L = parse_nef(b);
    printf("%ux%u bps=%u comp=%u strip=%zu+%zu\n", L.width, L.height, L.bps, L.compression, L.strip_off, L.strip_len);
    auto *m = L.makernote.find(0x96);
    if (!m) { puts("no 0x96 (not lossless?)"); return 1; }
    std::vector<uint16_t> px;
    std::string err;
    if (!decode_nikon_lossless(&b.d[L.strip_off], L.strip_len, &b.d[m->value_pos], m->size(), L.makernote.le,
                               L.width, L.height, L.bps, px, err)) {
        printf("decode failed: %s\n", err.c_str());
        return 1;
    }
    std::vector<uint8_t> o(px.size() * 2);
    memcpy(o.data(), px.data(), o.size());
    write_file(argv[2], o);
    puts("ok");
    return 0;
}
