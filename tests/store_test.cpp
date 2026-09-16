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
