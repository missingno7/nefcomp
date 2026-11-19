// SPDX-License-Identifier: GPL-2.0-or-later
// nefcomp - shrink Nikon NEF files after shooting:
//   * lossless NEF -> High Efficiency (HE / HE*) NEF   (verified: every HE camera, Z 8 ... ZR)
//   * any NEF      -> JPEG XL raw DNG that can be turned back into the NEF
#include "dng_jxl.h"
#include "he_decode.h"
#include "nef_convert.h"
#include "nef_rebuild.h"
#include "platform.h"
#include "tiff.h"


#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

using namespace nefcomp;
namespace fs = std::filesystem;

#ifndef NEFCOMP_VERSION
#define NEFCOMP_VERSION "dev"
#endif

static const char *kUsage =
    "nefcomp " NEFCOMP_VERSION " - shrink Nikon NEF raw files after shooting\n"
    "\n"
    "usage:\n"
    "  nefcomp [options] <file.NEF | directory> ...    lossless NEF -> HE / HE* NEF; HE* -> HE (-m he)\n"
    "  nefcomp --dng [options] <file.NEF|NRW | dir>    NEF -> JPEG XL raw DNG (restorable)\n"
    "  nefcomp restore [options] <file.dng | dir> ...  DNG made by --dng -> genuine Nikon NEF:\n"
    "                                                  lossless NEF by default (no extra loss, editable\n"
    "                                                  in camera; uncompressed originals and lossy ones\n"
    "                                                  from bodies without lossless keep their format);\n"
    "                                                  --as original = original NEF format\n"
    "  nefcomp info <file.NEF> ...\n"
    "  nefcomp --version\n"
    "\n"
    "options:\n"
    "  -m, --mode MODE        hestar (default) | he | lossless (lossless: --dng only)\n"
    "      --dng              write a JPEG XL compressed DNG instead of an HE NEF; works for\n"
    "                         any camera Adobe DNG Converter supports (needs it installed)\n"
    "  DNG options:\n"
    "  -q, --quality Q        1..100: 100 = lossless, 70 = hestar (default), 55 = he\n"
    "      --bpp B            instead of -q: target size, B bits per raw pixel on average\n"
    "                         (not bit depth; data stays 14-bit). HE* = 5, HE = 3\n"
    "      --effort E         JPEG XL effort 1..9 (default 7)\n"
    "      --keep-previews    store the camera's JPEG previews (losslessly recompressed);\n"
    "                         with -m lossless, restore then gives a byte-identical NEF.\n"
    "                         Default: drop them, restore renders new ones from the raw\n"
    "      --distance D       raw JPEG XL distance (expert; overrides --quality)\n"
    "      --dng-converter P  path to Adobe DNG Converter.exe\n"
    "  -o, --output PATH      output file (single input) or directory\n"
    "      --suffix TEXT      output name suffix (default _HEs / _HE for NEF, none for DNG)\n"
    "      --verify           report the reconstruction error against the original samples\n"
    "      --overwrite        replace existing output files (default: skip them)\n"
    "      --force            write a DNG even if it is not smaller than the original NEF\n"
    "      --allow-untested   allow HE NEF output for HE cameras not yet verified\n"
    "  -j, --threads N        encoder threads (default: all cores)\n";

struct Options {
    HeMode mode = HeMode::HEStar;
    bool lossless = false, dng = false;
    float distance = -1;
    double quality = -1, bpp = 0;
    int effort = 7;
    bool keep_previews = false;
    std::string output, suffix, dng_converter;
    bool verify = false, overwrite = false, allow_untested = false, suffix_set = false;
    bool auto_mode = false, recursive = false, force = false, restore_original = false;
    unsigned threads = 0;
};

static std::string lower_ext(const fs::path &p) {
    std::string e = p.extension().u8string();
    for (auto &c : e) c = char(std::tolower(static_cast<unsigned char>(c)));
    return e;
}

static bool raw_input_ext(const fs::path &p) {
    std::string e = lower_ext(p);
    return e == ".nef" || e == ".nrw";
}

