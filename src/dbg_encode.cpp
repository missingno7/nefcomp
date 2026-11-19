// Debug: lossless NEF -> HE codestream (.jxs) for round-trip testing.
#include "tiff.h"
#include "nikon_lossless.h"
#include "he_encoder.h"
#include <chrono>
#include <cstdio>
#include <cstring>
using namespace nefcomp;
int main(int argc, char **argv) {
    if (argc != 4) { puts("usage: dbg_encode in.NEF he|hestar out.jxs"); return 2; }
    Buf b;
    auto e = read_file(argv[1], b.d);
    if (!e.empty()) { puts(e.c_str()); return 1; }
    auto L = parse_nef(b);
    auto *m = L.makernote.find(0x96);
    std::vector<uint16_t> px;
    std::string err;
    if (!m || !decode_nikon_lossless(&b.d[L.strip_off], L.strip_len, &b.d[m->value_pos], m->size(),
                                     L.makernote.le, L.width, L.height, L.bps, px, err)) {
        printf("decode failed: %s\n", err.c_str());
        return 1;
    }
    HeEncodeOptions opt;
    opt.mode = strcmp(argv[2], "he") == 0 ? HeMode::HE : HeMode::HEStar;
    HeEncodeStats st;
    auto t0 = std::chrono::steady_clock::now();
    auto cs = he_encode(px.data(), L.width, L.height, opt, &st);
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    write_file(argv[3], cs);
    printf("bytes=%zu time=%.2fs meanS=%.2f pad min/mean/max=%zu/%.1f/%zu Q:", cs.size(), secs,
           st.mean_quality_index, st.min_pad, st.mean_pad, st.max_pad);
    for (unsigned q = 0; q < 16; q++) if (st.q_histogram[q]) printf(" %u:%zu", q, st.q_histogram[q]);
    puts("");
    return 0;
}
