// SPDX-License-Identifier: GPL-2.0-or-later
// NEF -> JPEG XL raw (CFA) DNG with the NEF's own data embedded, and back.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace nefcomp {

struct DngOptions {
    float distance = 0.056f;    // JPEG XL distance (see quality_to_distance; q70)
    bool lossless = false;      // lossless JPEG XL raw data
    double target_bpp = 0;      // >0: pick the distance that gives ~this many bits per raw pixel
    int effort = 7;             // JPEG XL effort 1..9 (slower = smaller)
    bool keep_previews = false; // store the camera JPEG previews (lossless JPEG recompression)
    unsigned threads = 0;
    unsigned tile = 512;
    std::string dng_converter;  // path to "Adobe DNG Converter.exe" (empty = default location)
};

// nefcomp quality scale: 100 = lossless, 70 ~ HE* quality, 55 ~ HE quality, lower = smaller.
float quality_to_distance(double q);

struct DngResult {
    std::vector<uint8_t> dng;
    size_t jxl_bytes = 0, skeleton_bytes = 0;
    size_t previews_dropped = 0, previews_kept_bytes = 0;  // original preview bytes dropped / kept
    float distance = 0;
    double psnr = 0, rms = 0;   // reconstruction error vs the NEF's samples (14-bit)
    int max_err = 0;
    unsigned width = 0, height = 0;
};

DngResult nef_to_jxl_dng(const std::string &nef_path, const DngOptions &opt);

struct RestoreResult {
    std::vector<uint8_t> nef;
    std::string original_name;
    bool pixels_exact = false, file_exact = false;
    unsigned previews_regenerated = 0;
    uint64_t original_size = 0;
};

// Rebuild the Nikon NEF from a DNG written by nef_to_jxl_dng.
RestoreResult jxl_dng_to_nef(const std::vector<uint8_t> &dng, unsigned threads = 0);

// Decode the raw samples of a JPEG XL DNG (as written by nef_to_jxl_dng).
std::vector<uint16_t> jxl_dng_decode(const std::vector<uint8_t> &dng, unsigned &W, unsigned &H, unsigned threads = 0);

std::string default_dng_converter();

} // namespace nefcomp