// Extension for a restored raw: ".NRW" when the DNG's OriginalRawFileName says
// the camera wrote an NRW (Coolpix), else ".NEF". Reads only the header, IFD0
// and that tag (they can lie anywhere in the file).
static std::string restored_ext(const fs::path &dng) {
    FILE *f = platform::open_file(dng, "rb");
    if (!f) return ".NEF";
    std::string ext = ".NEF";
    auto rd = [&](uint64_t at, size_t n, std::vector<uint8_t> &v) {
        v.resize(n);
        return platform::seek64(f, int64_t(at), SEEK_SET) == 0 && fread(v.data(), 1, n, f) == n;
    };
    std::vector<uint8_t> h, cnt, ents, val;
    if (rd(0, 8, h) && (h[0] == 'I' || h[0] == 'M')) {
        const bool le = h[0] == 'I';
        auto u16 = [&](const uint8_t *p) { return le ? unsigned(p[0] | p[1] << 8) : unsigned(p[0] << 8 | p[1]); };
        auto u32 = [&](const uint8_t *p) {
            return le ? uint32_t(p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24)
                      : uint32_t(uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]);
        };
        const uint32_t ifd0 = u32(&h[4]);
        if (rd(ifd0, 2, cnt) && rd(ifd0 + 2, size_t(u16(cnt.data())) * 12, ents)) {
            for (size_t i = 0; i + 12 <= ents.size(); i += 12) {
                const uint8_t *e = &ents[i];
                const unsigned type = u16(e + 2);
                const uint32_t count = u32(e + 4);
                if (u16(e) != 0xc68b || (type != 1 && type != 2) || count == 0 || count > 4096) continue;
                std::string n;
                if (count <= 4) n.assign(reinterpret_cast<const char *>(e + 8), count);
                else if (rd(u32(e + 8), count, val)) n.assign(val.begin(), val.end());
                n = n.c_str();
                // byte-wise check: the name's encoding does not matter
                if (n.size() >= 4) {
                    std::string tail = n.substr(n.size() - 4);
                    for (auto &c : tail) c = char(std::tolower(uint8_t(c)));
                    if (tail == ".nrw") ext = ".NRW";
                }
                break;
            }
        }
    }
    fclose(f);
    return ext;
}

static void write_atomic(const fs::path &out, const std::vector<uint8_t> &data) {
    std::string tmp = out.u8string() + ".part";
    write_file(tmp, data);
    std::error_code ec;
    fs::rename(fs::u8path(tmp), out, ec);
    if (ec) { fs::remove(out); fs::rename(fs::u8path(tmp), out); }
}

static std::string nef_format_name(unsigned code);

static int cmd_info(const std::vector<std::string> &files) {
    int rc = 0;
    for (auto &f : files) {
        std::vector<uint8_t> d;
        auto e = read_file(f, d);
        if (!e.empty()) { fprintf(stderr, "%s: %s\n", f.c_str(), e.c_str()); rc = 1; continue; }
        try {
            NefInfo i = inspect_nef(d);
            printf("%s: %s %s (%s), %ux%u %u-bit, %s (code %u), black %u, raw %.1f MB of %.1f MB\n", f.c_str(),
                   i.make.c_str(), i.model.c_str(), i.firmware.c_str(), i.width, i.height, i.bps,
                   i.compression_code ? nef_format_name(i.compression_code).c_str() : compression_name(i.compression),
                   i.compression_code, i.black, i.strip_len / 1e6, i.file_size / 1e6);
        } catch (const std::exception &ex) {
            fprintf(stderr, "%s: %s\n", f.c_str(), ex.what());
            rc = 1;
        }
    }
    return rc;
}

