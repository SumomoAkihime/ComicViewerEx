#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace cv {
namespace fs = std::filesystem;
std::string utf8(const std::wstring& value);
std::wstring wide(const std::string& value);
fs::path executableDir();
std::wstring lower(std::wstring value);
bool isImage(const fs::path& path);
bool isArchive(const fs::path& path);
bool naturalLess(const std::wstring& a, const std::wstring& b);
std::string fileIdentity(const fs::path& path);
struct Cancelled : std::exception {};
using Cancel = std::function<bool()>;
using Progress = std::function<void(uint64_t, uint64_t)>;
inline void checkCancel(const Cancel& cancel) { if (cancel && cancel()) throw Cancelled{}; }
struct PageEntry { std::wstring name; uint32_t index{}; uint64_t size{}; };
struct Pixels {
    uint32_t width{}, height{};
    uint32_t originalWidth{}, originalHeight{};
    std::vector<uint8_t> bgra;
    size_t bytes() const { return bgra.size(); }
};
struct ReadingState {
    std::wstring entry;
    int page{}, half{}, mode{}, fit{};
    bool rtl{};
    double zoom{1.0};
};
struct ReadingPreferences {
    int mode{}, fit{};
    bool rtl{};

    void apply(ReadingState& state) const
    {
        state.mode = mode;
        state.fit = fit;
        state.rtl = rtl;
        if (mode != 2) state.half = 0;
    }
};
struct BookRow { std::string id; fs::path path; uint64_t size{}; std::wstring tags; };
struct Tag { int64_t id{}; std::wstring name; };
struct Password { int64_t id{}; std::wstring value; };
}
