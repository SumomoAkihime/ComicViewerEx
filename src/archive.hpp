#pragma once
#include "common.hpp"
#include <span>
namespace cv {
struct PasswordRequired : std::runtime_error { PasswordRequired() : std::runtime_error("需要密码或密码错误") {} };
class BookSource {
public:
    virtual ~BookSource() = default;
    virtual const std::vector<PageEntry>& entries() const = 0;
    virtual std::vector<uint8_t> read(size_t page, const Cancel& cancel, const Progress& progress) = 0;
    virtual bool solid() const = 0;
    virtual bool encrypted() const { return false; }
};
// One source belongs to one worker thread. cacheRoot is an app-owned session folder.
std::unique_ptr<BookSource> openBook(const fs::path& path, const std::wstring& password,
    const fs::path& cacheRoot, const Cancel& cancel, const Progress& progress);
}