static size_t convert_he(const fs::path &in, const fs::path &out, const Options &o) {
    auto t0 = std::chrono::steady_clock::now();
    std::vector<uint8_t> d;
    auto e = read_file(in.u8string(), d);
    if (!e.empty()) throw std::runtime_error(e);
    HeEncodeOptions opt;
    opt.mode = o.mode;
    opt.threads = o.threads;
    ConvertResult r = convert_lossless_to_he(d, opt, o.allow_untested);
    for (auto &w : r.warnings) printf("  warning: %s: %s\n", in.u8string().c_str(), w.c_str());
    std::string verify_msg;
    if (o.verify) {
        Buf b;
        b.d = r.output;
        NefLayout L = parse_nef(b);
        std::vector<uint16_t> dec;
        std::string err;
        if (!he_decode(&b.d[L.strip_off], L.strip_len, L.width, L.height, dec, err))
            throw std::runtime_error("VERIFY FAILED: " + err + " (output not written)");
        double se = 0;
        int maxe = 0;
        for (size_t i = 0; i < dec.size(); i++) {
            int dd = int(dec[i]) - int(r.source_cfa[i]);
            se += double(dd) * dd;
            maxe = std::max(maxe, std::abs(dd));
        }
        double rms = std::sqrt(se / dec.size()), peak = 16383.0 - r.black;
        char buf[160];
        snprintf(buf, sizeof buf, ", verify: PSNR %.2f dB, rms %.2f, max %d", 20 * std::log10(peak / rms), rms, maxe);
        verify_msg = buf;
    }
    write_atomic(out, r.output);
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    printf("  HE   %s -> %s: %.1f MB -> %.1f MB (%s)%s [%.1fs]\n", in.u8string().c_str(), out.u8string().c_str(),
           d.size() / 1e6, r.output.size() / 1e6, o.mode == HeMode::HE ? "HE" : "HE*", verify_msg.c_str(), secs);
    return r.output.size();
}

struct SkipError : std::runtime_error { using std::runtime_error::runtime_error; };

static size_t convert_dng(const fs::path &in, const fs::path &out, const Options &o) {
    auto t0 = std::chrono::steady_clock::now();
    DngOptions opt;
    double q = o.quality >= 0 ? o.quality : o.lossless ? 100 : (o.mode == HeMode::HE ? 55 : 70);
    opt.lossless = q >= 100 && o.distance < 0 && o.bpp <= 0;
    opt.distance = o.distance >= 0 ? o.distance : quality_to_distance(q);
    opt.target_bpp = o.bpp;
    opt.effort = o.effort;
    opt.keep_previews = o.keep_previews;
    opt.threads = o.threads;
    opt.dng_converter = o.dng_converter;
    DngResult r = nef_to_jxl_dng(in.u8string(), opt);
    if (r.dng.size() >= fs::file_size(in)) {
        char why[200];
        snprintf(why, sizeof why, "DNG would not be smaller (%.1f MB -> %.1f MB); lower -q / --bpp, or --force",
                 double(fs::file_size(in)) / 1e6, r.dng.size() / 1e6);
        if (!o.force) throw SkipError(why);
        printf("  warning: %s: %s\n", in.u8string().c_str(), why);
    }
    write_atomic(out, r.dng);
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    char qs[64];
    if (opt.lossless) snprintf(qs, sizeof qs, "lossless");
    else snprintf(qs, sizeof qs, "distance %.3f, %.2f bpp", r.distance, r.jxl_bytes * 8.0 / (double(r.width) * r.height));
    char v[160] = "";
    if (o.verify) {
        if (r.rms == 0) snprintf(v, sizeof v, ", verify: EXACT");
        else snprintf(v, sizeof v, ", verify: PSNR %.2f dB, rms %.2f, max %d", r.psnr, r.rms, r.max_err);
    }
    printf("  DNG  %s -> %s: %.1f MB -> %.1f MB (JPEG XL %s; raw %.1f MB + NEF data %.2f MB, previews %s)%s [%.1fs]\n",
           in.u8string().c_str(), out.u8string().c_str(), double(fs::file_size(in)) / 1e6, r.dng.size() / 1e6, qs,
           r.jxl_bytes / 1e6, r.skeleton_bytes / 1e6, o.keep_previews ? "kept" : "regenerated on restore", v, secs);
    return r.dng.size();
}

static size_t restore_one(const fs::path &in, const fs::path &out, const Options &o) {
    auto t0 = std::chrono::steady_clock::now();
    std::vector<uint8_t> d;
    auto e = read_file(in.u8string(), d);
    if (!e.empty()) throw std::runtime_error(e);
    RestoreResult r = jxl_dng_to_nef(d, o.threads, o.restore_original);
    write_atomic(out, r.nef);
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::string what;
    if (r.file_exact) what = "byte-identical to the original NEF";
    else {
        what = r.converted_to_lossless ? "lossless NEF (original was " + std::string(r.original_format) + ")"
               : std::string(r.original_format) == "lossless" ? std::string("lossless NEF")
                                                               : "NEF in its original " + std::string(r.original_format) + " format";
        what += r.pixels_exact ? ", raw identical to the original" : ", raw as compressed in the DNG";
        if (r.previews_regenerated) what += ", previews regenerated";
    }
    printf("  NEF  %s -> %s: %.1f MB (original %s %.1f MB): %s [%.1fs]\n", in.u8string().c_str(), out.u8string().c_str(),
           r.nef.size() / 1e6, r.original_name.c_str(), r.original_size / 1e6, what.c_str(), secs);
    return r.nef.size();
}

