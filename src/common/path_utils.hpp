#pragma once

#include <filesystem>
#include <fstream>
#include <cctype>
#include <optional>
#include <string>
#include <string_view>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

namespace mcdk::path {

inline std::filesystem::path from_utf8(std::string_view text);

// Resolve an existing user supplied path without allowing it to escape root.
// `std::filesystem::path::is_absolute()` is platform dependent, therefore the
// textual Windows forms are rejected explicitly on every platform as well.
inline std::optional<std::filesystem::path> resolve_existing_within_root(
    const std::filesystem::path& root, std::string_view user_path, std::string* error = nullptr) {
    auto fail = [&](const char* message) -> std::optional<std::filesystem::path> {
        if (error) *error = message;
        return std::nullopt;
    };
    if (root.empty()) return fail("knowledge root is unavailable");
    if (user_path.empty()) return fail("path is required");

    std::string normalized(user_path);
    for (char& c : normalized) if (c == '\\') c = '/';
    if (normalized.rfind("//", 0) == 0 ||
        (normalized.size() >= 2 && std::isalpha(static_cast<unsigned char>(normalized[0])) && normalized[1] == ':'))
        return fail("absolute and drive paths are not allowed");

    const auto relative = from_utf8(normalized);
    if (relative.is_absolute()) return fail("absolute paths are not allowed");

    std::error_code ec;
    const auto canonical_root = std::filesystem::weakly_canonical(root, ec);
    if (ec || !std::filesystem::is_directory(canonical_root, ec)) return fail("knowledge root is unavailable");
    const auto candidate = std::filesystem::weakly_canonical(canonical_root / relative, ec);
    if (ec || !std::filesystem::exists(candidate, ec)) return fail("path does not exist");

    auto root_it = canonical_root.begin();
    auto candidate_it = candidate.begin();
    for (; root_it != canonical_root.end(); ++root_it, ++candidate_it) {
        if (candidate_it == candidate.end() || *root_it != *candidate_it)
            return fail("path escapes knowledge root");
    }
    return candidate;
}

// 约定：模块内部尽量传 path，只有日志/协议边界再转 UTF-8 string。
inline std::filesystem::path from_utf8(std::string_view text) {
#if defined(__cpp_lib_char8_t)
    return std::filesystem::u8path(text);
#else
    return std::filesystem::u8path(text.begin(), text.end());
#endif
}

inline std::string to_utf8(const std::filesystem::path& value) {
    auto u8 = value.generic_u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

inline std::string filename_to_utf8(const std::filesystem::path& value) {
    return to_utf8(value.filename());
}

inline std::filesystem::path executable_path() {
#ifdef _WIN32
    // 用宽字符 API，避免可执行文件路径里有中文时被本地代码页截断。
    std::wstring buffer(MAX_PATH, L'\0');
    while (true) {
        DWORD size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (size == 0) return {};
        if (size < buffer.size() - 1) {
            buffer.resize(size);
            return std::filesystem::path(buffer);
        }
        buffer.resize(buffer.size() * 2);
    }
#elif defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    if (size == 0) return {};

    std::string buffer(size, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) return {};
    buffer.resize(std::char_traits<char>::length(buffer.c_str()));
    return std::filesystem::weakly_canonical(std::filesystem::path(buffer));
#else
    return std::filesystem::canonical("/proc/self/exe");
#endif
}

inline std::filesystem::path executable_dir() {
    return executable_path().parent_path();
}

} // namespace mcdk::path
