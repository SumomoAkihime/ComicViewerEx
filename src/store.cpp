#include "store.hpp"

#if __has_include("../third_party/sqlite/sqlite3.h")
#include "../third_party/sqlite/sqlite3.h"
#else
#include <sqlite3.h>
#endif

#include <charconv>
#include <cmath>
#include <limits>
#include <system_error>

namespace cv {
namespace {

[[noreturn]] void fail(const char* action, sqlite3* db = nullptr)
{
    std::string message = action;
    if (db != nullptr) {
        message += ": ";
        message += sqlite3_errmsg(db);
    }
    throw std::runtime_error(message);
}

void checkSqlite(int result, const char* action, sqlite3* db)
{
    if (result != SQLITE_OK) fail(action, db);
}

class Statement {
public:
    Statement(sqlite3* db, const char* sql, const char* action)
        : db_(db), action_(action)
    {
        checkSqlite(sqlite3_prepare_v2(db_, sql, -1, &stmt_, nullptr), action_, db_);
    }

    ~Statement()
    {
        if (stmt_ != nullptr) sqlite3_finalize(stmt_);
    }

    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    sqlite3_stmt* get() const { return stmt_; }

    void bindText(int index, const std::string& value)
    {
        checkSqlite(sqlite3_bind_text(stmt_, index, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT),
                    action_, db_);
    }

    void bindText(int index, const std::wstring& value)
    {
        bindText(index, utf8(value));
    }

    void bindInt64(int index, int64_t value)
    {
        checkSqlite(sqlite3_bind_int64(stmt_, index, value), action_, db_);
    }

    void bindInt(int index, int value)
    {
        checkSqlite(sqlite3_bind_int(stmt_, index, value), action_, db_);
    }

    void bindDouble(int index, double value)
    {
        checkSqlite(sqlite3_bind_double(stmt_, index, value), action_, db_);
    }

    int step()
    {
        const int result = sqlite3_step(stmt_);
        if (result != SQLITE_ROW && result != SQLITE_DONE) fail(action_, db_);
        return result;
    }

private:
    sqlite3* db_{};
    sqlite3_stmt* stmt_{};
    const char* action_{};
};

void exec(sqlite3* db, const char* sql, const char* action)
{
    char* error = nullptr;
    const int result = sqlite3_exec(db, sql, nullptr, nullptr, &error);
    if (result != SQLITE_OK) {
        std::string message = action;
        if (error != nullptr) {
            message += ": ";
            message += error;
            sqlite3_free(error);
        } else {
            message += ": ";
            message += sqlite3_errmsg(db);
        }
        throw std::runtime_error(message);
    }
}

class Transaction {
public:
    explicit Transaction(sqlite3* db) : db_(db)
    {
        exec(db_, "BEGIN IMMEDIATE", "开启数据库事务失败");
    }

    ~Transaction()
    {
        if (active_) sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
    }

    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;