// ------------------------------------------------------------------ batch
enum class Act { HE, DNG, Restore, Skip };
struct Job {
    fs::path in, root, out;
    Act act = Act::Skip;
    std::string why;
};

static std::vector<uint8_t> read_prefix(const fs::path &p, size_t n) {
    std::vector<uint8_t> v;
    FILE *f = platform::open_file(p, "rb");
    if (!f) throw std::runtime_error("cannot open file");
    v.resize(n);
    v.resize(fread(v.data(), 1, n, f));
    fclose(f);
    return v;
}

static std::string nef_format_name(unsigned code) {
    switch (code) {
    case 1: return "lossy compressed (type 1)";
    case 2: return "uncompressed";
    case 3: return "lossless compressed";
    case 4: return "lossy compressed";
    case 5: case 6: case 7: case 9: return "uncompressed 12-bit";
    case 8: return "small-size NEF";
    case 10: return "uncompressed";
    case 13: return "High Efficiency";
    case 14: return "High Efficiency*";
    default: return "NEF compression " + std::to_string(code);
    }
}

static void plan_nef(Job &j, const Options &o) {
    NefInfo info;
    NefRaw raw;
    try {
        try {
            auto pre = read_prefix(j.in, 32u << 20);  // metadata lives before the raw strip
            info = inspect_nef(pre, false);
            raw = describe_raw(pre, false);
        } catch (const std::exception &) {
            std::vector<uint8_t> full;
            auto e = read_file(j.in.u8string(), full);
            if (!e.empty()) throw std::runtime_error(e);
            info = inspect_nef(full);
            raw = describe_raw(full);
        }
    } catch (const std::exception &ex) {
        j.why = std::string("not a readable Nikon NEF (") + ex.what() + ")";
        return;
    }
    const bool is_he = raw.format == RawFormat::HE || raw.format == RawFormat::HEStar;
    const bool lossless = raw.format == RawFormat::Lossless;
    const bool to_dng_ok = lossless || raw.format == RawFormat::Uncompressed || raw.format == RawFormat::Lossy || is_he;
    const bool he_ok = he_verified_model(info.model) || (o.allow_untested && he_capable_model(info.model));
    // HE* -> HE is a valid step down; HE -> HE* would only grow the file
    const bool downgrade = raw.format == RawFormat::HEStar && o.mode == HeMode::HE && he_ok;
    std::string fmt = raw.why.empty() ? nef_format_name(info.compression_code) : raw.why;
    auto already = [&]() {
        j.why = raw.format == RawFormat::HE && o.mode == HeMode::HEStar && !o.dng
                    ? "already High Efficiency (smaller than HE*)"
                    : "already " + fmt;
    };
    if (o.dng) {
        if (!to_dng_ok) { j.why = "unsupported raw format (" + fmt + ")"; return; }
        j.act = Act::DNG;
    } else if (o.auto_mode) {
        if ((lossless && he_ok) || downgrade) j.act = Act::HE;
        else if (is_he) already();
        else if (to_dng_ok) j.act = Act::DNG;
        else j.why = "unsupported raw format (" + fmt + ")";
    } else {
        if (downgrade) { j.act = Act::HE; return; }
        if (is_he) { already(); return; }
        if (!lossless) { j.why = "not lossless compressed (" + fmt + "); try --dng"; return; }
        if (!he_capable_model(info.model)) { j.why = info.model + " has no HE mode; use --auto or --dng"; return; }
        if (!he_ok) { j.why = info.model + ": HE output untested (--allow-untested, or --dng)"; return; }
        j.act = Act::HE;
    }
}

static bool under(const fs::path &p, const fs::path &dir) {
    std::error_code ec;
    auto a = fs::weakly_canonical(p, ec), b = fs::weakly_canonical(dir, ec);
    auto [mi, mj] = std::mismatch(b.begin(), b.end(), a.begin(), a.end());
    return mi == b.end();
}

