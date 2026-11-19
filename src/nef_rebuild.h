// SPDX-License-Identifier: GPL-2.0-or-later
// Split a NEF into "skeleton" (everything before the raw strip) + pixels, and
// rebuild a genuine-format NEF from a skeleton and (possibly modified) pixels.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace nefcomp {

enum class RawFormat { Lossless, Lossy, Packed14, HE, HEStar, Unsupported };

struct NefRaw {
    RawFormat format = RawFormat::Unsupported;
    unsigned width = 0, height = 0, bps = 0, black = 0;
    size_t strip_off = 0, strip_len = 0;
};

// Describe the raw strip of a NEF (or of a skeleton when check_strip=false).
NefRaw describe_raw(const std::vector<uint8_t> &file, bool check_strip = true);

// Decode the raw strip of a complete NEF to 14-bit samples.
std::vector<uint16_t> decode_raw(const std::vector<uint8_t> &file, NefRaw *info = nullptr);

// Skeleton = file bytes before the raw strip (the strip must be last).
std::vector<uint8_t> nef_skeleton(const std::vector<uint8_t> &file);

// Lossy NEF: raw codes and the code -> value curve (Nikon's, as LibRaw interpolates it).
void decode_raw_codes(const std::vector<uint8_t> &file, std::vector<uint16_t> &codes, std::vector<uint16_t> &curve);

// Re-encode `pixels` in the skeleton's original format and append it.
// For lossy NEFs each pixel is mapped to the nearest code of `lossy_curve`
// (default: the curve from the skeleton's makernote).
// The result is verified by decoding it again (bit-exact for lossless/packed).
std::vector<uint8_t> nef_assemble(const std::vector<uint8_t> &skeleton, const std::vector<uint16_t> &pixels,
                                  const std::vector<uint16_t> *lossy_curve = nullptr);

} // namespace nefcomp
