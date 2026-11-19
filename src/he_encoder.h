// SPDX-License-Identifier: GPL-2.0-or-later
// Nikon High Efficiency (HE / HE*) RAW codestream encoder.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace nefcomp {

enum class HeMode { HE = 13, HEStar = 14 };   // values as stored in makernote 0x51

struct HeEncodeOptions {
    HeMode mode = HeMode::HEStar;
    unsigned black = 1008;      // sensor black level (14-bit units)
    unsigned threads = 0;       // 0 = hardware concurrency
    unsigned min_pad = 32;      // minimum zero padding kept in each precinct (bytes)
};

struct HeEncodeStats {
    size_t precincts = 0;
    double mean_quality_index = 0;          // mean of Q*25+(24-R); lower is better
    std::array<size_t, 16> q_histogram{};   // precincts per Q
    size_t min_pad = 0, max_pad = 0;
    double mean_pad = 0;
};

// Codestream size used by the camera for a given mode and raw geometry.
uint32_t he_codestream_size(unsigned width, unsigned height, HeMode mode);

// Encode an RGGB Bayer frame (14-bit samples, row-major, width x height) into
// a complete HE codestream (SOC ... EOC) of exactly he_codestream_size() bytes.
// Throws std::runtime_error on unsupported geometry or rate failure.
std::vector<uint8_t> he_encode(const uint16_t *cfa, unsigned width, unsigned height,
                               const HeEncodeOptions &opt, HeEncodeStats *stats = nullptr);

} // namespace nefcomp