static int run_batch(Options &o, const std::vector<std::string> &inputs, bool restore) {
    const std::string in_ext = restore ? ".dng" : ".nef";
    // 1. collect
    std::vector<Job> jobs;
    bool any_dir = false;
    const bool out_given = !o.output.empty();
    const fs::path out_path = fs::u8path(o.output);
    for (auto &in : inputs) {
        fs::path p = fs::u8path(in);
        std::error_code ec;
        if (fs::is_directory(p, ec)) {
            any_dir = true;
            auto add = [&](const fs::directory_entry &de) {
                if (!de.is_regular_file() || (restore ? lower_ext(de.path()) != in_ext : !raw_input_ext(de.path()))) return;
                if (out_given && fs::is_directory(out_path) && under(de.path(), out_path)) return;  // our outputs
                jobs.push_back({fs::path(de.path()).lexically_normal().make_preferred(), fs::path(p).lexically_normal().make_preferred(), {}, Act::Skip, ""});
            };
            if (o.recursive)
                for (auto &de : fs::recursive_directory_iterator(p, fs::directory_options::skip_permission_denied, ec)) add(de);
            else
                for (auto &de : fs::directory_iterator(p, ec)) add(de);
        } else if (fs::exists(p, ec)) {
            { fs::path q = fs::path(p).lexically_normal().make_preferred(); jobs.push_back({q, q.parent_path(), {}, Act::Skip, ""}); }
        } else {
            fprintf(stderr, "%s: not found\n", in.c_str());
            return 2;
        }
    }
    if (jobs.empty()) { fprintf(stderr, "no %s files found%s\n", restore ? ".dng" : ".NEF/.NRW", o.recursive ? "" : " (use -r for subfolders)"); return 2; }
    std::sort(jobs.begin(), jobs.end(), [](const Job &a, const Job &b) { return a.in < b.in; });
    const bool out_is_dir = out_given && (jobs.size() > 1 || any_dir || fs::is_directory(out_path) ||
                                          o.output.back() == '\\' || o.output.back() == '/' ||
                                          out_path.extension().empty());  // no extension = folder

    // 2. plan
    for (auto &j : jobs) {
        if (restore) j.act = Act::Restore;
        else plan_nef(j, o);
        if (j.act == Act::Skip) continue;
        std::string suffix = o.suffix_set ? o.suffix
                             : j.act == Act::HE ? (o.mode == HeMode::HE ? "_HE" : "_HEs") : "";
        std::string ext = j.act == Act::HE ? j.in.extension().u8string() : j.act == Act::DNG ? ".dng" : restored_ext(j.in);
        fs::path name = fs::u8path(j.in.stem().u8string() + suffix + ext);
        if (!out_given) j.out = j.in.parent_path() / name;
        else if (out_is_dir) {
            std::error_code ec;
            fs::path rel = fs::relative(j.in.parent_path(), j.root, ec);
            if (ec || rel.empty() || *rel.begin() == "..") rel = ".";
            j.out = (out_path / rel / name).lexically_normal().make_preferred();
        } else j.out = fs::path(out_path).make_preferred();
        if (fs::exists(j.out) && !o.overwrite) { j.act = Act::Skip; j.why = "output exists: " + j.out.u8string() + " (--overwrite)"; }
    }
    unsigned n_he = 0, n_dng = 0, n_res = 0, n_skip = 0;
    for (auto &j : jobs) (j.act == Act::HE ? n_he : j.act == Act::DNG ? n_dng : j.act == Act::Restore ? n_res : n_skip)++;
    if (jobs.size() > 1) {
        printf("%zu file(s): ", jobs.size());
        if (restore) printf("%u to restore", n_res);
        else printf("%u -> HE NEF, %u -> JPEG XL DNG", n_he, n_dng);
        printf(", %u skipped\n", n_skip);
    }
    if (n_dng) {
        std::string exe = o.dng_converter.empty() ? default_dng_converter() : o.dng_converter;
        if (exe.empty()) {
            fprintf(stderr, "--dng needs Adobe DNG Converter, which exists for Windows and macOS only; "
                            "DNG jobs cannot run here (restore, info and HE conversion work)\n");
            for (auto &j : jobs) if (j.act == Act::DNG) { j.act = Act::Skip; j.why = "needs Adobe DNG Converter (Windows/macOS)"; }
        } else if (!fs::exists(fs::u8path(exe))) {
            fprintf(stderr, "Adobe DNG Converter not found at %s (install it or use --dng-converter); DNG jobs cannot run\n", exe.c_str());
            for (auto &j : jobs) if (j.act == Act::DNG) { j.act = Act::Skip; j.why = "needs Adobe DNG Converter"; }
        }
    }

    // 3. run
    unsigned ok = 0, failed = 0, skipped = 0;
    uint64_t bytes_in = 0, bytes_out = 0;
    std::vector<std::pair<std::string, unsigned>> reasons;
    auto tally = [&](const std::string &why) {
        std::string key = why.substr(0, why.find_first_of("(:;"));
        while (!key.empty() && (key.back() == ' ' || key.back() == '*')) key.pop_back();
        for (auto &r : reasons) if (r.first == key) { r.second++; return; }
        reasons.push_back({key, 1});
    };
    for (auto &j : jobs) {
        if (j.act == Act::Skip) {
            printf("  skip %s: %s\n", j.in.u8string().c_str(), j.why.c_str());
            skipped++;
            tally(j.why);
            fflush(stdout);
            continue;
        }
        try {
            if (!j.out.parent_path().empty()) fs::create_directories(j.out.parent_path());
            uint64_t in_size = fs::file_size(j.in);
            size_t out_size = j.act == Act::HE ? convert_he(j.in, j.out, o)
                              : j.act == Act::DNG ? convert_dng(j.in, j.out, o)
                                                  : restore_one(j.in, j.out, o);
            ok++;
            bytes_in += in_size;
            bytes_out += out_size;
        } catch (const SkipError &ex) {
            printf("  skip %s: %s\n", j.in.u8string().c_str(), ex.what());
            skipped++;
            tally(ex.what());
        } catch (const std::exception &ex) {
            std::string msg = ex.what();
            if (restore && msg.find("no embedded NEF") != std::string::npos) {
                printf("  skip %s: not a nefcomp DNG (no embedded NEF)\n", j.in.u8string().c_str());
                skipped++;
                tally("not a nefcomp DNG");
            } else {
                fflush(stdout);
                fprintf(stderr, "  FAIL %s: %s\n", j.in.u8string().c_str(), msg.c_str());
                fflush(stderr);
                failed++;
            }
        }
        fflush(stdout);
    }
    if (jobs.size() > 1 || skipped || failed) {
        printf("done: %u %s", ok, restore ? "restored" : "converted");
        if (ok && !restore)
            printf(" (%.1f MB -> %.1f MB, saved %.1f MB = %.0f%%)", bytes_in / 1e6, bytes_out / 1e6,
                   (double(bytes_in) - double(bytes_out)) / 1e6, bytes_in ? 100.0 * (1.0 - double(bytes_out) / bytes_in) : 0.0);
        printf(", %u skipped, %u failed\n", skipped, failed);
        for (auto &r : reasons) printf("  skipped %u x %s\n", r.second, r.first.c_str());
    }
    return failed ? 1 : 0;
}

