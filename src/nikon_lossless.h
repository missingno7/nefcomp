// Nikon lossless-compressed NEF decoder (14/12-bit, "F0"/"D" variants).
// Written from the publicly documented format (dcraw / LibRaw nikon_load_raw).
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>
#include <string>

namespace nefcomp {

// meta: contents of makernote tag 0x0096 (NEFLinearizationTable).
// meta_le: true if the makernote is little-endian ("II").
// Returns false (and sets err) on malformed input.
bool decode_nikon_lossless(const uint8_t *strip, size_t strip_len,
                           const uint8_t *meta, size_t meta_len, bool meta_le,
                           unsigned width, unsigned height, unsigned bps,
                           std::vector<uint16_t> &out, std::string &err);

// Lossy NEFs (makernote 0x96 version 0x44): decode the raw codes (before the
// curve) plus the curve that maps code -> 14-bit value; encode codes back.
// Rows from the "split" row on use coarser Huffman symbols: a value that
// overshoots below zero is stored as zero (as Adobe decodes it), and a code
// may exceed the curve (value = curve[min(code, curve.size() - 1)]).
// encode_nikon_lossy: codes the stream cannot represent exactly (after the
// split row) are replaced by the nearest representable ones, in place.
bool decode_nikon_codes(const uint8_t *strip, size_t strip_len, const uint8_t *meta, size_t meta_len, bool meta_le,
                        unsigned width, unsigned height, unsigned bps, std::vector<uint16_t> &codes,
                        std::vector<uint16_t> &curve, std::string &err);
bool nikon_lossy_curve(const uint8_t *meta, size_t meta_len, bool meta_le, unsigned bps, std::vector<uint16_t> &curve,
                       std::string &err);
bool encode_nikon_lossy(std::vector<uint16_t> &codes, unsigned width, unsigned height, unsigned bps, const uint8_t *meta,
                        size_t meta_len, bool meta_le, std::vector<uint8_t> &out, std::string &err);

// Encode samples as a Nikon lossless (F0) strip using the predictor seeds in
// `meta` (makernote 0x0096). Inverse of decode_nikon_lossless.
bool encode_nikon_lossless(const uint16_t *px, unsigned width, unsigned height, unsigned bps,
                           const uint8_t *meta, size_t meta_len, bool meta_le,
                           std::vector<uint8_t> &out, std::string &err);

// Nikon uncompressed 14-bit: 4 samples in 7 bytes (LSB first), rows padded to 16 bytes.
size_t nikon_packed14_stride(unsigned width);
void decode_nikon_packed14(const uint8_t *src, unsigned width, unsigned height, std::vector<uint16_t> &out);
std::vector<uint8_t> encode_nikon_packed14(const uint16_t *px, unsigned width, unsigned height);

} // namespace nefcomp
