#include "common.hpp"
#include <shlwapi.h>
#include <cwctype>
#include <iomanip>
#include <sstream>

namespace cv {
std::string utf8(const std::wstring& value) {
    if (value.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (!n) throw std::runtime_error("文本编码转换失败");
    std::string result(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), n, nullptr, nullptr);
    return result;
}
std::wstring wide(const std::string& value) {
    if (value.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (!n) return L"无法读取错误说明";
    std::wstring result(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), n);
    return result;
}
fs::path executableDir() {
    std::wstring result(32768, L'\0');
    auto n = GetModuleFileNameW(nullptr, result.data(), static_cast<DWORD>(result.size()));
    result.resize(n);
    return fs::path(result).parent_path();
}
std::wstring lower(std::wstring value) { std::transform(value.begin(), value.end(), value.begin(), towlower); return value; }
bool isImage(const fs::path& path) {
    const auto ext = lower(path.extension().wstring());
    return ext == L".jpg" || ext == L".jpeg" || ext == L".png" || ext == L".webp" || ext == L".bmp" || ext == L".gif" || ext == L".tif" || ext == L".tiff";
}
bool isArchive(const fs::path& path) {
    const auto ext = lower(path.extension().wstring());
    return ext == L".zip" || ext == L".cbz" || ext == L".rar" || ext == L".cbr" || ext == L".7z";
}
bool naturalLess(const std::wstring& a, const std::wstring& b) {
    const int cmp = StrCmpLogicalW(a.c_str(), b.c_str());
    return cmp == 0 ? a < b : cmp < 0;
}
std::string fileIdentity(const fs::path& path) {
    HANDLE handle = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle != INVALID_HANDLE_VALUE) {
        FILE_ID_INFO info{};
        FILE_BASIC_INFO basic{};
        const bool ok = GetFileInformationByHandleEx(handle, FileIdInfo, &info, sizeof(info)) != FALSE;
        GetFileInformationByHandleEx(handle, FileBasicInfo, &basic, sizeof(basic));
        CloseHandle(handle);
        if (ok) {
            std::ostringstream result;
            result << "fid:" << std::hex << info.VolumeSerialNumber << ':';
            for (auto b : info.FileId.Identifier) result << std::setw(2) << std::setfill('0') << static_cast<unsigned>(b);
            // Creation time prevents association after the file system reuses a deleted file ID.
            result << ':' << basic.CreationTime.QuadPart;
            return result.str();
        }
    }
    return "path:" + utf8(lower(fs::absolute(path).lexically_normal().wstring()));
}
}