    void commit()
    {
        exec(db_, "COMMIT", "提交数据库事务失败");
        active_ = false;
    }

private:
    sqlite3* db_{};
    bool active_{true};
};

std::string columnText(sqlite3_stmt* stmt, int column)
{
    const unsigned char* text = sqlite3_column_text(stmt, column);
    const int length = sqlite3_column_bytes(stmt, column);
    return text == nullptr ? std::string{} : std::string(reinterpret_cast<const char*>(text), static_cast<size_t>(length));
}

void requireText(const std::wstring& value, const char* action)
{
    if (value.empty()) throw std::runtime_error(std::string(action) + ": 名称不能为空");
}

bool exists(sqlite3* db, const char* sql, const std::string& value, const char* action)
{
    Statement statement(db, sql, action);
    statement.bindText(1, value);
    const int result = statement.step();
    return result == SQLITE_ROW && sqlite3_column_int(statement.get(), 0) != 0;
}

void requireBook(sqlite3* db, const std::string& book)
{
    if (!exists(db, "SELECT EXISTS(SELECT 1 FROM books WHERE id=?1)", book, "检查书籍失败"))
        throw std::runtime_error("书籍不存在");
}

void requireTag(sqlite3* db, int64_t tag)
{
    Statement statement(db, "SELECT EXISTS(SELECT 1 FROM tags WHERE id=?1)", "检查标签失败");
    statement.bindInt64(1, tag);
    const int result = statement.step();
    if (result != SQLITE_ROW || sqlite3_column_int(statement.get(), 0) == 0)
        throw std::runtime_error("标签不存在");
}

constexpr const char* ReadingModeSetting = "reading.mode";
constexpr const char* ReadingFitSetting = "reading.fit";
constexpr const char* ReadingRtlSetting = "reading.rtl";

struct StoredReadingPreferences {
    ReadingPreferences value;
    bool complete{};
};

bool parseInteger(const std::string& text, int& value)
{
    if (text.empty()) return false;
    const char* first = text.data();
    const char* last = first + text.size();
    const auto result = std::from_chars(first, last, value);
    return result.ec == std::errc{} && result.ptr == last;
}

StoredReadingPreferences storedReadingPreferences(sqlite3* db)
{
    StoredReadingPreferences result;
    bool modePresent = false;
    bool fitPresent = false;
    bool rtlPresent = false;
    Statement statement(db, "SELECT key,value FROM settings WHERE key IN ('reading.mode','reading.fit','reading.rtl')",
                        "读取全局阅读偏好失败");
    while (statement.step() == SQLITE_ROW) {
        const std::string key = columnText(statement.get(), 0);
        const std::string value = columnText(statement.get(), 1);
        int parsed = 0;
        if (key == ReadingModeSetting) {
            modePresent = true;
            if (!parseInteger(value, parsed) || parsed < 0 || parsed > 2) parsed = 0;
            result.value.mode = parsed;
        } else if (key == ReadingFitSetting) {
            fitPresent = true;
            if (!parseInteger(value, parsed) || parsed < 0 || parsed > 3) parsed = 0;
            result.value.fit = parsed;
        } else if (key == ReadingRtlSetting) {
            rtlPresent = true;
            if (!parseInteger(value, parsed) || (parsed != 0 && parsed != 1)) parsed = 0;
            result.value.rtl = parsed != 0;
        }
    }
    result.complete = modePresent && fitPresent && rtlPresent;
    return result;
}

ReadingPreferences legacyReadingPreferences(sqlite3* db)
{
    ReadingPreferences result;
    // 仅查旧数据库，不要求上次阅读的文件仍在磁盘上。
    Statement statement(db, "SELECT r.mode,r.fit,r.rtl FROM settings s "
                            "JOIN books b ON b.path=s.value JOIN reading r ON r.book_id=b.id "
                            "WHERE s.key='lastBook' LIMIT 1", "读取旧阅读偏好失败");
    if (statement.step() != SQLITE_ROW) return result;
    const int mode = sqlite3_column_int(statement.get(), 0);
    const int fit = sqlite3_column_int(statement.get(), 1);
    const int rtl = sqlite3_column_int(statement.get(), 2);
    result.mode = mode >= 0 && mode <= 2 ? mode : 0;
    result.fit = fit >= 0 && fit <= 3 ? fit : 0;
    result.rtl = rtl == 1;
    return result;
}

} // namespace

Store::Store(const fs::path& file)
{
    if (file.empty()) throw std::runtime_error("数据库路径不能为空");

    std::error_code error;
    const fs::path parent = file.parent_path();
    if (!parent.empty()) {
        fs::create_directories(parent, error);
        if (error || !fs::is_directory(parent, error) || error)
            throw std::runtime_error("创建数据库目录失败");
    }

    std::string filename;
    try {
        filename = utf8(file.wstring());
    } catch (const std::exception& exception) {
        throw std::runtime_error(std::string("转换数据库路径失败: ") + exception.what());
    }
    sqlite3* opened = nullptr;
    const int result = sqlite3_open_v2(filename.c_str(), &opened,
                                       SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
    if (result != SQLITE_OK) {
        std::string message = "打开数据库失败";
        if (opened != nullptr) {
            message += ": ";
            message += sqlite3_errmsg(opened);
            sqlite3_close(opened);
        }
        throw std::runtime_error(message);
    }
    db_ = opened;

    try {
        checkSqlite(sqlite3_busy_timeout(db_, 5000), "设置数据库超时失败", db_);
        exec(db_, "PRAGMA foreign_keys=ON", "启用数据库外键失败");

        int version = 0;
        {
            Statement statement(db_, "PRAGMA user_version", "读取数据库版本失败");
            if (statement.step() != SQLITE_ROW) fail("读取数据库版本失败", db_);
            version = sqlite3_column_int(statement.get(), 0);
        }
        if (version > 1) throw std::runtime_error("数据库版本过高");

        Transaction transaction(db_);
        exec(db_, "CREATE TABLE IF NOT EXISTS books ("
                  "id TEXT PRIMARY KEY, path TEXT NOT NULL, size INTEGER NOT NULL DEFAULT 0)",
             "创建书籍表失败");
        exec(db_, "CREATE TABLE IF NOT EXISTS reading ("
                  "book_id TEXT PRIMARY KEY REFERENCES books(id) ON DELETE CASCADE, "
                  "entry TEXT NOT NULL DEFAULT '', page INTEGER NOT NULL DEFAULT 0, "
                  "half INTEGER NOT NULL DEFAULT 0, mode INTEGER NOT NULL DEFAULT 0, "
                  "fit INTEGER NOT NULL DEFAULT 0, rtl INTEGER NOT NULL DEFAULT 0, "
                  "zoom REAL NOT NULL DEFAULT 1.0)",
             "创建阅读状态表失败");
        exec(db_, "CREATE TABLE IF NOT EXISTS tags ("
                  "id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT NOT NULL UNIQUE)",
             "创建标签表失败");
        exec(db_, "CREATE TABLE IF NOT EXISTS book_tags ("
                  "book_id TEXT NOT NULL REFERENCES books(id) ON DELETE CASCADE, "
                  "tag_id INTEGER NOT NULL REFERENCES tags(id) ON DELETE CASCADE, "
                  "PRIMARY KEY(book_id, tag_id))",
             "创建书籍标签关联表失败");
        exec(db_, "CREATE TABLE IF NOT EXISTS settings ("
                  "key TEXT PRIMARY KEY, value TEXT NOT NULL)",
             "创建设置表失败");
        exec(db_, "CREATE TABLE IF NOT EXISTS passwords ("
                  "id INTEGER PRIMARY KEY AUTOINCREMENT, value TEXT NOT NULL UNIQUE)",
             "创建密码表失败");
        exec(db_, "CREATE TABLE IF NOT EXISTS book_passwords ("
                  "book_id TEXT PRIMARY KEY REFERENCES books(id) ON DELETE CASCADE, "
                  "password_id INTEGER NOT NULL REFERENCES passwords(id) ON DELETE CASCADE)",
             "创建书籍密码关联表失败");
        exec(db_, "PRAGMA user_version=1", "写入数据库版本失败");
        transaction.commit();
    } catch (...) {
        sqlite3_close(db_);
        db_ = nullptr;
        throw;
    }
}

Store::~Store()
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (db_ != nullptr) sqlite3_close(db_);
}

std::string Store::ensureBook(const fs::path& path)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (path.empty()) throw std::runtime_error("书籍路径不能为空");
    std::error_code error;
    const auto status = fs::status(path, error);
    if (error || (!fs::is_regular_file(status) && !fs::is_directory(status)))
        throw std::runtime_error("书籍文件或目录不存在或不可用");
    uintmax_t fileSize = 0;
    if (fs::is_regular_file(status)) {
        fileSize = fs::file_size(path, error);
        if (error || fileSize > static_cast<uintmax_t>(std::numeric_limits<int64_t>::max()))
            throw std::runtime_error("读取书籍文件大小失败");
    }

