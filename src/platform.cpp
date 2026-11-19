// SPDX-License-Identifier: GPL-2.0-or-later
#include "platform.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#else
#include <fcntl.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif

namespace nefcomp::platform {

namespace fs = std::filesystem;

#ifdef _WIN32

std::vector<std::string> args_utf8(int, char **) {
    // Windows hands narrow argv over in the ANSI code page, which loses names
    // like "Fotky čeština": take the UTF-16 command line and keep UTF-8.
    std::vector<std::string> args;
    int argc = 0;
    wchar_t **wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; wargv && i < argc; i++) args.push_back(fs::path(wargv[i]).u8string());
    if (wargv) LocalFree(wargv);
    return args;
}

void console_utf8() { SetConsoleOutputCP(CP_UTF8); }

FILE *open_file(const fs::path &path, const char *mode) {
    std::wstring m(mode, mode + std::char_traits<char>::length(mode));
    return _wfopen(path.wstring().c_str(), m.c_str());
}

int seek64(FILE *f, int64_t offset, int whence) { return _fseeki64(f, offset, whence); }
int64_t tell64(FILE *f) { return _ftelli64(f); }

static std::wstring quote(const std::wstring &a) {
    // CommandLineToArgvW rules: backslashes are literal unless they precede a quote
    std::wstring r = L"\"";
    size_t bs = 0;
    for (wchar_t c : a) {
        if (c == L'\\') { bs++; continue; }
        if (c == L'"') r.append(bs * 2 + 1, L'\\');
        else r.append(bs, L'\\');
        bs = 0;
        r += c;
    }
    r.append(bs * 2, L'\\');
    return r + L"\"";
}

int run_quiet(const std::vector<std::string> &argv) {
    std::wstring cmd;
    for (const auto &a : argv) cmd += (cmd.empty() ? L"" : L" ") + quote(fs::u8path(a).wstring());
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE nul = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    si.hStdOutput = si.hStdError = nul;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    BOOL ok = CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    if (!ok) { CloseHandle(nul); return -1; }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(nul);
    return int(code);
}

std::string default_dng_converter() {
    return "C:\\Program Files\\Adobe\\Adobe DNG Converter\\Adobe DNG Converter.exe";
}

unsigned process_id() { return unsigned(GetCurrentProcessId()); }

#else  // POSIX

std::vector<std::string> args_utf8(int argc, char **argv) { return std::vector<std::string>(argv + 1, argv + argc); }

void console_utf8() {}

FILE *open_file(const fs::path &path, const char *mode) { return std::fopen(path.c_str(), mode); }

int seek64(FILE *f, int64_t offset, int whence) { return fseeko(f, off_t(offset), whence); }
int64_t tell64(FILE *f) { return int64_t(ftello(f)); }

int run_quiet(const std::vector<std::string> &argv) {
    std::vector<char *> av;
    for (const auto &a : argv) av.push_back(const_cast<char *>(a.c_str()));
    av.push_back(nullptr);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    pid_t pid = 0;
    int rc = posix_spawn(&pid, av[0], &fa, nullptr, av.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    if (rc != 0) return -1;
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {}
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

std::string default_dng_converter() {
#ifdef __APPLE__
    return "/Applications/Adobe DNG Converter.app/Contents/MacOS/Adobe DNG Converter";
#else
    return "";  // Adobe DNG Converter has no Linux version
#endif
}

unsigned process_id() { return unsigned(getpid()); }

#endif

FILE *open_file(const std::string &utf8_path, const char *mode) { return open_file(fs::u8path(utf8_path), mode); }

fs::path temp_dir() { return fs::temp_directory_path(); }

} // namespace nefcomp::platform
