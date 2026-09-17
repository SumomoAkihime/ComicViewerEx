#include "../src/store.hpp"

#include <cassert>
#include <chrono>
#include <fstream>
#include <iostream>

namespace {

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

struct TempDirectory {
    cv::fs::path path;

    TempDirectory()
    {
        std::error_code error;
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = cv::fs::temp_directory_path(error) / (L"ComicViewerEx-store-" + std::to_wstring(stamp));
        require(!error, "无法取得临时目录");
        cv::fs::create_directories(path, error);
        require(!error, "无法创建临时目录");
    }

    ~TempDirectory()
    {
        std::error_code error;
        cv::fs::remove_all(path, error);
    }
};

void writeFile(const cv::fs::path& path, const char* text)
{
    std::ofstream output(path, std::ios::binary);
    require(output.good(), "无法创建测试文件");
    output << text;
    require(output.good(), "无法写入测试文件");
}

} // namespace

int main()
{
    try {
        TempDirectory temporary;
        const cv::fs::path database = temporary.path / L"state" / L"store.db";
        const cv::fs::path bookPath = temporary.path / L"book.cbz";
        const cv::fs::path directoryPath = temporary.path / L"book-directory";
        writeFile(bookPath, "comic");
        {
            std::error_code error;
            cv::fs::create_directories(directoryPath, error);
            require(!error, "无法创建目录测试书籍");
        }

        std::string book;
        std::string directoryBook;
        int64_t action = 0;
        int64_t comedy = 0;
        {
            cv::Store store(database);
            book = store.ensureBook(bookPath);
            directoryBook = store.ensureBook(directoryPath);
            cv::ReadingState directoryState;
            directoryState.entry = L"001.jpg";
            directoryState.page = 2;
            directoryState.zoom = 0.9;
            store.saveReading(directoryBook, directoryState);
            store.createTag(L"动作");
            store.createTag(L"喜剧");
            const auto allTags = store.tags();
            require(allTags.size() == 2, "标签数量错误");
            for (const auto& tag : allTags) {
                if (tag.name == L"动作") action = tag.id;
                if (tag.name == L"喜剧") comedy = tag.id;
            }
            require(action != 0 && comedy != 0, "标签读取错误");
            store.assignTags({book}, {action, comedy}, true);
            const auto assigned = store.bookTagItems(book);
            require(assigned.size() == 2 && assigned[0].id != assigned[1].id, "同一文件应返回多个独立标签");
            store.assignTags({book}, {action}, true);
            require(store.bookTagItems(book).size() == 2, "重复打标不应重复或覆盖其他标签");
            store.createTag(L"作者, 合集");
            const auto withComma = store.tags();
            const auto commaTag = std::find_if(withComma.begin(), withComma.end(), [](const auto& tag) { return tag.name == L"作者, 合集"; })->id;
            store.assignTags({book, directoryBook}, {commaTag}, true);
            const auto threeTags = store.bookTagItems(book);
            require(threeTags.size() == 3 && std::any_of(threeTags.begin(), threeTags.end(), [&](const auto& tag) { return tag.id == commaTag && tag.name == L"作者, 合集"; }), "标签名称中的逗号不能拆分为多个标签");
            store.assignTags({book}, {commaTag}, false);
            require(store.bookTagItems(book).size() == 2 && store.bookTagItems(directoryBook).size() == 1, "移除指定标签不得覆盖其他文件或标签");
            store.deleteTag(commaTag);
            require(store.matches(book, {action, comedy}, true), "标签 AND 匹配错误");
            require(store.matches(book, {action, 999999}, false), "标签 OR 匹配错误");
            require(!store.matches(book, {action, 999999}, true), "未知标签不应通过 AND");
            require(store.matches(book, {}, true), "无筛选应匹配全部");

            cv::ReadingState state;
            state.entry = L"目录/001.jpg";
            state.page = 3;
            state.half = 1;
            state.mode = 2;
            state.fit = 1;
            state.rtl = true;
            state.zoom = 1.25;
            store.saveReading(book, state);
            store.setSetting("language", L"中文");
            store.addPassword(L"旧密码");
            store.rememberPassword(book, L"成功密码");
            require(store.bookPassword(book) == L"成功密码", "书籍密码保存错误");
            require(store.setting("language") == L"中文", "设置保存错误");
        }

        {
            cv::ReadingState state;
            state.entry = L"005.jpg";
            state.page = 7;
            state.half = 1;
            state.mode = 2;
            state.fit = 3;
            state.rtl = true;
            state.zoom = 1.75;
            const auto entry = state.entry;
            const auto page = state.page;
            const auto zoom = state.zoom;
            cv::ReadingPreferences preferences;
            preferences.mode = 1;
            preferences.fit = 0;
            preferences.rtl = false;
            preferences.apply(state);
            require(state.mode == 1 && state.fit == 0 && !state.rtl && state.half == 0,
                    "阅读偏好应用错误");
            require(state.entry == entry && state.page == page && state.zoom == zoom,
                    "应用阅读偏好不应修改进度");
        }

        const cv::fs::path renamedPath = temporary.path / L"renamed.cbz";
        cv::fs::rename(bookPath, renamedPath);
        {
            cv::Store store(database);
            require(store.ensureBook(renamedPath) == book, "同文件改名后身份不一致");
            require(store.ensureBook(directoryPath) == directoryBook, "目录书籍身份不一致");
            const auto directoryState = store.reading(directoryBook);
            require(directoryState.entry == L"001.jpg" && directoryState.page == 2 && directoryState.zoom == 0.9,
                    "目录阅读状态持久化错误");
            const auto state = store.reading(book);
            require(state.entry == L"目录/001.jpg" && state.page == 3 && state.rtl && state.zoom == 1.25,
                    "阅读状态持久化错误");
            require(store.bookTags(book).find(L"动作") != std::wstring::npos, "标签持久化错误");
            require(store.bookPassword(book) == L"成功密码", "书籍密码持久化错误");

            const auto passwords = store.passwords();
            require(passwords.size() == 2, "密码数量错误");
            for (const auto& password : passwords) {
                if (password.value == L"旧密码") {
                    store.updatePassword(password.id, L"更新密码");
                    store.deletePassword(password.id);
                }
            }
            require(store.passwords().size() == 1, "密码删除错误");
            store.assignTags({book}, {comedy}, false);
            require(!store.matches(book, {comedy}, true), "标签移除错误");
            store.renameTag(action, L"冒险");
            require(store.bookTags(book).find(L"冒险") != std::wstring::npos, "标签重命名错误");
        }

        const cv::fs::path preferencesDatabase = temporary.path / L"preferences.db";
        const cv::fs::path legacyBookPath = temporary.path / L"legacy.cbz";
        writeFile(legacyBookPath, "legacy");
        {
            cv::Store store(preferencesDatabase);
            const std::string legacyBook = store.ensureBook(legacyBookPath);
            cv::ReadingState legacyState;
            legacyState.mode = 2;
            legacyState.fit = 3;
            legacyState.rtl = true;
            store.saveReading(legacyBook, legacyState);
            store.setSetting("lastBook", legacyBookPath.wstring());
        }
        std::error_code removeError;
        cv::fs::remove(legacyBookPath, removeError);
        require(!removeError, "无法删除迁移测试书籍");
        {
            cv::Store store(preferencesDatabase);
            const auto preferences = store.readingPreferences();
            require(preferences.mode == 2 && preferences.fit == 3 && preferences.rtl,
                    "旧书阅读偏好迁移错误");
            cv::ReadingPreferences updated;
            updated.mode = 1;
            updated.fit = 2;
            updated.rtl = false;
            store.saveReadingPreferences(updated);
        }
        {
            cv::Store store(preferencesDatabase);
            const auto preferences = store.readingPreferences();
            require(preferences.mode == 1 && preferences.fit == 2 && !preferences.rtl,
                    "全局阅读偏好持久化错误");
            store.setSetting("lastBook", L"其他漫画.cbz");
            const auto unchanged = store.readingPreferences();
            require(unchanged.mode == 1 && unchanged.fit == 2 && !unchanged.rtl,
                    "全局阅读偏好不应随上次书籍改变");
        }

        {
            const cv::fs::path defaultDatabase = temporary.path / L"default-preferences.db";
            cv::Store store(defaultDatabase);
            store.setSetting("lastBook", (temporary.path / L"not-found.cbz").wstring());
            const auto preferences = store.readingPreferences();
            require(preferences.mode == 0 && preferences.fit == 0 && !preferences.rtl,
                    "无旧书时默认阅读偏好错误");
        }

        {
            cv::Store store(preferencesDatabase);
            store.setSetting("reading.mode", L"999");
            store.setSetting("reading.fit", L"invalid");
            store.setSetting("reading.rtl", L"-1");
            const auto preferences = store.readingPreferences();
            require(preferences.mode == 0 && preferences.fit == 0 && !preferences.rtl,
                    "异常全局阅读偏好未回退");
        }

        bool invalidFailed = false;
        try {
            cv::Store invalid(temporary.path / L"missing" / L"x.db");
            invalid.ensureBook(temporary.path / L"missing.cbz");
        } catch (const std::exception&) {
            invalidFailed = true;
        }
        require(invalidFailed, "无效书籍路径未报错");
        require(SetFileAttributesW(database.c_str(), FILE_ATTRIBUTE_READONLY) != FALSE, "无法设置只读测试数据");
        bool readonlyFailed = false;
        try { cv::Store readonly(database); readonly.setSetting("test", L"不能保存"); }
        catch (const std::exception&) { readonlyFailed = true; }
        SetFileAttributesW(database.c_str(), FILE_ATTRIBUTE_NORMAL);
        require(readonlyFailed, "只读数据库未明确报错");
        std::cout << "store_test passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "store_test failed: " << exception.what() << '\n';
        return 1;
    }
}
