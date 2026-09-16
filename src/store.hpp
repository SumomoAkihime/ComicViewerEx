#pragma once
#include "common.hpp"
#include <mutex>
struct sqlite3;
namespace cv {
class Store {
public:
    explicit Store(const fs::path& file);
    ~Store();
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;
    std::string ensureBook(const fs::path& path);
    ReadingState reading(const std::string& book);
    void saveReading(const std::string& book, const ReadingState& state);
    std::wstring setting(const std::string& key, const std::wstring& fallback = L"");
    void setSetting(const std::string& key, const std::wstring& value);
    std::vector<Tag> tags();
    void createTag(const std::wstring& name);
    void renameTag(int64_t tag, const std::wstring& name);
    void deleteTag(int64_t tag);
    void assignTags(const std::vector<std::string>& books, const std::vector<int64_t>& tags, bool add);
    std::wstring bookTags(const std::string& book);
    bool matches(const std::string& book, const std::vector<int64_t>& tags, bool all);
    std::vector<Password> passwords();
    void addPassword(const std::wstring& password);
    void updatePassword(int64_t id, const std::wstring& password);
    void deletePassword(int64_t id);
    std::wstring bookPassword(const std::string& book);
    void rememberPassword(const std::string& book, const std::wstring& password);
private:
    sqlite3* db_{};
    std::recursive_mutex mutex_;
};
}