    std::string id;
    try {
        id = fileIdentity(path);
    } catch (const std::exception& exception) {
        throw std::runtime_error(std::string("获取书籍身份失败: ") + exception.what());
    }
    if (id.empty()) throw std::runtime_error("获取书籍身份失败");

    Statement statement(db_, "INSERT INTO books(id,path,size) VALUES(?1,?2,?3) "
                             "ON CONFLICT(id) DO UPDATE SET path=excluded.path,size=excluded.size",
                        "保存书籍失败");
    statement.bindText(1, id);
    statement.bindText(2, utf8(path.wstring()));
    statement.bindInt64(3, static_cast<int64_t>(fileSize));
    statement.step();
    return id;
}

ReadingState Store::reading(const std::string& book)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    ReadingState state;
    Statement statement(db_, "SELECT entry,page,half,mode,fit,rtl,zoom FROM reading WHERE book_id=?1", "读取阅读状态失败");
    statement.bindText(1, book);
    if (statement.step() == SQLITE_ROW) {
        state.entry = wide(columnText(statement.get(), 0));
        state.page = sqlite3_column_int(statement.get(), 1);
        state.half = sqlite3_column_int(statement.get(), 2);
        state.mode = sqlite3_column_int(statement.get(), 3);
        state.fit = sqlite3_column_int(statement.get(), 4);
        state.rtl = sqlite3_column_int(statement.get(), 5) != 0;
        state.zoom = sqlite3_column_double(statement.get(), 6);
    }
    return state;
}

