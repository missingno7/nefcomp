// SPDX-License-Identifier: GPL-2.0-or-later
// Render camera-style preview images from raw Bayer data (used to regenerate
// the NEF's embedded JPEG previews when they were not stored).
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace nefcomp {

struct RenderParams {
    double black = 1008, white = 16383;
    double neutral[3] = {0.5, 1.0, 0.5};          // AsShotNeutral (camera RGB of white)
    double fm[9] = {0.6, 0.3, 0.1, 0.25, 0.7, 0.05, 0.0, 0.1, 0.75};  // ForwardMatrix: WB'd camera -> XYZ D50
    double exposure_ev = 0;                       // BaselineExposure
    unsigned crop_x = 0, crop_y = 0, crop_w = 0, crop_h = 0;  // DefaultCrop in raw pixels (0 = full)
    unsigned threads = 0;
};

// Returns out_w*out_h*3 bytes of 8-bit sRGB. The raw is RGGB, width W, height H.
std::vector<uint8_t> render_preview(const uint16_t *raw, unsigned W, unsigned H, const RenderParams &p,
                                    unsigned out_w, unsigned out_h);

// Baseline JPEG like Nikon's previews (4:2:2, standard tables, no JFIF/Exif).
// Picks the highest quality whose size is <= max_bytes (0 = quality 90).
std::vector<uint8_t> encode_jpeg_fit(const uint8_t *rgb, unsigned w, unsigned h, size_t max_bytes);

} // namespace nefcomp
