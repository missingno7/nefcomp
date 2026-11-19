// SPDX-License-Identifier: GPL-2.0-or-later
// Small MD5 / SHA-256 implementations (RFC 1321 / FIPS 180-4).
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace nefcomp {

std::array<uint8_t, 16> md5(const uint8_t *data, size_t len);
std::array<uint8_t, 32> sha256(const uint8_t *data, size_t len);

} // namespace nefcomp