void Store::saveReading(const std::string& book, const ReadingState& state)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    requireBook(db_, book);
    if (!std::isfinite(state.zoom)) throw std::runtime_error("保存阅读状态失败: 缩放值无效");
    Statement statement(db_, "INSERT INTO reading(book_id,entry,page,half,mode,fit,rtl,zoom) "
                             "VALUES(?1,?2,?3,?4,?5,?6,?7,?8) "
                             "ON CONFLICT(book_id) DO UPDATE SET entry=excluded.entry,page=excluded.page,"
                             "half=excluded.half,mode=excluded.mode,fit=excluded.fit,rtl=excluded.rtl,zoom=excluded.zoom",
                        "保存阅读状态失败");
    statement.bindText(1, book);
    statement.bindText(2, state.entry);
    statement.bindInt(3, state.page);
    statement.bindInt(4, state.half);
    statement.bindInt(5, state.mode);
    statement.bindInt(6, state.fit);
    statement.bindInt(7, state.rtl ? 1 : 0);
    statement.bindDouble(8, state.zoom);
    statement.step();
}

ReadingPreferences Store::readingPreferences()
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const auto stored = storedReadingPreferences(db_);
    if (stored.complete) return stored.value;

    const ReadingPreferences migrated = legacyReadingPreferences(db_);
    saveReadingPreferences(migrated);
    return migrated;
}

void Store::saveReadingPreferences(const ReadingPreferences& preferences)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    ReadingPreferences normalized = preferences;
    if (normalized.mode < 0 || normalized.mode > 2) normalized.mode = 0;
    if (normalized.fit < 0 || normalized.fit > 3) normalized.fit = 0;

    Transaction transaction(db_);
    const char* sql = "INSERT INTO settings(key,value) VALUES(?1,?2) "
                      "ON CONFLICT(key) DO UPDATE SET value=excluded.value";
    const auto save = [&](const char* key, int value) {
        Statement statement(db_, sql, "保存全局阅读偏好失败");
        statement.bindText(1, key);
        statement.bindText(2, std::to_string(value));
        statement.step();
    };
    save(ReadingModeSetting, normalized.mode);
    save(ReadingFitSetting, normalized.fit);
    save(ReadingRtlSetting, normalized.rtl ? 1 : 0);
    transaction.commit();
}

