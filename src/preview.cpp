// SPDX-License-Identifier: GPL-2.0-or-later
#include "preview.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <thread>

#include <jpeglib.h>

namespace nefcomp {

namespace {

// XYZ (D50) -> linear sRGB (Bradford-adapted)
constexpr double kSaturation = 1.35;  // look tuned against Nikon preview JPEGs
constexpr double kGain = 1.7;

const double kXyzD50ToSrgb[9] = {3.1338561, -1.6168667, -0.4906146, -0.9787684, 1.9161415,
                                 0.0334540, 0.0719453,  -0.2289914, 1.4052427};

struct Pipeline {
    double m[9];   // camera (black-subtracted, normalised) -> linear sRGB, incl. WB and exposure
    double inv_neutral[3];
    uint8_t lut[4097];
    explicit Pipeline(const RenderParams &p) {
        double gain = std::pow(2.0, p.exposure_ev) * kGain;
        double wb[9] = {0};
        for (int i = 0; i < 3; i++) { inv_neutral[i] = 1.0 / std::max(1e-6, p.neutral[i]); wb[i * 4] = inv_neutral[i]; }
        double t[9];
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 3; c++) {
                t[r * 3 + c] = 0;
                for (int k = 0; k < 3; k++) t[r * 3 + c] += kXyzD50ToSrgb[r * 3 + k] * p.fm[k * 3 + c];
            }
        for (int i = 0; i < 9; i++) m[i] = t[i] * gain;
        // tone + sRGB transfer, indexed by linear value * 4096 (clamped)
        for (int i = 0; i <= 4096; i++) {
            double x = i / 4096.0 * 2.0;  // lut covers 0..2 (headroom before the shoulder)
            // filmic shoulder (ACES-like fit), normalised so that 2.0 -> ~1
            double y = (x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14);
            y = std::clamp(y / 0.9, 0.0, 1.0);
            y = y * y * (3 - 2 * y) * 0.35 + y * 0.65;  // extra midtone contrast
            double s = y <= 0.0031308 ? 12.92 * y : 1.055 * std::pow(y, 1 / 2.4) - 0.055;
            lut[i] = uint8_t(std::lround(std::clamp(s, 0.0, 1.0) * 255.0));
        }
    }
    inline void apply(double r, double g, double b, uint8_t *out) const {
        // white balance with highlight clipping to keep clipped areas neutral
        r = std::min(r * inv_neutral[0], 1.0);
        g = std::min(g * inv_neutral[1], 1.0);
        b = std::min(b * inv_neutral[2], 1.0);
        double c[3] = {r, g, b};
        double s[3];
        for (int k = 0; k < 3; k++) {
            s[k] = 0;
            for (int j = 0; j < 3; j++) s[k] += m[k * 3 + j] * c[j];
        }
        // camera-JPEG-like saturation boost around luminance
        double y = 0.2126 * s[0] + 0.7152 * s[1] + 0.0722 * s[2];
        for (int k = 0; k < 3; k++) {
            double v = y + (s[k] - y) * kSaturation;
            int idx = int(std::clamp(v, 0.0, 2.0) * 2048.0 + 0.5);
            out[k] = lut[idx];
        }
    }
};

template <class F> void parallel_rows(unsigned n, unsigned threads, F f) {
    unsigned nt = threads ? threads : std::max(1u, std::thread::hardware_concurrency());
    nt = std::max(1u, std::min(nt, n));
    std::atomic<unsigned> next{0};
    auto work = [&]() { for (unsigned i; (i = next++) < n;) f(i); };
    std::vector<std::thread> pool;
    for (unsigned i = 1; i < nt; i++) pool.emplace_back(work);
    work();
    for (auto &t : pool) t.join();
}

} // namespace

