// SPDX-License-Identifier: GPL-2.0-or-later
//
// NEF -> JPEG XL raw DNG (DNG 1.7) and back.
//
// * Adobe DNG Converter (-u) writes an uncompressed DNG with Adobe's colour
//   profile, opcodes and Exif for the camera; nefcomp rewrites only its raw IFD.
// * The 14-bit mosaic is companded with Nikon HE's square-root transfer into
//   16-bit codes; a DNG LinearizationTable maps them back (exactly invertible).
// * Row/ColumnInterleaveFactor = 2: the stored image is the four Bayer planes
//   [R G1; G2 B] (the interleave applies to the whole image, not per tile).
// * Each tile is a bare JPEG XL codestream (compression 52546).
// * NewRawImageDigest = MD5 over the per-tile MD5s of the compressed tiles
//   (how Adobe computes it for JPEG XL raw data). A stale digest makes Camera
//   Raw warn that the file is damaged.
// * Private IFD0 tag 0xFE12 carries the rest of the NEF (see nef_embed.cpp) so
//   `restore` can rebuild a genuine NEF.

#include "dng_jxl.h"
#include "hash.h"
#include "jxl_util.h"
#include "nef_embed.h"
#include "nef_rebuild.h"
#include "preview.h"
#include "tiff.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace nefcomp {

namespace fs = std::filesystem;