std::wstring Store::setting(const std::string& key, const std::wstring& fallback)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    Statement statement(db_, "SELECT value FROM settings WHERE key=?1", "读取设置失败");
    statement.bindText(1, key);
    if (statement.step() != SQLITE_ROW) return fallback;
    return wide(columnText(statement.get(), 0));
}

void Store::setSetting(const std::string& key, const std::wstring& value)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (key.empty()) throw std::runtime_error("保存设置失败: 键不能为空");
    Statement statement(db_, "INSERT INTO settings(key,value) VALUES(?1,?2) "
                             "ON CONFLICT(key) DO UPDATE SET value=excluded.value",
                        "保存设置失败");
    statement.bindText(1, key);
    statement.bindText(2, value);
    statement.step();
}

std::vector<Tag> Store::tags()
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::vector<Tag> result;
    Statement statement(db_, "SELECT id,name FROM tags ORDER BY name COLLATE NOCASE,id", "读取标签失败");
    while (statement.step() == SQLITE_ROW) {
        Tag tag;
        tag.id = sqlite3_column_int64(statement.get(), 0);
        tag.name = wide(columnText(statement.get(), 1));
        result.push_back(std::move(tag));
    }
    return result;
}

void Store::createTag(const std::wstring& name)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    requireText(name, "创建标签失败");
    Statement statement(db_, "INSERT INTO tags(name) VALUES(?1)", "创建标签失败");
    statement.bindText(1, name);
    statement.step();
}

void Store::renameTag(int64_t tag, const std::wstring& name)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    requireText(name, "重命名标签失败");
    Statement statement(db_, "UPDATE tags SET name=?1 WHERE id=?2", "重命名标签失败");
    statement.bindText(1, name);
    statement.bindInt64(2, tag);
    statement.step();
    if (sqlite3_changes(db_) == 0) throw std::runtime_error("标签不存在");
}

void Store::deleteTag(int64_t tag)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    Statement statement(db_, "DELETE FROM tags WHERE id=?1", "删除标签失败");
    statement.bindInt64(1, tag);
    statement.step();
    if (sqlite3_changes(db_) == 0) throw std::runtime_error("标签不存在");
}

void Store::assignTags(const std::vector<std::string>& books, const std::vector<int64_t>& tagIds, bool add)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (books.empty() || tagIds.empty()) return;
    for (const auto& book : books) requireBook(db_, book);
    for (const auto tag : tagIds) requireTag(db_, tag);

    Transaction transaction(db_);
    const char* sql = add
        ? "INSERT OR IGNORE INTO book_tags(book_id,tag_id) VALUES(?1,?2)"
        : "DELETE FROM book_tags WHERE book_id=?1 AND tag_id=?2";
    for (const auto& book : books) {
        for (const auto tag : tagIds) {
            Statement statement(db_, sql, add ? "添加书籍标签失败" : "移除书籍标签失败");
            statement.bindText(1, book);
            statement.bindInt64(2, tag);
            statement.step();
        }
    }
    transaction.commit();
}

std::vector<Tag> Store::bookTagItems(const std::string& book)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    Statement statement(db_, "SELECT t.id,t.name FROM tags t JOIN book_tags bt ON bt.tag_id=t.id "
                             "WHERE bt.book_id=?1 ORDER BY t.name COLLATE NOCASE,t.id",
                        "读取书籍标签失败");
    statement.bindText(1, book);
    std::vector<Tag> result;
    while (statement.step() == SQLITE_ROW) result.push_back({sqlite3_column_int64(statement.get(), 0), wide(columnText(statement.get(), 1))});
    return result;
}

std::wstring Store::bookTags(const std::string& book)
{
    std::wstring result;
    for (const auto& tag : bookTagItems(book)) {
        if (!result.empty()) result += L", ";
        result += tag.name;
    }
    return result;
}