int main(int argc, char **argv) {
    std::vector<std::string> args = platform::args_utf8(argc, argv);  // UTF-8 on every platform
    platform::console_utf8();
    if (args.empty() || args[0] == "-h" || args[0] == "--help") { fputs(kUsage, stdout); return args.empty() ? 2 : 0; }
    if (args[0] == "--version" || args[0] == "-V") { printf("nefcomp %s\n", NEFCOMP_VERSION); return 0; }
    if (args[0] == "info") return cmd_info({args.begin() + 1, args.end()});
    if (args[0] == "decode" || args[0] == "skeleton" || args[0] == "assemble") {
        try {
            if (args[0] == "decode" && args.size() == 3) {   // NEF -> raw uint16 LE samples
                std::vector<uint8_t> d;
                auto e = read_file(args[1], d);
                if (!e.empty()) throw std::runtime_error(e);
                NefRaw info;
                std::vector<uint16_t> px;
                if (lower_ext(args[1]) == ".dng") px = jxl_dng_decode(d, info.width, info.height);
                else px = decode_raw(d, &info);
                if (getenv("NEFCOMP_CODES") && info.format == RawFormat::Lossy) {  // debugging: raw int16 codes
                    std::vector<uint16_t> curve;
                    decode_raw_codes(d, px, curve);
                }
                std::vector<uint8_t> o(px.size() * 2);
                memcpy(o.data(), px.data(), o.size());
                write_file(args[2], o);
                printf("%u %u %u\n", info.width, info.height, info.black);
                return 0;
            }
            if (args[0] == "skeleton" && args.size() == 3) { // NEF -> bytes before the raw strip
                std::vector<uint8_t> d;
                auto e = read_file(args[1], d);
                if (!e.empty()) throw std::runtime_error(e);
                write_file(args[2], nef_skeleton(d));
                return 0;
            }
            if (args[0] == "assemble" && args.size() == 4) { // skeleton + raw uint16 -> NEF
                std::vector<uint8_t> sk, raw;
                auto e = read_file(args[1], sk);
                if (e.empty()) e = read_file(args[2], raw);
                if (!e.empty()) throw std::runtime_error(e);
                std::vector<uint16_t> px(raw.size() / 2);
                memcpy(px.data(), raw.data(), px.size() * 2);
                write_file(args[3], nef_assemble(sk, px));
                return 0;
            }
            fprintf(stderr, "usage: nefcomp decode in.NEF out.u16 | skeleton in.NEF out.bin | assemble skel.bin px.u16 out.NEF\n");
            return 2;
        } catch (const std::exception &ex) {
            fprintf(stderr, "%s: %s\n", args[0].c_str(), ex.what());
            return 1;
        }
    }
    bool restore = args[0] == "restore";
    if (restore) args.erase(args.begin());

    Options o;
    std::vector<std::string> inputs;
    try {
        for (size_t i = 0; i < args.size(); i++) {
            const std::string &a = args[i];
            auto value = [&]() -> std::string {
                if (i + 1 >= args.size()) throw std::runtime_error("missing value for " + a);
                return args[++i];
            };
            if (a == "-m" || a == "--mode") {
                std::string m = value();
                if (m == "he") o.mode = HeMode::HE;
                else if (m == "hestar" || m == "he*" || m == "HE*") o.mode = HeMode::HEStar;
                else if (m == "lossless") o.lossless = true;
                else throw std::runtime_error("unknown mode " + m);
            } else if (a == "--dng") o.dng = true;
            else if (a == "--auto") o.auto_mode = true;
            else if (a == "-r" || a == "--recursive") o.recursive = true;
            else if (a == "--force") o.force = true;
            else if (a == "--as") {
                std::string v = value();
                if (v == "lossless") o.restore_original = false;
                else if (v == "original") o.restore_original = true;
                else throw std::runtime_error("--as must be lossless or original");
            }
            else if (a == "-q" || a == "--quality") o.quality = std::stod(value());
            else if (a == "--bpp") o.bpp = std::stod(value());
            else if (a == "--effort") o.effort = std::clamp(std::stoi(value()), 1, 9);
            else if (a == "--keep-previews") o.keep_previews = true;
            else if (a == "--distance") o.distance = std::stof(value());
            else if (a == "--dng-converter") o.dng_converter = value();
            else if (a == "-o" || a == "--output") o.output = value();
            else if (a == "--suffix") { o.suffix = value(); o.suffix_set = true; }
            else if (a == "--verify") o.verify = true;
            else if (a == "--overwrite") o.overwrite = true;
            else if (a == "--allow-untested") o.allow_untested = true;
            else if (a == "-j" || a == "--threads") o.threads = unsigned(std::stoul(value()));
            else if (!a.empty() && a[0] == '-') throw std::runtime_error("unknown option " + a);
            else inputs.push_back(a);
        }
        if (o.lossless && !o.dng && !o.auto_mode && !restore)
            throw std::runtime_error("-m lossless is only available with --dng / --auto");
        if (o.dng && o.auto_mode) throw std::runtime_error("use either --dng or --auto");
        int quality_opts = (o.quality >= 0) + (o.bpp > 0) + (o.distance >= 0) + o.lossless;
        if (quality_opts > 1)
            throw std::runtime_error("choose one of -q (quality), --bpp (size target), --distance or -m lossless; "
                                     "they are alternatives, not combinable");
    } catch (const std::exception &ex) {
        fprintf(stderr, "%s\n\n%s", ex.what(), kUsage);
        return 2;
    }
    return run_batch(o, inputs, restore);
}
