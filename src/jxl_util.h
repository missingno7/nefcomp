// SPDX-License-Identifier: GPL-2.0-or-later
// Thin wrappers around libjxl (encode/decode single-channel images, lossless
// JPEG recompression) and brotli.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace nefcomp {

// Single-channel 16-bit image. lossless=true ignores distance.
std::vector<uint8_t> jxl_encode_gray16(const uint16_t *px, unsigned w, unsigned h, float distance, bool lossless,
                                       int effort);
bool jxl_decode_gray16(const uint8_t *data, size_t len, unsigned w, unsigned h, std::vector<uint16_t> &px);

// Lossless 8-bit single-channel image.
std::vector<uint8_t> jxl_encode_gray8_lossless(const uint8_t *px, unsigned w, unsigned h, int effort);
bool jxl_decode_gray8(const uint8_t *data, size_t len, unsigned w, unsigned h, std::vector<uint8_t> &px);

// Lossless JPEG recompression; returns empty on failure (unsupported JPEG).
std::vector<uint8_t> jxl_from_jpeg(const uint8_t *jpeg, size_t len, int effort);
bool jxl_to_jpeg(const uint8_t *data, size_t len, std::vector<uint8_t> &jpeg);

std::vector<uint8_t> brotli_compress(const uint8_t *data, size_t len);
bool brotli_decompress(const uint8_t *data, size_t len, size_t out_len, std::vector<uint8_t> &out);

} // namespace nefcomp