namespace {

constexpr uint16_t kSkelTag = 0xFE12;
constexpr uint16_t kJxlCompression = 52546;

// ---------------------------------------------------------------- TIFF tree
struct TEntry { uint16_t type = 0; uint32_t count = 0; std::vector<uint8_t> data; };
struct TNode {
    std::map<uint16_t, TEntry> tags;
    std::map<uint16_t, std::vector<TNode>> subs;
    std::map<uint16_t, std::vector<std::vector<uint8_t>>> blocks;  // keyed by offsets tag
};
const uint16_t kIfdTags[] = {0x14a, 0x8769, 0x8825, 0xa005};
const std::pair<uint16_t, uint16_t> kBlockTags[] = {{0x111, 0x117}, {0x144, 0x145}, {0x201, 0x202}};

struct Tiff {
    const Buf &b;
    bool le;
    uint32_t rd32(const uint8_t *p) const { return le ? p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24 : uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
    std::vector<uint32_t> ints(const TEntry &e) const {
        std::vector<uint32_t> v;
        const uint8_t *p = e.data.data();
        for (uint32_t i = 0; i < e.count; i++) {
            if (e.type == 3) v.push_back(le ? p[2 * i] | p[2 * i + 1] << 8 : p[2 * i] << 8 | p[2 * i + 1]);
            else if (e.type == 4 || e.type == 13) v.push_back(rd32(p + 4 * i));
            else if (e.type == 1 || e.type == 7) v.push_back(p[i]);
            else throw std::runtime_error("unexpected TIFF type");
        }
        return v;
    }
    std::vector<double> reals(const TEntry &e) const {
        std::vector<double> v;
        const uint8_t *p = e.data.data();
        for (uint32_t i = 0; i < e.count; i++) {
            if (e.type == 5) { uint32_t n = rd32(p + 8 * i), d = rd32(p + 8 * i + 4); v.push_back(d ? double(n) / d : 0); }
            else if (e.type == 10) { int32_t n = int32_t(rd32(p + 8 * i)), d = int32_t(rd32(p + 8 * i + 4)); v.push_back(d ? double(n) / d : 0); }
            else if (e.type == 11) { uint32_t u = rd32(p + 4 * i); float f; std::memcpy(&f, &u, 4); v.push_back(f); }
            else { auto iv = ints(e); return std::vector<double>(iv.begin(), iv.end()); }
        }
        return v;
    }
    std::vector<TNode> chain(size_t off, int depth = 0) const {
        std::vector<TNode> out;
        while (off) {
            if (depth > 8 || out.size() > 64) throw std::runtime_error("TIFF structure too deep");
            Ifd ifd = b.read_ifd(off, 0, le);
            TNode n;
            for (auto &e : ifd.entries) {
                b.need(e.value_pos, e.size());
                n.tags[e.tag] = {e.type, e.count, std::vector<uint8_t>(b.d.begin() + e.value_pos, b.d.begin() + e.value_pos + e.size())};
            }
            for (uint16_t t : kIfdTags)
                if (n.tags.count(t)) for (uint32_t o : ints(n.tags[t])) n.subs[t].push_back(chain(o, depth + 1).at(0));
            for (auto [ot, ct] : kBlockTags)
                if (n.tags.count(ot) && n.tags.count(ct)) {
                    auto offs = ints(n.tags[ot]), cnts = ints(n.tags[ct]);
                    if (offs.size() != cnts.size()) throw std::runtime_error("offset/count mismatch");
                    for (size_t i = 0; i < offs.size(); i++) {
                        b.need(offs[i], cnts[i]);
                        n.blocks[ot].emplace_back(b.d.begin() + offs[i], b.d.begin() + offs[i] + cnts[i]);
                    }
                }
            out.push_back(std::move(n));
            off = b.u32(ifd.pos + 2 + 12 * ifd.entries.size(), le);
            if (depth) break;  // sub-IFDs: only the first of a chain is used
        }
        return out;
    }
};

struct TiffWriter {
    bool le;
    std::vector<uint8_t> out;
    explicit TiffWriter(bool l) : le(l) {
        out = le ? std::vector<uint8_t>{'I', 'I', 42, 0, 0, 0, 0, 0} : std::vector<uint8_t>{'M', 'M', 0, 42, 0, 0, 0, 0};
    }
    void p16(std::vector<uint8_t> &v, uint16_t x) { if (le) { v.push_back(uint8_t(x)); v.push_back(uint8_t(x >> 8)); } else { v.push_back(uint8_t(x >> 8)); v.push_back(uint8_t(x)); } }
    void p32(std::vector<uint8_t> &v, uint32_t x) { for (int i = 0; i < 4; i++) v.push_back(uint8_t(x >> (le ? 8 * i : 24 - 8 * i))); }
    void s32(size_t at, uint32_t x) { for (int i = 0; i < 4; i++) out[at + i] = uint8_t(x >> (le ? 8 * i : 24 - 8 * i)); }
    void align() { if (out.size() & 1) out.push_back(0); }
    TEntry longs(const std::vector<uint32_t> &v) {
        TEntry e{4, uint32_t(v.size()), {}};
        for (uint32_t x : v) p32(e.data, x);
        return e;
    }
    uint32_t chain(std::vector<TNode> &nodes) {
        std::vector<size_t> pos;
        for (auto &n : nodes) pos.push_back(ifd(n));
        for (size_t i = 0; i + 1 < pos.size(); i++) {
            unsigned cnt = le ? out[pos[i]] | out[pos[i] + 1] << 8 : out[pos[i]] << 8 | out[pos[i] + 1];
            s32(pos[i] + 2 + 12 * cnt, uint32_t(pos[i + 1]));
        }
        return uint32_t(pos.at(0));
    }
    size_t ifd(TNode &n) {
        auto tags = n.tags;
        for (auto &[t, kids] : n.subs) {
            std::vector<uint32_t> offs;
            for (auto &k : kids) { std::vector<TNode> one{k}; offs.push_back(chain(one)); }
            tags[t] = longs(offs);
        }
        for (auto &[ot, blocks] : n.blocks) {
            uint16_t ct = 0;
            for (auto [a, c] : kBlockTags) if (a == ot) ct = c;
            std::vector<uint32_t> offs, cnts;
            for (auto &blk : blocks) {
                align();
                offs.push_back(uint32_t(out.size()));
                cnts.push_back(uint32_t(blk.size()));
                out.insert(out.end(), blk.begin(), blk.end());
            }
            tags[ot] = longs(offs);
            tags[ct] = longs(cnts);
        }
        if (out.size() > 0xF0000000u) throw std::runtime_error("DNG too large");
        std::map<uint16_t, uint32_t> vpos;
        for (auto &[t, e] : tags)
            if (e.data.size() > 4) { align(); vpos[t] = uint32_t(out.size()); out.insert(out.end(), e.data.begin(), e.data.end()); }
        align();
        size_t pos = out.size();
        p16(out, uint16_t(tags.size()));
        for (auto &[t, e] : tags) {
            p16(out, t); p16(out, e.type); p32(out, e.count);
            if (vpos.count(t)) p32(out, vpos[t]);
            else { auto d = e.data; d.resize(4, 0); out.insert(out.end(), d.begin(), d.end()); }
        }
        p32(out, 0);
        return pos;
    }
};

TNode *find_raw(std::vector<TNode> &chain, const Tiff &t) {
    for (auto &kid : chain.at(0).subs[0x14a])
        if (kid.tags.count(0xfe) && t.ints(kid.tags[0xfe]).at(0) == 0) return &kid;
    if (chain.at(0).tags.count(0xfe) && t.ints(chain[0].tags[0xfe]).at(0) == 0) return &chain[0];
    throw std::runtime_error("DNG has no raw IFD");
}

// ------------------------------------------------------------- companding
std::vector<uint16_t> he_transfer(unsigned black) {
    unsigned A, B = black, C;
    if (black == 1008) { A = 84429; C = 16255; }
    else {
        C = unsigned(std::floor(std::sqrt(double(black) * 262144.0)));
        A = unsigned(std::lround(65536.0 * std::sqrt((16384.0 - black) * 262144.0) / (65535.0 - C)));
    }
    std::vector<uint16_t> tr(65536);
    for (int64_t i = 0; i < 65536; i++) {
        int64_t d = i - C, q;
        if (d < 0) q = -d * d;
        else { int64_t z = (d * A + 32768) >> 16; q = z * z; }
        int64_t v = q + 163840, fl = v >= 0 ? v >> 18 : -(((-v) + (1 << 18) - 1) >> 18);
        tr[i] = uint16_t(std::clamp<int64_t>(int64_t(B) + fl, 0, 16383));
    }
    return tr;
}

std::vector<uint16_t> he_inverse(const std::vector<uint16_t> &tr) {
    std::vector<uint16_t> inv(16384);
    for (int r = 0; r < 16384; r++) {
        auto it = std::lower_bound(tr.begin(), tr.end(), uint16_t(r));
        size_t k = it == tr.end() ? 65535 : size_t(it - tr.begin());
        if (k && std::abs(int(tr[k - 1]) - r) < std::abs(int(tr[k]) - r)) --k;
        inv[r] = uint16_t(k);
        if (tr[k] != r) throw std::runtime_error("internal: companding not invertible");
    }
    return inv;
}

template <class F> void parallel_for(unsigned n, unsigned threads, F f) {
    unsigned nt = threads ? threads : std::max(1u, std::thread::hardware_concurrency());
    nt = std::max(1u, std::min(nt, n));
    std::atomic<unsigned> next{0};
    std::mutex m;
    std::string err;
    auto work = [&]() {
        try {
            for (unsigned i; (i = next++) < n;) f(i);
        } catch (const std::exception &e) {
            std::lock_guard<std::mutex> g(m);
            if (err.empty()) err = e.what();
            next = n;
        }
    };
    std::vector<std::thread> pool;
    for (unsigned i = 1; i < nt; i++) pool.emplace_back(work);
    work();
    for (auto &t : pool) t.join();
    if (!err.empty()) throw std::runtime_error(err);
}

std::wstring widen(const std::string &s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n ? n - 1 : 0, L'\0');
    if (n) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}

// Run Adobe DNG Converter: uncompressed DNG of `nef` into `outdir`.
void run_dng_converter(const std::string &exe, const fs::path &nef, const fs::path &outdir, const std::string &name) {
    std::wstring cmd = L"\"" + widen(exe) + L"\" -u -d \"" + outdir.wstring() + L"\" -o \"" + widen(name) + L"\" \"" +
                       fs::absolute(nef).wstring() + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE nul = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    si.hStdOutput = si.hStdError = nul;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    BOOL ok = CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    if (!ok) { CloseHandle(nul); throw std::runtime_error("cannot start Adobe DNG Converter (" + exe + ")"); }
    WaitForSingleObject(pi.hProcess, INFINITE);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(nul);
}

// Colour/crop parameters for preview rendering, from the DNG's tags.
RenderParams render_params(const Tiff &t, TNode &ifd0, TNode &raw) {
    RenderParams rp;
    auto has = [](TNode &n, uint16_t k) { return n.tags.count(k) > 0; };
    if (has(raw, 0xc61a)) { auto v = t.reals(raw.tags[0xc61a]); double s = 0; for (double x : v) s += x; rp.black = s / v.size(); }
    if (has(raw, 0xc61d)) rp.white = t.reals(raw.tags[0xc61d]).at(0);
    if (has(raw, 0xc618)) {  // 16-bit scale DNG (see jxl_dng_decode): levels in nefcomp's 14-bit units
        auto lut = t.ints(raw.tags[0xc618]);
        if (!lut.empty() && *std::max_element(lut.begin(), lut.end()) > 16383) { rp.black /= 4; rp.white /= 4; }
    }
    if (has(ifd0, 0xc628)) { auto v = t.reals(ifd0.tags[0xc628]); for (int i = 0; i < 3 && i < int(v.size()); i++) rp.neutral[i] = v[i]; }
    uint16_t fm = has(ifd0, 0xc715) ? 0xc715 : has(ifd0, 0xc714) ? 0xc714 : 0;  // prefer the D65 matrix
    if (fm) { auto v = t.reals(ifd0.tags[fm]); if (v.size() == 9) std::copy(v.begin(), v.end(), rp.fm); }
    if (has(ifd0, 0xc62a)) rp.exposure_ev = t.reals(ifd0.tags[0xc62a]).at(0);
    if (has(raw, 0xc61f) && has(raw, 0xc620)) {
        auto o = t.reals(raw.tags[0xc61f]), s = t.reals(raw.tags[0xc620]);
        unsigned ax = 0, ay = 0;
        if (has(raw, 0xc68d)) { auto a = t.ints(raw.tags[0xc68d]); ay = a.at(0); ax = a.at(1); }
        rp.crop_x = ax + unsigned(o.at(0)); rp.crop_y = ay + unsigned(o.at(1));
        rp.crop_w = unsigned(s.at(0)); rp.crop_h = unsigned(s.at(1));
    }
    return rp;
}


// Decode the JPEG XL raw IFD of a DNG into 14-bit linear samples.
std::vector<uint16_t> decode_jxl_raw(const Tiff &t, TNode &raw, unsigned threads, unsigned &W, unsigned &H) {
    auto iv = [&](uint16_t tag) { return t.ints(raw.tags.at(tag)).at(0); };
    W = iv(0x100); H = iv(0x101);
    if (iv(0x103) != kJxlCompression) throw std::runtime_error("raw data is not JPEG XL");
    const unsigned TW = iv(0x142), TH = iv(0x143), tx = (W + TW - 1) / TW, ty = (H + TH - 1) / TH;
    auto &tiles = raw.blocks.at(0x144);
    if (tiles.size() != size_t(tx) * ty) throw std::runtime_error("tile count mismatch");
    std::vector<uint16_t> stored(size_t(W) * H);
    parallel_for(unsigned(tiles.size()), threads, [&](unsigned i) {
        std::vector<uint16_t> d;
        if (!jxl_decode_gray16(tiles[i].data(), tiles[i].size(), TW, TH, d)) throw std::runtime_error("JPEG XL tile decode failed");
        unsigned yy = i / tx, xx = i % tx;
        for (unsigned y = 0; y < TH && yy * TH + y < H; y++)
            for (unsigned x = 0; x < TW && xx * TW + x < W; x++) stored[size_t(yy * TH + y) * W + xx * TW + x] = d[size_t(y) * TW + x];
    });
    unsigned rif = raw.tags.count(0xc71f) ? iv(0xc71f) : 1, cif = raw.tags.count(0xcd43) ? iv(0xcd43) : 1;
    if (!((rif == 2 && cif == 2) || (rif == 1 && cif == 1))) throw std::runtime_error("unsupported interleave");
    std::vector<uint16_t> lut;
    if (raw.tags.count(0xc618)) for (uint32_t v : t.ints(raw.tags[0xc618])) lut.push_back(uint16_t(v));
    // nefcomp's tables top out at 16383, or 4 x that for files Adobe keeps at 16-bit scale
    const unsigned shift = !lut.empty() && *std::max_element(lut.begin(), lut.end()) > 16383 ? 2 : 0;
    const unsigned h2 = H / 2, w2 = W / 2;
    std::vector<uint16_t> px(size_t(W) * H);
    for (unsigned y = 0; y < H; y++)
        for (unsigned x = 0; x < W; x++) {
            unsigned sy = rif == 2 ? (y & 1) * h2 + y / 2 : y, sx = cif == 2 ? (x & 1) * w2 + x / 2 : x;
            uint16_t c = stored[size_t(sy) * W + sx];
            uint16_t v = lut.empty() ? c : lut[std::min<size_t>(c, lut.size() - 1)];
            px[size_t(y) * W + x] = std::min<uint16_t>(uint16_t(v >> shift), 16383);
        }
    return px;
}

} // namespace