bool Store::matches(const std::string& book, const std::vector<int64_t>& tagIds, bool all)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (tagIds.empty()) return true;

    std::string sql = all
        ? "SELECT COUNT(DISTINCT tag_id) FROM book_tags WHERE book_id=?1 AND tag_id IN ("
        : "SELECT EXISTS(SELECT 1 FROM book_tags WHERE book_id=?1 AND tag_id IN (";
    for (size_t i = 0; i < tagIds.size(); ++i) {
        if (i != 0) sql += ',';
        sql += '?';
        sql += std::to_string(i + 2);
    }
    sql += all ? ")" : "))";
    Statement statement(db_, sql.c_str(), "查询标签匹配失败");
    statement.bindText(1, book);
    for (size_t i = 0; i < tagIds.size(); ++i) statement.bindInt64(static_cast<int>(i + 2), tagIds[i]);
    if (statement.step() != SQLITE_ROW) fail("查询标签匹配失败", db_);
    if (!all) return sqlite3_column_int(statement.get(), 0) != 0;
    return sqlite3_column_int64(statement.get(), 0) == static_cast<sqlite3_int64>(tagIds.size());
}

std::vector<Password> Store::passwords()
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::vector<Password> result;
    Statement statement(db_, "SELECT id,value FROM passwords ORDER BY id", "读取密码失败");
    while (statement.step() == SQLITE_ROW) {
        Password password;
        password.id = sqlite3_column_int64(statement.get(), 0);
        password.value = wide(columnText(statement.get(), 1));
        result.push_back(std::move(password));
    }
    return result;
}

void Store::addPassword(const std::wstring& password)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    requireText(password, "添加密码失败");
    Statement statement(db_, "INSERT INTO passwords(value) VALUES(?1)", "添加密码失败");
    statement.bindText(1, password);
    statement.step();
}

void Store::updatePassword(int64_t id, const std::wstring& password)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    requireText(password, "更新密码失败");
    Statement statement(db_, "UPDATE passwords SET value=?1 WHERE id=?2", "更新密码失败");
    statement.bindText(1, password);
    statement.bindInt64(2, id);
    statement.step();
    if (sqlite3_changes(db_) == 0) throw std::runtime_error("密码不存在");
}

void Store::deletePassword(int64_t id)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    Statement statement(db_, "DELETE FROM passwords WHERE id=?1", "删除密码失败");
    statement.bindInt64(1, id);
    statement.step();
    if (sqlite3_changes(db_) == 0) throw std::runtime_error("密码不存在");
}

std::wstring Store::bookPassword(const std::string& book)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    Statement statement(db_, "SELECT p.value FROM passwords p JOIN book_passwords bp ON bp.password_id=p.id "
                             "WHERE bp.book_id=?1", "读取书籍密码失败");
    statement.bindText(1, book);
    if (statement.step() != SQLITE_ROW) return L"";
    return wide(columnText(statement.get(), 0));
}

void Store::rememberPassword(const std::string& book, const std::wstring& password)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    requireBook(db_, book);
    requireText(password, "保存书籍密码失败");
    Transaction transaction(db_);
    {
        Statement statement(db_, "INSERT OR IGNORE INTO passwords(value) VALUES(?1)", "保存书籍密码失败");
        statement.bindText(1, password);
        statement.step();
    }
    int64_t passwordId = 0;
    {
        Statement statement(db_, "SELECT id FROM passwords WHERE value=?1", "读取书籍密码失败");
        statement.bindText(1, password);
        if (statement.step() != SQLITE_ROW) fail("读取书籍密码失败", db_);
        passwordId = sqlite3_column_int64(statement.get(), 0);
    }
    {
        Statement statement(db_, "INSERT INTO book_passwords(book_id,password_id) VALUES(?1,?2) "
                                 "ON CONFLICT(book_id) DO UPDATE SET password_id=excluded.password_id",
                            "关联书籍密码失败");
        statement.bindText(1, book);
        statement.bindInt64(2, passwordId);
        statement.step();
    }
    transaction.commit();
}

} // namespace cv
