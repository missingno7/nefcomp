// SPDX-License-Identifier: GPL-2.0-or-later
// The "NEF skeleton" stored inside a DNG: everything of the original NEF
// except the raw image, in a compact reversible form.
#pragma once
#include "preview.h"

#include <cstdint>
#include <string>
#include <vector>

namespace nefcomp {

struct EmbedStats {
    size_t skeleton = 0;          // bytes of the NEF before the raw strip
    size_t blob = 0;              // bytes stored in the DNG
    size_t previews_dropped = 0;  // original bytes of previews that will be regenerated
    size_t previews_kept = 0;     // original bytes of previews stored (lossless JPEG recompression)
    size_t helpers = 0, helpers_stored = 0;  // other embedded images (bytes before/after)
    bool makernote_shared = false;           // makernote taken from the DNG's own copy
};

// nef: complete original file; px: its decoded raw samples; dng_makernote:
// the makernote copy inside the DNG (DNGPrivateData "MakN"), may be empty.
std::vector<uint8_t> embed_build(const std::vector<uint8_t> &nef, const std::vector<uint16_t> &px,
                                 const std::vector<uint8_t> &dng_makernote, bool keep_previews, EmbedStats *st);

struct EmbedRestore {
    bool pixels_exact = false, file_exact = false;
    unsigned previews_regenerated = 0;
    uint64_t original_size = 0;
};

// Rebuild the NEF from the blob, the (decoded) raw samples and the DNG's makernote.
std::vector<uint8_t> embed_restore(const std::vector<uint8_t> &blob, const std::vector<uint16_t> &px,
                                   const std::vector<uint8_t> &dng_makernote, const RenderParams &rp,
                                   EmbedRestore *info);

// Extract the makernote copy from DNGPrivateData ("Adobe\0MakN..."), or empty.
std::vector<uint8_t> dng_private_makernote(const std::vector<uint8_t> &private_data);

} // namespace nefcomp
