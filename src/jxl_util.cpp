// SPDX-License-Identifier: GPL-2.0-or-later
#include "jxl_util.h"

#include <brotli/decode.h>
#include <brotli/encode.h>
#include <jxl/decode.h>
#include <jxl/encode.h>

#include <stdexcept>

namespace nefcomp {

namespace {
struct EncGuard { JxlEncoder *e; ~EncGuard() { JxlEncoderDestroy(e); } };
struct DecGuard { JxlDecoder *d; ~DecGuard() { JxlDecoderDestroy(d); } };

std::vector<uint8_t> drain(JxlEncoder *enc) {
    std::vector<uint8_t> out(1 << 16);
    uint8_t *next = out.data();
    size_t avail = out.size();
    for (;;) {
        JxlEncoderStatus st = JxlEncoderProcessOutput(enc, &next, &avail);
        if (st == JXL_ENC_SUCCESS) break;
        if (st != JXL_ENC_NEED_MORE_OUTPUT) throw std::runtime_error("JPEG XL encode failed");
        size_t used = next - out.data();
        out.resize(out.size() * 2);
        next = out.data() + used;
        avail = out.size() - used;
    }
    out.resize(next - out.data());
    return out;
}

std::vector<uint8_t> encode_gray(const void *px, unsigned w, unsigned h, JxlDataType type, unsigned bits,
                                 float distance, bool lossless, int effort) {
    JxlEncoder *enc = JxlEncoderCreate(nullptr);
    if (!enc) throw std::runtime_error("JxlEncoderCreate failed");
    EncGuard g{enc};
    JxlBasicInfo info;
    JxlEncoderInitBasicInfo(&info);
    info.xsize = w; info.ysize = h;
    info.bits_per_sample = bits; info.exponent_bits_per_sample = 0;
    info.num_color_channels = 1;
    info.uses_original_profile = lossless ? JXL_TRUE : JXL_FALSE;
    if (JxlEncoderSetBasicInfo(enc, &info) != JXL_ENC_SUCCESS) throw std::runtime_error("JPEG XL basic info");
    JxlColorEncoding ce;
    JxlColorEncodingSetToSRGB(&ce, JXL_TRUE);
    if (JxlEncoderSetColorEncoding(enc, &ce) != JXL_ENC_SUCCESS) throw std::runtime_error("JPEG XL colour encoding");
    JxlEncoderFrameSettings *fs = JxlEncoderFrameSettingsCreate(enc, nullptr);
    JxlEncoderFrameSettingsSetOption(fs, JXL_ENC_FRAME_SETTING_EFFORT, effort);
    if (lossless) JxlEncoderSetFrameLossless(fs, JXL_TRUE);
    else if (JxlEncoderSetFrameDistance(fs, distance) != JXL_ENC_SUCCESS) throw std::runtime_error("JPEG XL distance");
    JxlPixelFormat pf{1, type, JXL_NATIVE_ENDIAN, 0};
    size_t bytes = size_t(w) * h * (type == JXL_TYPE_UINT16 ? 2 : 1);
    if (JxlEncoderAddImageFrame(fs, &pf, px, bytes) != JXL_ENC_SUCCESS) throw std::runtime_error("JPEG XL add frame");
    JxlEncoderCloseInput(enc);
    return drain(enc);
}

bool decode_gray(const uint8_t *data, size_t len, unsigned w, unsigned h, JxlDataType type, void *out, size_t out_bytes) {
    JxlDecoder *dec = JxlDecoderCreate(nullptr);
    if (!dec) return false;
    DecGuard g{dec};
    if (JxlDecoderSubscribeEvents(dec, JXL_DEC_BASIC_INFO | JXL_DEC_FULL_IMAGE) != JXL_DEC_SUCCESS) return false;
    JxlDecoderSetInput(dec, data, len);
    JxlDecoderCloseInput(dec);
    JxlPixelFormat pf{1, type, JXL_NATIVE_ENDIAN, 0};
    for (;;) {
        JxlDecoderStatus st = JxlDecoderProcessInput(dec);
        if (st == JXL_DEC_BASIC_INFO) {
            JxlBasicInfo info;
            if (JxlDecoderGetBasicInfo(dec, &info) != JXL_DEC_SUCCESS || info.xsize != w || info.ysize != h) return false;
        } else if (st == JXL_DEC_NEED_IMAGE_OUT_BUFFER) {
            if (JxlDecoderSetImageOutBuffer(dec, &pf, out, out_bytes) != JXL_DEC_SUCCESS) return false;
        } else if (st == JXL_DEC_FULL_IMAGE) {
            continue;
        } else {
            return st == JXL_DEC_SUCCESS;
        }
    }
}
} // namespace

std::vector<uint8_t> jxl_encode_gray16(const uint16_t *px, unsigned w, unsigned h, float distance, bool lossless, int effort) {
    return encode_gray(px, w, h, JXL_TYPE_UINT16, 16, distance, lossless, effort);
}
bool jxl_decode_gray16(const uint8_t *data, size_t len, unsigned w, unsigned h, std::vector<uint16_t> &px) {
    px.assign(size_t(w) * h, 0);
    return decode_gray(data, len, w, h, JXL_TYPE_UINT16, px.data(), px.size() * 2);
}
std::vector<uint8_t> jxl_encode_gray8_lossless(const uint8_t *px, unsigned w, unsigned h, int effort) {
    return encode_gray(px, w, h, JXL_TYPE_UINT8, 8, 0, true, effort);
}
bool jxl_decode_gray8(const uint8_t *data, size_t len, unsigned w, unsigned h, std::vector<uint8_t> &px) {
    px.assign(size_t(w) * h, 0);
    return decode_gray(data, len, w, h, JXL_TYPE_UINT8, px.data(), px.size());
}

std::vector<uint8_t> jxl_from_jpeg(const uint8_t *jpeg, size_t len, int effort) {
    try {
        JxlEncoder *enc = JxlEncoderCreate(nullptr);
        if (!enc) return {};
        EncGuard g{enc};
        if (JxlEncoderStoreJPEGMetadata(enc, JXL_TRUE) != JXL_ENC_SUCCESS) return {};
        JxlEncoderFrameSettings *fs = JxlEncoderFrameSettingsCreate(enc, nullptr);
        JxlEncoderFrameSettingsSetOption(fs, JXL_ENC_FRAME_SETTING_EFFORT, effort);
        if (JxlEncoderAddJPEGFrame(fs, jpeg, len) != JXL_ENC_SUCCESS) return {};
        JxlEncoderCloseInput(enc);
        return drain(enc);
    } catch (...) {
        return {};
    }
}

bool jxl_to_jpeg(const uint8_t *data, size_t len, std::vector<uint8_t> &jpeg) {
    JxlDecoder *dec = JxlDecoderCreate(nullptr);
    if (!dec) return false;
    DecGuard g{dec};
    if (JxlDecoderSubscribeEvents(dec, JXL_DEC_JPEG_RECONSTRUCTION | JXL_DEC_FULL_IMAGE) != JXL_DEC_SUCCESS) return false;
    JxlDecoderSetInput(dec, data, len);
    JxlDecoderCloseInput(dec);
    jpeg.assign(len * 2 + 65536, 0);
    size_t used = 0;
    bool have = false;
    for (;;) {
        JxlDecoderStatus st = JxlDecoderProcessInput(dec);
        if (st == JXL_DEC_JPEG_RECONSTRUCTION) {
            if (JxlDecoderSetJPEGBuffer(dec, jpeg.data(), jpeg.size()) != JXL_DEC_SUCCESS) return false;
            have = true;
        } else if (st == JXL_DEC_JPEG_NEED_MORE_OUTPUT) {
            size_t remaining = JxlDecoderReleaseJPEGBuffer(dec);
            used = jpeg.size() - remaining;
            jpeg.resize(jpeg.size() * 2);
            if (JxlDecoderSetJPEGBuffer(dec, jpeg.data() + used, jpeg.size() - used) != JXL_DEC_SUCCESS) return false;
        } else if (st == JXL_DEC_NEED_IMAGE_OUT_BUFFER) {
            return false;  // not a recompressed JPEG
        } else if (st == JXL_DEC_FULL_IMAGE || st == JXL_DEC_SUCCESS) {
            if (!have) return false;
            size_t remaining = JxlDecoderReleaseJPEGBuffer(dec);
            jpeg.resize(jpeg.size() - remaining);
            return true;
        } else {
            return false;
        }
    }
}

std::vector<uint8_t> brotli_compress(const uint8_t *data, size_t len) {
    size_t n = BrotliEncoderMaxCompressedSize(len);
    std::vector<uint8_t> out(n ? n : len + 1024);
    size_t outn = out.size();
    if (!BrotliEncoderCompress(11, 24, BROTLI_MODE_GENERIC, len, data, &outn, out.data()))
        throw std::runtime_error("brotli compress failed");
    out.resize(outn);
    return out;
}

bool brotli_decompress(const uint8_t *data, size_t len, size_t out_len, std::vector<uint8_t> &out) {
    out.assign(out_len, 0);
    size_t n = out_len;
    return BrotliDecoderDecompress(len, data, &n, out.data()) == BROTLI_DECODER_RESULT_SUCCESS && n == out_len;
}

} // namespace nefcomp
