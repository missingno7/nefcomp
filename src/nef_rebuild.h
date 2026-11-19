// SPDX-License-Identifier: GPL-2.0-or-later
// Split a NEF into "skeleton" (everything except the raw strip) + pixels, and
// rebuild a genuine-format NEF from a skeleton and (possibly modified) pixels.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace nefcomp {

enum class RawFormat { Lossless, Lossy, Uncompressed, HE, HEStar, Unsupported };

// Byte layouts of uncompressed raw data (as LibRaw reads them).
enum class UncompLayout {
    None,
    Packed14,          // Nikon 14-bit, 4 samples in 7 bytes (LSB first), rows padded to 16 bytes
    PackedMSB12,       // TIFF compression 1, 12-bit MSB-first bit packing (D1, D2H, D200, D300, ...)
    PackedMSB12W32,    // Coolpix (NRW): the same bit stream read as little-endian 32-bit words
    PackedMSB12Interlaced,  // Coolpix E5400/E8400/E8800: MSB-first 12-bit, all even rows, then all odd rows
    PackedMSB12Pad,    // D100: 12-bit MSB-first, 10 samples in 15 bytes + 1 pad byte; the row is wider
                       // than the TIFF width says (NefRaw::width is the real one)
    PackedLE12,        // Nikon 12-bit pairs: b0 | (b1&15)<<8, (b1>>4) | b2<<4
    PackedLE12Padded,  // same, rows padded to 16 bytes
    Unpacked16,        // 16-bit words in the file's byte order (TIFF compression 1, 14/16-bit)
    Unpacked16Shift4,  // big-endian 16-bit words holding a 12-bit value << 4 (Coolpix P7700, ...);
                       // nefcomp works with word >> 2 (14-bit range, NefRaw::bps = 14)
};

struct NefRaw {
    RawFormat format = RawFormat::Unsupported;
    UncompLayout layout = UncompLayout::None;
    unsigned width = 0, height = 0, bps = 0, black = 0;
    size_t strip_off = 0, strip_len = 0;
    unsigned strip_count = 1;
    std::string why;  // reason, for some unsupported files
};

// Describe the raw strip of a NEF (or of a skeleton when check_strip=false).
NefRaw describe_raw(const std::vector<uint8_t> &file, bool check_strip = true);

// Decode the raw strip of a complete NEF to samples (12/14-bit linear values).
std::vector<uint16_t> decode_raw(const std::vector<uint8_t> &file, NefRaw *info = nullptr);

// Skeleton = the file without its raw strip data. When the strip is the last
// thing in the file, the skeleton is everything before it; otherwise the
// strip's bytes are zeroed in place so data after it keeps its offsets.
std::vector<uint8_t> nef_skeleton(const std::vector<uint8_t> &file);

// Lossy NEF: raw codes (int16 stored as uint16: values below zero can occur
// after a "split" row) and the code -> value curve (as LibRaw interpolates it).
void decode_raw_codes(const std::vector<uint8_t> &file, std::vector<uint16_t> &codes, std::vector<uint16_t> &curve);

// Lossy NEFs: a curve can map several codes to one value. These list the
// pixels whose original code differs from the default (lowest) choice, so an
// exact restore can reproduce the camera's bitstream.
struct CodeOverride { uint32_t index; uint16_t code; };
std::vector<CodeOverride> lossy_code_overrides(const std::vector<uint16_t> &codes, const std::vector<uint16_t> &curve);

// Re-encode `pixels` in the skeleton's original format and put it back.
// For lossy NEFs each pixel is mapped to the nearest code of `lossy_curve`
// (default: the curve from the skeleton's makernote).
// The result is verified by decoding it again.
std::vector<uint8_t> nef_assemble(const std::vector<uint8_t> &skeleton, const std::vector<uint16_t> &pixels,
                                  const std::vector<uint16_t> *lossy_curve = nullptr,
                                  const std::vector<CodeOverride> *overrides = nullptr);

} // namespace nefcomp