float quality_to_distance(double q) {
    if (q >= 100) return 0;
    q = std::max(1.0, q);
    return float(0.1 * std::pow(10.0, (60.0 - q) / 40.0));  // 80 -> 0.032, 60 -> 0.1, 40 -> 0.32
}

std::vector<uint16_t> jxl_dng_decode(const std::vector<uint8_t> &dng, unsigned &W, unsigned &H, unsigned threads) {
    Buf b;
    b.d = dng;
    if (b.d.size() < 8) throw std::runtime_error("not a DNG");
    bool le = b.d[0] == 'I';
    Tiff t{b, le};
    std::vector<TNode> chain = t.chain(b.u32(4, le));
    return decode_jxl_raw(t, *find_raw(chain, t), threads, W, H);
}

std::string default_dng_converter() {
    return "C:\\Program Files\\Adobe\\Adobe DNG Converter\\Adobe DNG Converter.exe";
}

DngResult nef_to_jxl_dng(const std::string &nef_path, const DngOptions &o) {
    DngResult res;
    std::vector<uint8_t> nef;
    auto e = read_file(nef_path, nef);
    if (!e.empty()) throw std::runtime_error(e);
    NefRaw info;
    std::vector<uint16_t> px = decode_raw(nef, &info);
    unsigned W = info.width, H = info.height;  // become the DNG raw size below
    const unsigned NW = W, NH = H;             // full NEF raw size

    // 1. Adobe DNG Converter -> uncompressed base DNG (metadata/profile source)
    std::string exe = o.dng_converter.empty() ? default_dng_converter() : o.dng_converter;
    if (!fs::exists(widen(exe))) throw std::runtime_error("Adobe DNG Converter not found at " + exe + " (use --dng-converter)");
    wchar_t tmpw[MAX_PATH];
    GetTempPathW(MAX_PATH, tmpw);
    static std::atomic<unsigned> seq{0};
    fs::path tmp = fs::path(tmpw) / (L"nefcomp_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(seq++));
    fs::create_directories(tmp);
    struct Cleanup { fs::path p; ~Cleanup() { std::error_code ec; fs::remove_all(p, ec); } } cleanup{tmp};
    run_dng_converter(exe, fs::u8path(nef_path), tmp, "base.dng");  // UTF-8 -> path
    Buf b;
    e = read_file((tmp / "base.dng").u8string(), b.d);
    if (!e.empty()) throw std::runtime_error("Adobe DNG Converter did not produce a DNG (unsupported camera?)");

    bool le = b.d.size() > 2 && b.d[0] == 'I';
    Tiff t{b, le};
    std::vector<TNode> chain = t.chain(b.u32(4, le));
    TNode &raw = *find_raw(chain, t);
    TNode &ifd0 = chain[0];
    auto iv = [&](uint16_t tag) { return t.ints(raw.tags.at(tag)).at(0); };
    const unsigned DW = iv(0x100), DH = iv(0x101);
    if (iv(0x103) != 1 || iv(0x102) != 16 || iv(0x106) != 32803 || DW > NW || DH > NH)
        throw std::runtime_error("unexpected raw layout in Adobe's DNG");
    // Adobe's raw samples must equal nefcomp's decode. Adobe may crop masked
    // border columns/rows: the crop offset is found by exact matching and the
    // cropped-off pixels are kept losslessly in the embedded NEF data. For
    // lossy NEFs Adobe keeps the raw codes plus its curve (LinearizationTable);
    // we take the linear values from Adobe's curve, so the DNG shows exactly
    // what Camera Raw shows for the original NEF, and keep that curve.
    std::vector<uint16_t> lossy_curve, lossy_codes;
    unsigned cx = 0, cy = 0, scale_shift = 0;
    {
        std::vector<uint8_t> data;
        for (auto &blk : raw.blocks.at(0x111)) data.insert(data.end(), blk.begin(), blk.end());
        if (data.size() < size_t(DW) * DH * 2) throw std::runtime_error("short raw data in Adobe's DNG");
        auto sample = [&](size_t i) {
            return le ? uint16_t(data[2 * i] | data[2 * i + 1] << 8) : uint16_t(data[2 * i] << 8 | data[2 * i + 1]);
        };
        std::vector<uint16_t> ref = px;  // what Adobe stores: linear values, or raw codes for lossy NEFs
        if (info.format == RawFormat::Lossy) {
            std::vector<uint16_t> nikon_curve, adobe_lut;
            decode_raw_codes(nef, lossy_codes, nikon_curve);  // int16 codes (may leave the curve after a split)
            if (!raw.tags.count(0xc618)) throw std::runtime_error("Adobe's DNG of a lossy NEF has no linearization table");
            for (uint32_t v : t.ints(raw.tags[0xc618])) adobe_lut.push_back(uint16_t(v));
            if (adobe_lut.empty()) throw std::runtime_error("Adobe's linearization table is empty");
            // Adobe stores codes clamped like LibRaw; its table covers the codes it stores
            lossy_curve.resize(std::min<size_t>(std::max(nikon_curve.size(), adobe_lut.size()), 0x4000));
            for (size_t i = 0; i < lossy_curve.size(); i++)
                lossy_curve[i] = std::min<uint16_t>(adobe_lut[std::min(i, adobe_lut.size() - 1)], 16383);
            for (size_t i = 0; i < px.size(); i++) {
                int c = std::clamp<int>(int16_t(lossy_codes[i]), 0, 0x3fff);
                ref[i] = uint16_t(c);
                px[i] = lossy_curve[std::min<size_t>(c, lossy_curve.size() - 1)];
            }
        }
        auto matches = [&](unsigned x0, unsigned y0, unsigned rows) {
            for (unsigned y = 0; y < rows; y++)
                for (unsigned x = 0; x < DW; x++)
                    if (sample(size_t(y) * DW + x) != ref[size_t(y + y0) * NW + x + x0]) return false;
            return true;
        };
        bool found = false;
        for (int pass = 0; pass < 2 && !found; pass++) {
            if (pass == 1) {  // Adobe keeps 16-bit words where nefcomp uses word >> 2 (Coolpix P7700, ...)
                if (info.format != RawFormat::Uncompressed || info.layout != UncompLayout::Unpacked16Shift4) break;
                for (auto &v : ref) v = uint16_t(v << 2);
                scale_shift = 2;
            }
            for (unsigned y0 = 0; y0 <= NH - DH && !found; y0++)
                for (unsigned x0 = 0; x0 <= NW - DW && !found; x0++)
                    if (matches(x0, y0, std::min(DH, 8u)) && matches(x0, y0, DH)) { cx = x0; cy = y0; found = true; }
        }
        if (!found) throw std::runtime_error("Adobe's raw data differs from nefcomp's decode");
    }
    // the Bayer plane split needs even sizes: an odd Adobe crop (e.g. D80, 2611
    // rows) takes one more NEF row/column, and an ActiveArea keeps Adobe's region
    const unsigned EW = DW + (DW & 1), EH = DH + (DH & 1);
    if (cx + EW > NW || cy + EH > NH) throw std::runtime_error("odd raw dimensions");
    const bool extended = EW != DW || EH != DH;
    std::vector<uint16_t> px_full = px;
    if (EW != NW || EH != NH) {  // the DNG holds the cropped region
        std::vector<uint16_t> crop(size_t(EW) * EH);
        for (unsigned y = 0; y < EH; y++)
            std::copy_n(&px_full[size_t(y + cy) * NW + cx], EW, &crop[size_t(y) * EW]);
        px = std::move(crop);
    }
    W = EW; H = EH;
    res.width = W; res.height = H;

    // black level for the companding curve: Adobe's (e.g. 0 for older bodies)
    unsigned black = info.black ? info.black : 1008;
    if (raw.tags.count(0xc61a)) {
        auto v = t.reals(raw.tags[0xc61a]);
        double sum = 0;
        for (double x : v) sum += x;
        if (!v.empty()) black = unsigned(std::lround(sum / v.size())) >> scale_shift;
    }

    // 2. companding + Bayer plane interleave
    auto tr = he_transfer(black);
    auto inv = he_inverse(tr);
    const unsigned h2 = H / 2, w2 = W / 2;
    std::vector<uint16_t> stored(size_t(W) * H);
    for (unsigned y = 0; y < H; y++)
        for (unsigned x = 0; x < W; x++) {
            unsigned sy = (y & 1) * h2 + y / 2, sx = (x & 1) * w2 + x / 2;
            stored[size_t(sy) * W + sx] = inv[std::min<unsigned>(px[size_t(y) * W + x], 16383)];
        }

    // 3. JPEG XL tiles
    const unsigned T = o.tile, tx = (W + T - 1) / T, ty = (H + T - 1) / T;
    auto make_tile = [&](unsigned i) {
        unsigned yy = i / tx, xx = i % tx;
        std::vector<uint16_t> tile(size_t(T) * T);
        for (unsigned y = 0; y < T; y++) {
            unsigned sy = std::min(yy * T + y, H - 1);  // replicate edges into padding
            for (unsigned x = 0; x < T; x++) tile[size_t(y) * T + x] = stored[size_t(sy) * W + std::min(xx * T + x, W - 1)];
        }
        return tile;
    };
    float distance = o.distance;
    if (!o.lossless && o.target_bpp > 0) {
        // estimate on a sample of full tiles, bisect log(distance)
        std::vector<unsigned> sample;
        for (unsigned i = 0; i < tx * ty; i++)
            if ((i / tx + 1) * T <= H && (i % tx + 1) * T <= W && i % 5 == 2) sample.push_back(i);
        if (sample.empty()) sample.push_back(0);
        std::vector<std::vector<uint16_t>> tiles_s;
        for (unsigned i : sample) tiles_s.push_back(make_tile(i));
        double lo = std::log(0.003), hi = std::log(3.0);
        for (int it = 0; it < 12; it++) {
            double mid = (lo + hi) / 2;
            std::atomic<size_t> bytes{0};
            parallel_for(unsigned(sample.size()), o.threads, [&](unsigned k) {
                bytes += jxl_encode_gray16(tiles_s[k].data(), T, T, float(std::exp(mid)), false, o.effort).size();
            });
            double bpp = bytes * 8.0 / (double(sample.size()) * T * T);
            if (bpp > o.target_bpp) lo = mid; else hi = mid;
        }
        distance = float(std::exp(hi));
    }
    res.distance = o.lossless ? 0 : distance;
    std::vector<std::vector<uint8_t>> tiles(size_t(tx) * ty);
    std::vector<uint16_t> dec_stored(size_t(W) * H);
    parallel_for(tx * ty, o.threads, [&](unsigned i) {
        auto tile = make_tile(i);
        tiles[i] = jxl_encode_gray16(tile.data(), T, T, distance, o.lossless, o.effort);
        std::vector<uint16_t> d;
        if (!jxl_decode_gray16(tiles[i].data(), tiles[i].size(), T, T, d)) throw std::runtime_error("JXL self-decode failed");
        unsigned yy = i / tx, xx = i % tx;
        for (unsigned y = 0; y < T && yy * T + y < H; y++)
            for (unsigned x = 0; x < T && xx * T + x < W; x++) dec_stored[size_t(yy * T + y) * W + xx * T + x] = d[size_t(y) * T + x];
    });
    for (auto &tl : tiles) res.jxl_bytes += tl.size();
    double se = 0;
    for (unsigned y = 0; y < H; y++)
        for (unsigned x = 0; x < W; x++) {
            unsigned sy = (y & 1) * h2 + y / 2, sx = (x & 1) * w2 + x / 2;
            int d = int(tr[dec_stored[size_t(sy) * W + sx]]) - int(px[size_t(y) * W + x]);
            se += double(d) * d;
            res.max_err = std::max(res.max_err, std::abs(d));
        }
    res.rms = std::sqrt(se / (double(W) * H));
    res.psnr = res.rms > 0 ? 20 * std::log10((16383.0 - black) / res.rms) : INFINITY;

    // 4. rewrite the raw IFD
    TiffWriter w(le);
    auto shorts = [&](uint16_t v) { TEntry x{3, 1, {}}; w.p16(x.data, v); return x; };
    for (uint16_t k : {0x111, 0x116, 0x117}) raw.tags.erase(k);
    raw.blocks.clear();
    raw.blocks[0x144] = tiles;
    raw.tags[0x103] = shorts(kJxlCompression);
    raw.tags[0x142] = w.longs({T});
    raw.tags[0x143] = w.longs({T});
    if (extended) {
        raw.tags[0x100] = w.longs({W});
        raw.tags[0x101] = w.longs({H});
        if (!raw.tags.count(0xc68d)) raw.tags[0xc68d] = w.longs({0, 0, DH, DW});  // ActiveArea: top, left, bottom, right
    }
    raw.tags[0xc71f] = shorts(2);   // RowInterleaveFactor
    raw.tags[0xcd43] = shorts(2);   // ColumnInterleaveFactor
    {
        TEntry f{11, 1, {}};
        float dv = res.distance;
        uint32_t bits;
        std::memcpy(&bits, &dv, 4);
        w.p32(f.data, bits);
        raw.tags[0xcd44] = f;       // JXLDistance
    }
    raw.tags[0xcd45] = w.longs({uint32_t(o.effort)});  // JXLEffort
    {
        TEntry lt{3, 65536, {}};
        for (uint16_t v : tr) w.p16(lt.data, uint16_t(v << scale_shift));  // Adobe's value scale
        raw.tags[0xc618] = lt;      // LinearizationTable
    }
    {
        std::vector<uint8_t> digests;
        for (auto &tl : tiles) { auto m = md5(tl.data(), tl.size()); digests.insert(digests.end(), m.begin(), m.end()); }
        auto m = md5(digests.data(), digests.size());
        ifd0.tags[0xc7a7] = TEntry{1, 16, std::vector<uint8_t>(m.begin(), m.end())};  // NewRawImageDigest
        ifd0.tags.erase(0xc71c);                                                       // legacy RawImageDigest
    }
    ifd0.tags[0xc612] = TEntry{1, 4, {1, 7, 0, 0}};  // DNGVersion
    ifd0.tags[0xc613] = TEntry{1, 4, {1, 7, 0, 0}};  // DNGBackwardVersion

    // 5. the rest of the NEF
    std::vector<uint8_t> mn = ifd0.tags.count(0xc634) ? dng_private_makernote(ifd0.tags[0xc634].data) : std::vector<uint8_t>{};
    EmbedStats es;
    RawCrop crop{cx, cy, W, H};
    auto blob = embed_build(nef, px_full, mn, o.keep_previews, &es, lossy_curve.empty() ? nullptr : &lossy_curve,
                            (W != NW || H != NH) ? &crop : nullptr, lossy_codes.empty() ? nullptr : &lossy_codes,
                            o.lossless);
    res.skeleton_bytes = blob.size();
    res.previews_dropped = es.previews_dropped;
    res.previews_kept_bytes = es.previews_kept;
    ifd0.tags[kSkelTag] = TEntry{7, uint32_t(blob.size()), std::move(blob)};

    uint32_t first = w.chain(chain);
    w.s32(4, first);
    res.dng = std::move(w.out);
    return res;
}

RestoreResult jxl_dng_to_nef(const std::vector<uint8_t> &dng, unsigned threads, bool original_format) {
    RestoreResult rr;
    Buf b;
    b.d = dng;
    if (b.d.size() < 8) throw std::runtime_error("not a DNG");
    bool le = b.d[0] == 'I';
    Tiff t{b, le};
    std::vector<TNode> chain = t.chain(b.u32(4, le));
    TNode &ifd0 = chain[0];
    if (!ifd0.tags.count(kSkelTag)) throw std::runtime_error("this DNG has no embedded NEF (not written by nefcomp --dng)");
    if (ifd0.tags.count(0xc68b)) {
        auto &n = ifd0.tags[0xc68b].data;
        rr.original_name.assign(n.begin(), std::find(n.begin(), n.end(), 0));
    }
    TNode &raw = *find_raw(chain, t);
    unsigned W = 0, H = 0;
    std::vector<uint16_t> px = decode_jxl_raw(t, raw, threads, W, H);
    std::vector<uint8_t> mn = ifd0.tags.count(0xc634) ? dng_private_makernote(ifd0.tags[0xc634].data) : std::vector<uint8_t>{};
    RenderParams rp = render_params(t, ifd0, raw);
    rp.threads = threads;
    EmbedRestore ei;
    rr.nef = embed_restore(ifd0.tags[kSkelTag].data, px, mn, rp, &ei, !original_format);
    rr.converted_to_lossless = ei.converted_to_lossless;
    rr.original_format = ei.original_format;
    rr.pixels_exact = ei.pixels_exact;
    rr.file_exact = ei.file_exact;
    rr.previews_regenerated = ei.previews_regenerated;
    rr.original_size = ei.original_size;
    return rr;
}

} // namespace nefcomp
