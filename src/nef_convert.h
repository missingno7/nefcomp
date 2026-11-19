// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "he_encoder.h"
#include <cstdint>
#include <string>
#include <vector>

namespace nefcomp {

enum class NefCompression { Unknown, Lossless, HE, HEStar, Other };

struct NefInfo {
    std::string make, model, firmware;
    unsigned width = 0, height = 0, bps = 0;
    NefCompression compression = NefCompression::Unknown;
    unsigned compression_code = 0;   // makernote 0x51 byte 10
    unsigned black = 0;
    size_t strip_off = 0, strip_len = 0, file_size = 0;
};

// check_strip=false: `file` may be just the start of the NEF (metadata only).
NefInfo inspect_nef(const std::vector<uint8_t> &file, bool check_strip = true);

// Cameras that shoot HE in camera (the only ones Adobe/LibRaw decode HE for), and
// the subset whose HE container layout nefcomp has verified.
bool he_capable_model(const std::string &model);
bool he_verified_model(const std::string &model);
const char *compression_name(NefCompression c);

struct ConvertResult {
    std::vector<uint8_t> output;
    std::vector<uint16_t> source_cfa;   // decoded lossless samples (for verification)
    unsigned width = 0, height = 0, black = 0;
    HeEncodeStats stats;
    std::vector<std::string> warnings;
};

// Convert a lossless-compressed Nikon NEF into an HE / HE* NEF, or an HE* NEF into HE.
// `allow_untested_model`: permit cameras other than the Z 8 (whose own HE files
// the container changes were derived from).
ConvertResult convert_lossless_to_he(const std::vector<uint8_t> &file, const HeEncodeOptions &opt,
                                     bool allow_untested_model);

} // namespace nefcomp