std::vector<uint8_t> render_preview(const uint16_t *raw, unsigned W, unsigned H, const RenderParams &p, unsigned ow,
                                    unsigned oh) {
    unsigned cx = p.crop_x, cy = p.crop_y, cw = p.crop_w ? p.crop_w : W, ch = p.crop_h ? p.crop_h : H;
    if (cx + cw > W || cy + ch > H) { cx = cy = 0; cw = W; ch = H; }
    Pipeline pl(p);
    const double scale = 1.0 / std::max(1.0, p.white - p.black);
    auto norm = [&](unsigned x, unsigned y) {
        return std::max(0.0, (double(raw[size_t(y) * W + x]) - p.black) * scale);
    };
    std::vector<uint8_t> out(size_t(ow) * oh * 3);
    if (ow * 2 > cw) {
        // near full size: bilinear demosaic, nearest sampling of the crop
        parallel_rows(oh, p.threads, [&](unsigned oy) {
            unsigned y = cy + unsigned(uint64_t(oy) * ch / oh);
            y = std::clamp(y, 1u, H - 2);
            for (unsigned ox = 0; ox < ow; ox++) {
                unsigned x = cx + unsigned(uint64_t(ox) * cw / ow);
                x = std::clamp(x, 1u, W - 2);
                bool ry = !(y & 1), rx = !(x & 1);  // RGGB
                double r, g, b;
                double c = norm(x, y);
                double n4 = (norm(x - 1, y) + norm(x + 1, y) + norm(x, y - 1) + norm(x, y + 1)) / 4;
                double d4 = (norm(x - 1, y - 1) + norm(x + 1, y - 1) + norm(x - 1, y + 1) + norm(x + 1, y + 1)) / 4;
                double h2 = (norm(x - 1, y) + norm(x + 1, y)) / 2, v2 = (norm(x, y - 1) + norm(x, y + 1)) / 2;
                if (ry && rx) { r = c; g = n4; b = d4; }
                else if (!ry && !rx) { b = c; g = n4; r = d4; }
                else if (ry) { g = c; r = h2; b = v2; }   // G on red row
                else { g = c; b = h2; r = v2; }           // G on blue row
                pl.apply(r, g, b, &out[(size_t(oy) * ow + ox) * 3]);
            }
        });
        return out;
    }
    // downscaled: area-average Bayer quads
    parallel_rows(oh, p.threads, [&](unsigned oy) {
        unsigned y0 = cy + unsigned(uint64_t(oy) * ch / oh) & ~1u, y1 = cy + unsigned(uint64_t(oy + 1) * ch / oh) & ~1u;
        y1 = std::max(y1, y0 + 2);
        y1 = std::min(y1, H & ~1u);
        for (unsigned ox = 0; ox < ow; ox++) {
            unsigned x0 = cx + unsigned(uint64_t(ox) * cw / ow) & ~1u, x1 = cx + unsigned(uint64_t(ox + 1) * cw / ow) & ~1u;
            x1 = std::max(x1, x0 + 2);
            x1 = std::min(x1, W & ~1u);
            double r = 0, g = 0, b = 0;
            unsigned n = 0;
            for (unsigned y = y0; y + 1 < y1 + 1 && y < y1; y += 2)
                for (unsigned x = x0; x < x1; x += 2) {
                    r += norm(x, y);
                    g += norm(x + 1, y) + norm(x, y + 1);
                    b += norm(x + 1, y + 1);
                    n++;
                }
            n = std::max(n, 1u);
            pl.apply(r / n, g / (2 * n), b / n, &out[(size_t(oy) * ow + ox) * 3]);
        }
    });
    return out;
}

static std::vector<uint8_t> encode_jpeg_q(const uint8_t *rgb, unsigned w, unsigned h, int quality) {
    jpeg_compress_struct ci;
    jpeg_error_mgr jerr;
    ci.err = jpeg_std_error(&jerr);
    jerr.error_exit = [](j_common_ptr c) {
        char msg[JMSG_LENGTH_MAX];
        (*c->err->format_message)(c, msg);
        throw std::runtime_error(std::string("libjpeg: ") + msg);
    };
    jpeg_create_compress(&ci);
    unsigned char *mem = nullptr;
    unsigned long mem_size = 0;
    jpeg_mem_dest(&ci, &mem, &mem_size);
    ci.image_width = w;
    ci.image_height = h;
    ci.input_components = 3;
    ci.in_color_space = JCS_RGB;
    jpeg_set_defaults(&ci);
    jpeg_set_quality(&ci, quality, TRUE);
    ci.write_JFIF_header = FALSE;
    ci.comp_info[0].h_samp_factor = 2;  // 4:2:2 like Nikon's previews
    ci.comp_info[0].v_samp_factor = 1;
    ci.comp_info[1].h_samp_factor = ci.comp_info[1].v_samp_factor = 1;
    ci.comp_info[2].h_samp_factor = ci.comp_info[2].v_samp_factor = 1;
    ci.dct_method = JDCT_ISLOW;
    jpeg_start_compress(&ci, TRUE);
    while (ci.next_scanline < h) {
        JSAMPROW row = const_cast<JSAMPROW>(rgb + size_t(ci.next_scanline) * w * 3);
        jpeg_write_scanlines(&ci, &row, 1);
    }
    jpeg_finish_compress(&ci);
    std::vector<uint8_t> out(mem, mem + mem_size);
    jpeg_destroy_compress(&ci);
    free(mem);
    return out;
}

std::vector<uint8_t> encode_jpeg_fit(const uint8_t *rgb, unsigned w, unsigned h, size_t max_bytes) {
    if (!max_bytes) return encode_jpeg_q(rgb, w, h, 90);
    std::vector<uint8_t> best;
    int lo = 10, hi = 95;  // find the highest quality that fits
    while (lo <= hi) {
        int q = (lo + hi) / 2;
        auto j = encode_jpeg_q(rgb, w, h, q);
        if (j.size() <= max_bytes) { best = std::move(j); lo = q + 1; }
        else hi = q - 1;
    }
    if (best.empty()) {
        best = encode_jpeg_q(rgb, w, h, 5);
        if (best.size() > max_bytes) throw std::runtime_error("cannot fit regenerated preview into its slot");
    }
    return best;
}

} // namespace nefcomp
