// SPDX-License-Identifier: GPL-2.0-or-later
// The few operating-system services nefcomp needs, for Windows and POSIX
// (Linux, macOS). All strings are UTF-8; paths are std::filesystem::path.
#pragma once
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace nefcomp::platform {

// Command-line arguments (without the program name) as UTF-8.
std::vector<std::string> args_utf8(int argc, char **argv);

// Make the console print UTF-8 (Windows); no-op elsewhere.
void console_utf8();

// fopen for a UTF-8 path / a filesystem path.
FILE *open_file(const std::string &utf8_path, const char *mode);
FILE *open_file(const std::filesystem::path &path, const char *mode);

// 64-bit seek/tell.
int seek64(FILE *f, int64_t offset, int whence);
int64_t tell64(FILE *f);

// Run a program with arguments, output discarded, no console window.
// Returns its exit code, or -1 if it could not be started.
int run_quiet(const std::vector<std::string> &argv);

// Where Adobe DNG Converter is installed by default ("" where it does not exist, e.g. Linux).
std::string default_dng_converter();

// Directory for temporary files, and an id unique to this process.
std::filesystem::path temp_dir();
unsigned process_id();

} // namespace nefcomp::platform
