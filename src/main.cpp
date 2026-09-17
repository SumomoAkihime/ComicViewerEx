#include "reader.hpp"
#include "resource.h"
#include "sidebar.hpp"
#include "tag_bar.hpp"
#include <commctrl.h>
#include <d2d1.h>
#include <dwrite.h>
#include <shellapi.h>
#include <shlobj.h>
#include <windowsx.h>
#include <wrl/client.h>
#include <chrono>
#include <cmath>
#include <fstream>
#include <sstream>
#include <unordered_map>

using namespace cv;
using Microsoft::WRL::ComPtr;
namespace {
constexpr UINT WM_READER = WM_APP + 1, WM_CATALOG = WM_APP + 2, WM_INITIALIZE = WM_APP + 3;
constexpr UINT WM_TAG_CONTEXT = WM_APP + 13;
enum Command {
    Open = 200, ReadFolder, Library, Previous, Next, Single, Dual, Split, Direction,
    FitPage, FitWidth, FitHeight, Actual, ZoomIn, ZoomOut, Fullscreen, Jump,
    Passwords, CancelRead, Refresh, TagCreate, TagRename, TagDelete, TagAdd, TagRemove, TagClear,
    Exit, About, Recursive, MatchMode, TagsList, FilesList, DirectoryTree, TagFilter, DirectoryDropdown, TagTabs, FilterTagsList
};
HINSTANCE instance;
LRESULT CALLBACK filesProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);

void showError(HWND owner, const std::wstring& text) { MessageBoxW(owner, text.c_str(), L"ComicViewerEx", MB_OK | MB_ICONWARNING); }
std::wstring windowText(HWND window) {
    int length = GetWindowTextLengthW(window);
    std::wstring value(length + 1, L'\0'); GetWindowTextW(window, value.data(), length + 1); value.resize(length); return value;
}
struct Input { std::wstring title, label, value; bool password{}, remember{}, accepted{}; };
INT_PTR CALLBACK inputProc(HWND window, UINT message, WPARAM w, LPARAM l) {
    auto data = reinterpret_cast<Input*>(GetWindowLongPtrW(window, DWLP_USER));
    if (message == WM_INITDIALOG) {
        data = reinterpret_cast<Input*>(l); SetWindowLongPtrW(window, DWLP_USER, l);
        SetWindowTextW(window, data->title.c_str()); SetDlgItemTextW(window, IDC_LABEL, data->label.c_str()); SetDlgItemTextW(window, IDC_VALUE, data->value.c_str());
        ShowWindow(GetDlgItem(window, IDC_REMEMBER), data->password ? SW_SHOW : SW_HIDE);
        if (data->password) SendDlgItemMessageW(window, IDC_VALUE, EM_SETPASSWORDCHAR, L'●', 0);
        CheckDlgButton(window, IDC_REMEMBER, data->remember ? BST_CHECKED : BST_UNCHECKED);
        SendDlgItemMessageW(window, IDC_VALUE, EM_SETSEL, 0, -1); SetFocus(GetDlgItem(window, IDC_VALUE)); return FALSE;
    }
    if (message == WM_COMMAND) {
        if (LOWORD(w) == IDOK) { data->value = windowText(GetDlgItem(window, IDC_VALUE)); data->remember = IsDlgButtonChecked(window, IDC_REMEMBER) == BST_CHECKED; data->accepted = true; EndDialog(window, IDOK); return TRUE; }
        if (LOWORD(w) == IDCANCEL) { EndDialog(window, IDCANCEL); return TRUE; }
    }
    return FALSE;
}
bool prompt(HWND owner, Input& value) { DialogBoxParamW(instance, MAKEINTRESOURCEW(IDD_INPUT), owner, inputProc, reinterpret_cast<LPARAM>(&value)); return value.accepted; }
fs::path selectPath(HWND owner, bool folder) {
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return {};
    DWORD flags{}; dialog->GetOptions(&flags); dialog->SetOptions(flags | FOS_FORCEFILESYSTEM | (folder ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST));
    dialog->SetTitle(folder ? L"选择漫画目录" : L"打开漫画压缩包或图片");
    COMDLG_FILTERSPEC filters[] = {{L"漫画与图片", L"*.zip;*.cbz;*.rar;*.cbr;*.7z;*.jpg;*.jpeg;*.png;*.webp;*.bmp;*.gif;*.tif;*.tiff"}, {L"所有文件", L"*.*"}};
    if (!folder) dialog->SetFileTypes(2, filters);
    if (FAILED(dialog->Show(owner))) return {};
    ComPtr<IShellItem> item; if (FAILED(dialog->GetResult(&item))) return {};
    PWSTR value{}; if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &value))) return {};
    fs::path result(value); CoTaskMemFree(value); return result;
}

struct CatalogEvent { uint64_t generation{}; std::vector<BookRow> rows; std::wstring error; };
class Catalog {
public:
    Catalog(Store& store, HWND window) : store_(store), window_(window), worker_([this] { run(); }) {}
    ~Catalog() { { std::lock_guard lock(mutex_); stopped_ = true; ++generation_; } wake_.notify_one(); worker_.join(); }
    uint64_t scan(fs::path path, bool recursive, std::vector<int64_t> tags, bool all) {
        std::lock_guard lock(mutex_); request_ = {++generation_, std::move(path), recursive, std::move(tags), all}; pending_ = true; wake_.notify_one(); return generation_;
    }
private:
    struct Request { uint64_t generation{}; fs::path path; bool recursive{}; std::vector<int64_t> tags; bool all{true}; } request_;
    void run() {
        for (;;) {
            Request request;
            { std::unique_lock lock(mutex_); wake_.wait(lock, [&] { return stopped_ || pending_; }); if (stopped_) return; request = request_; pending_ = false; }
            auto event = std::make_unique<CatalogEvent>(); event->generation = request.generation;
            try {
                auto add = [&](const fs::directory_entry& file) {
                    if (generation_ != request.generation) throw Cancelled{};
                    std::error_code error;
                    if (!file.is_regular_file(error) || !isArchive(file.path())) return;
                    BookRow row; row.path = file.path(); row.id = fileIdentity(row.path); row.size = file.file_size(error);
                    if (!store_.matches(row.id, request.tags, request.all)) return;
                    row.tags = store_.bookTags(row.id); event->rows.push_back(std::move(row));
                };
                if (request.recursive) { for (const auto& file : fs::recursive_directory_iterator(request.path, fs::directory_options::skip_permission_denied)) add(file); }
                else { for (const auto& file : fs::directory_iterator(request.path, fs::directory_options::skip_permission_denied)) add(file); }
                std::sort(event->rows.begin(), event->rows.end(), [](auto& a, auto& b) { return naturalLess(a.path.wstring(), b.path.wstring()); });
            } catch (const Cancelled&) { continue; }
            catch (const std::exception& e) { event->error = L"扫描目录失败：" + wide(e.what()); }
            if (generation_ == request.generation && PostMessageW(window_, WM_CATALOG, 0, reinterpret_cast<LPARAM>(event.get()))) event.release();
        }
    }
    Store& store_; HWND window_; std::mutex mutex_; std::condition_variable wake_; std::atomic<uint64_t> generation_{};
    bool pending_{}, stopped_{}; std::thread worker_;
};

class Application {
public:
    HWND window{}, view{}, files{}, tags{}, recursive{}, match{}, status{};
    Sidebar sidebar;
    TagBar tagBar;
    HFONT font{};
    HMENU mainMenu{};
    std::unique_ptr<Store> store;
    std::unique_ptr<Reader> reader;
    std::unique_ptr<Catalog> catalog;
    fs::path directory, bookPath, commandLine, cacheRoot;
    std::string bookId;
    std::vector<BookRow> rows;
    std::vector<Tag> tagItems;
    std::vector<int64_t> filterTags;
    std::vector<PageEntry> entries;
    std::shared_ptr<Pixels> first, second;
    ReadingState state;
    ReadingPreferences preferences;
    uint64_t readerGeneration{}, catalogGeneration{};
    bool opening{}, busy{}, full{}, applyingTags{}, shuttingDown{};
    bool tagContextPending{};
    std::optional<std::wstring> pendingPassword;
    bool pendingRemember{};
    int64_t pendingPasswordReplacement{};
    fs::path listedDirectory;
    int requestedPage{};
    double panX{}, panY{};
    POINT dragPoint{}, pressPoint{};
    bool pressing{}, dragging{};
    WINDOWPLACEMENT placement{sizeof(WINDOWPLACEMENT)};
    HWND fullscreenFocus{};
    ComPtr<ID2D1Factory> factory;
    ComPtr<ID2D1HwndRenderTarget> target;
    ComPtr<ID2D1Bitmap> bitmap1, bitmap2;
    ComPtr<IDWriteFactory> writeFactory;
    ComPtr<IDWriteTextFormat> textFormat;
    bool bitmapsDirty{true};
    std::vector<HWND> buttons;
    std::wstring statusText{L"就绪"};

    void setStatus(const std::wstring& text) { statusText = text; SendMessageW(status, SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(text.c_str())); }
    HWND control(const wchar_t* type, const wchar_t* text, DWORD style, int id, DWORD extended = 0) {
        HWND child = CreateWindowExW(extended, type, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 100, 28, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
        SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE); return child;
    }
    HMENU makeMenu() {
        HMENU menu = CreateMenu(), file = CreatePopupMenu(), read = CreatePopupMenu(), tag = CreatePopupMenu(), help = CreatePopupMenu();
        auto item = [](HMENU menu, UINT id, const wchar_t* title) { AppendMenuW(menu, MF_STRING, id, title); };
        item(file, Open, L"打开压缩包/图片…\tCtrl+O"); item(file, ReadFolder, L"阅读图片目录…\tCtrl+D"); item(file, Library, L"浏览漫画目录…\tCtrl+L");
        AppendMenuW(file, MF_SEPARATOR, 0, nullptr); item(file, Passwords, L"常用密码表…"); item(file, CancelRead, L"取消读取\tEsc"); item(file, Exit, L"退出");
        item(read, Previous, L"上一页\t← / PgUp"); item(read, Next, L"下一页\t→ / PgDn / 空格"); item(read, Jump, L"跳转页码…\tCtrl+G");
        AppendMenuW(read, MF_SEPARATOR, 0, nullptr); item(read, Single, L"单页\t1"); item(read, Dual, L"双页\t2"); item(read, Split, L"宽图分页\t3"); item(read, Direction, L"从右向左阅读");
        AppendMenuW(read, MF_SEPARATOR, 0, nullptr); item(read, FitPage, L"适合页面\tF"); item(read, FitWidth, L"适合宽度\tW"); item(read, FitHeight, L"适合高度\tH"); item(read, Actual, L"原始比例\t0");
        item(read, ZoomIn, L"放大\t+ / Ctrl+滚轮"); item(read, ZoomOut, L"缩小\t-"); item(read, Fullscreen, L"全屏\tF11 / 鼠标中键");
        item(tag, TagCreate, L"新建标签…"); item(tag, TagRename, L"重命名选中标签…"); item(tag, TagDelete, L"删除选中标签");
        AppendMenuW(tag, MF_SEPARATOR, 0, nullptr); item(tag, TagAdd, L"为选中文件添加所选标签"); item(tag, TagRemove, L"从选中文件移除所选标签"); item(tag, TagClear, L"清除标签筛选");
        item(help, About, L"使用说明与关于");
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(file), L"文件"); AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(read), L"阅读"); AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(tag), L"标签"); AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(help), L"帮助"); return menu;
    }
    void create() {
        font = CreateFontW(-MulDiv(10, GetDpiForWindow(window), 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");
        mainMenu = makeMenu(); SetMenu(window, mainMenu);
        const std::pair<int, const wchar_t*> labels[] = {{Open,L"打开"},{Library,L"目录"},{Previous,L"上一页"},{Next,L"下一页"},{Single,L"单页"},{Dual,L"双页"},{Split,L"分页"},{FitPage,L"适合页面"},{Fullscreen,L"全屏"},{CancelRead,L"取消"}};
        for (auto& [id, text] : labels) buttons.push_back(control(L"BUTTON", text,
            ((id >= Single && id <= Split) || id == FitPage) ? BS_CHECKBOX | BS_PUSHLIKE : BS_PUSHBUTTON, id));
        sidebar.create(instance, window, font, {Recursive, MatchMode, TagsList, FilesList, DirectoryTree,
            TagCreate, TagClear, TagFilter, TagAdd, TagRemove, DirectoryDropdown, TagTabs, FilterTagsList});
        files = sidebar.files; tags = sidebar.tags; recursive = sidebar.recursive; match = sidebar.match;
        SetWindowSubclass(files, filesProc, 1, reinterpret_cast<DWORD_PTR>(this));
        view = CreateWindowExW(0, L"ComicViewerEx.Canvas", nullptr, WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 0, 0, 100, 100, window, nullptr, instance, this);
        status = CreateWindowExW(0, STATUSCLASSNAMEW, L"就绪", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window, nullptr, instance, nullptr);
        SendMessageW(status, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        tagBar.create(instance, window, font);
        tagBar.setContent(L"选择文件查看标签 · 点击标签气泡切换筛选", {}, {});
        D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory.GetAddressOf());
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(writeFactory.GetAddressOf()));
        if (writeFactory) writeFactory->CreateTextFormat(L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, static_cast<float>(px(18)), L"zh-CN", &textFormat);
        DragAcceptFiles(window, TRUE); updateReadingControls(); layout();
    }
    int px(int n) { return MulDiv(n, GetDpiForWindow(window), 96); }
    void refreshFonts() {
        HFONT replacement = CreateFontW(-MulDiv(10, GetDpiForWindow(window), 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");
        if (!replacement) return;
        for (auto button : buttons) SendMessageW(button, WM_SETFONT, reinterpret_cast<WPARAM>(replacement), TRUE);
        SendMessageW(status, WM_SETFONT, reinterpret_cast<WPARAM>(replacement), TRUE); sidebar.setFont(replacement);
        tagBar.setFont(replacement);
        if (font) DeleteObject(font); font = replacement;
    }
    void layout() {
        if (!view || !status || !sidebar.window) return;
        RECT rect{}; GetClientRect(window, &rect); int width = rect.right, height = rect.bottom;
        SendMessageW(status, WM_SIZE, 0, 0);
        const int top = full ? 0 : px(42), statusHeight = full ? 0 : px(24);
        const int tagHeight = full ? 0 : tagBar.preferredHeight(width), bottom = statusHeight + tagHeight;
        int x = px(8);
        for (size_t i = 0; i < buttons.size(); ++i) { ShowWindow(buttons[i], full ? SW_HIDE : SW_SHOW); int w = px(i == 7 ? 86 : 65); MoveWindow(buttons[i], x, px(7), w, px(28), TRUE); x += w + px(5); }
        ShowWindow(status, full ? SW_HIDE : SW_SHOW);
        ShowWindow(tagBar.window, full ? SW_HIDE : SW_SHOW);
        if (!full) MoveWindow(tagBar.window, 0, std::max(0, height - bottom), width, tagHeight, TRUE);
        auto area = sidebar.layout(width, height, top, bottom, full);
        MoveWindow(view, area.left, area.top, std::max(1L, area.right - area.left), std::max(1L, area.bottom - area.top), TRUE);
        if (target) { RECT canvasArea{}; GetClientRect(view, &canvasArea); target->Resize(D2D1::SizeU(canvasArea.right, canvasArea.bottom)); }
        InvalidateRect(view, nullptr, FALSE);
    }
    void initialize() {
        store = std::make_unique<Store>(executableDir() / L"data" / L"library.db");
        preferences = store->readingPreferences(); preferences.apply(state); updateReadingControls();
        sidebar.right = store->setting("sidebar.side") == L"right";
        try { sidebar.widthDip = std::clamp(std::stoi(store->setting("sidebar.width", L"380")), 280, 10000); } catch (...) { sidebar.widthDip = 380; }
        layout();
        cacheRoot = executableDir() / L"data" / L"cache";
        std::error_code ec;
        // Remove only filenames generated by this app, never an arbitrary directory tree.
        if (fs::exists(cacheRoot, ec)) {
            if (GetFileAttributesW(cacheRoot.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT) throw std::runtime_error("缓存目录不能是符号链接或目录联接");
            for (const auto& item : fs::directory_iterator(cacheRoot)) {
                auto name = item.path().stem().wstring();
                if (item.is_regular_file() && item.path().extension() == L".cvpage" && name.size() == 16 && std::all_of(name.begin(), name.end(), [](wchar_t c) { return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f'); })) fs::remove(item.path(), ec);
            }
        }
        fs::create_directories(cacheRoot);
        reader = std::make_unique<Reader>(*store, cacheRoot, [this](auto event) { if (PostMessageW(window, WM_READER, 0, reinterpret_cast<LPARAM>(event.get()))) event.release(); });
        catalog = std::make_unique<Catalog>(*store, window);
        refreshTags();
        directory = store->setting("directory");
        if (!directory.empty() && fs::is_directory(directory, ec)) scan();
        if (!commandLine.empty()) openPath(commandLine);
        else { const auto last = store->setting("lastBook"); if (!last.empty() && fs::exists(fs::path(last), ec)) openPath(last, !directory.empty() && fs::is_directory(directory, ec)); }
    }
    void refreshTags() {
        if (!store) return;
        auto selected = selectedTags();
        auto updated = store->tags(); applyingTags = true;
        ListView_DeleteAllItems(tags); ListView_DeleteAllItems(sidebar.filterTags); tagItems = std::move(updated);
        std::erase_if(filterTags, [&](auto id) { return std::none_of(tagItems.begin(), tagItems.end(), [&](const auto& tag) { return tag.id == id; }); });
        for (int i = 0; i < static_cast<int>(tagItems.size()); ++i) {
            for (HWND list : {tags, sidebar.filterTags}) {
                LVITEMW item{}; item.mask = LVIF_TEXT; item.iItem = i; item.pszText = tagItems[i].name.data();
                ListView_InsertItem(list, &item);
                const auto& checked = list == tags ? selected : filterTags;
                ListView_SetCheckState(list, i, std::find(checked.begin(), checked.end(), tagItems[i].id) != checked.end());
            }
        }
        applyingTags = false;
        requestTagContext();
    }
    std::vector<int64_t> selectedTags(bool filter = false) {
        HWND list = filter ? sidebar.filterTags : tags;
        std::vector<int64_t> result;
        for (int i = 0; i < static_cast<int>(tagItems.size()); ++i) if (ListView_GetCheckState(list, i)) result.push_back(tagItems[i].id);
        return result;
    }
    void syncFilterChecks() {
        applyingTags = true;
        for (int i = 0; i < static_cast<int>(tagItems.size()); ++i)
            ListView_SetCheckState(sidebar.filterTags, i, std::find(filterTags.begin(), filterTags.end(), tagItems[i].id) != filterTags.end());
        applyingTags = false;
        requestTagContext();
    }
    void requestTagContext() {
        if (!tagContextPending && !shuttingDown) tagContextPending = PostMessageW(window, WM_TAG_CONTEXT, 0, 0) != FALSE;
    }
    void refreshTagContext() {
        tagContextPending = false;
        if (!store || shuttingDown) return;
        const int count = ListView_GetSelectedCount(files);
        sidebar.setTagSummary(count, static_cast<int>(filterTags.size()));
        std::string id;
        std::wstring context;
        if (count > 0) {
            int i = ListView_GetNextItem(files, -1, count > 1 ? LVNI_FOCUSED : LVNI_SELECTED);
            if (i < 0) i = ListView_GetNextItem(files, -1, LVNI_SELECTED);
            if (i >= 0 && i < static_cast<int>(rows.size())) {
                id = rows[i].id;
                context = (count > 1 ? L"已选 " + std::to_wstring(count) + L" 个文件 · 焦点：" : L"文件：") + rows[i].path.filename().wstring();
            }
        }
        if (id.empty() && !bookId.empty()) { id = bookId; context = L"阅读中：" + bookPath.filename().wstring(); }
        if (context.empty()) context = L"选择文件查看标签";
        RECT client{}; GetClientRect(window, &client); const int previousHeight = tagBar.preferredHeight(client.right);
        tagBar.setContent(context, id.empty() ? std::vector<Tag>{} : store->bookTagItems(id), filterTags);
        if (previousHeight != tagBar.preferredHeight(client.right)) layout();
    }
    void toggleTagFilter(int64_t id) {
        if (std::none_of(tagItems.begin(), tagItems.end(), [&](const auto& tag) { return tag.id == id; })) return;
        auto found = std::find(filterTags.begin(), filterTags.end(), id);
        if (found == filterTags.end()) filterTags.push_back(id); else filterTags.erase(found);
        syncFilterChecks(); sidebar.selectTagPage(false); scan();
    }
    void scan() {
        if (!catalog || directory.empty()) return;
        sidebar.setDirectory(directory); setStatus(L"正在扫描目录…");
        catalogGeneration = catalog->scan(directory, Button_GetCheck(recursive) == BST_CHECKED, filterTags, SendMessageW(match, CB_GETCURSEL, 0, 0) == 0);
        store->setSetting("directory", directory.wstring());
    }
    void updateReadingControls() {
        CheckMenuRadioItem(mainMenu, Single, Split, Single + preferences.mode, MF_BYCOMMAND);
        CheckMenuItem(mainMenu, Direction, MF_BYCOMMAND | (preferences.rtl ? MF_CHECKED : MF_UNCHECKED));
        CheckMenuRadioItem(mainMenu, FitPage, Actual, FitPage + preferences.fit, MF_BYCOMMAND);
        for (int id : {Single, Dual, Split, FitPage}) Button_SetCheck(GetDlgItem(window, id),
            (id == FitPage ? preferences.fit == 0 : id - Single == preferences.mode) ? BST_CHECKED : BST_UNCHECKED);
    }
    void saveSidebar() {
        if (!store) return;
        store->setSetting("sidebar.side", sidebar.right ? L"right" : L"left");
        store->setSetting("sidebar.width", std::to_wstring(sidebar.widthDip));
    }
    void submitOpen() {
        opening = busy = true;
        readerGeneration = reader->open(bookPath, bookId, state, pendingPassword, pendingRemember, pendingPasswordReplacement);
        setStatus(L"正在打开漫画… 可按 Esc 取消");
    }
    void saveReading() {
        if (store && !bookId.empty() && first && !opening) store->saveReading(bookId, state);
    }
    void openPath(fs::path path, bool keepCatalog = false) {
        if (!reader) return;
        std::error_code ec;
        std::wstring chosenImage;
        if (isImage(path)) { chosenImage = path.filename().wstring(); path = path.parent_path(); }
        if (!fs::exists(path, ec)) throw std::runtime_error("文件或目录不存在");
        if (!fs::is_directory(path, ec) && !isArchive(path)) throw std::runtime_error("不支持此文件格式");
        auto absolute = fs::absolute(path).lexically_normal();
        if (keepCatalog && lower(absolute.wstring()) == lower(bookPath.wstring()) && (opening || first)) return;
        saveReading();
        auto id = store->ensureBook(absolute); auto saved = store->reading(id);
        bookPath = absolute; bookId = id; state = saved; preferences.apply(state);
        if (!chosenImage.empty()) state.entry = chosenImage;
        state.mode = std::clamp(state.mode, 0, 2); state.fit = std::clamp(state.fit, 0, 3); state.half = std::clamp(state.half, 0, 1); state.zoom = std::clamp(state.zoom, .05, 16.0);
        entries.clear(); first.reset(); second.reset(); bitmap1.Reset(); bitmap2.Reset(); opening = busy = true; requestedPage = state.page;
        panX = panY = 0; pendingPassword.reset(); pendingRemember = false; pendingPasswordReplacement = 0;
        submitOpen();
        if (!keepCatalog) { directory = fs::is_directory(absolute, ec) ? absolute : absolute.parent_path(); scan(); SetFocus(view); }
        SetWindowTextW(window, (bookPath.filename().wstring() + L" — ComicViewerEx").c_str()); InvalidateRect(view, nullptr, FALSE);
        requestTagContext();
    }
    void openListItem(int index) {
        if (index < 0 || index >= static_cast<int>(rows.size()) || GetKeyState(VK_CONTROL) < 0 || GetKeyState(VK_SHIFT) < 0) return;
        openPath(rows[index].path, true);
    }
    void refreshPage(bool backwards = false) {
        if (!reader || entries.empty() || opening) return;
        busy = true; readerGeneration = reader->load(requestedPage, state, backwards); setStatus(L"正在读取页面… 可按 Esc 取消");
    }
    void turn(int delta) {
        if (entries.empty() || opening) return;
        if (state.mode == 2 && first && first->width > first->height && !busy) {
            if ((delta > 0 && state.half == 0) || (delta < 0 && state.half == 1)) { state.half = delta > 0 ? 1 : 0; panX = panY = 0; saveReading(); updateStatus(); InvalidateRect(view, nullptr, FALSE); return; }
        }
        int step = delta > 0 && state.mode == 1 && second ? 2 : 1;
        int next = std::clamp(requestedPage + delta * step, 0, static_cast<int>(entries.size()) - 1);
        if (next == requestedPage) return;
        requestedPage = next; state.half = 0; refreshPage(delta < 0);
    }
    void updateStatus() {
        if (!first || entries.empty()) return;
        std::wstring value = L"第 " + std::to_wstring(state.page + 1);
        if (second) value += L"–" + std::to_wstring(state.page + 2);
        value += L" / " + std::to_wstring(entries.size()) + L" 页　" + entries[state.page].name;
        if (state.mode == 2 && first->width > first->height) value += state.half ? L"　后半页" : L"　前半页";
        if (first->originalWidth > first->width || (second && second->originalWidth > second->width)) value += L"　大图已按内存预算缩小解码";
        setStatus(value);
    }
    void acceptReader(std::unique_ptr<ReaderEvent> event) {
        if (event->generation != readerGeneration) return;
        if (event->kind == ReaderEvent::ProgressUpdate) { setStatus(event->message + L"　Esc 取消"); return; }
        if (event->kind == ReaderEvent::NeedPassword) {
            busy = false;
            Input input{L"压缩包密码", L"请输入密码（取消后可打开其他漫画）", L"", true};
            HWND focus = GetFocus();
            if (prompt(window, input)) { pendingPassword = input.value; pendingRemember = input.remember; submitOpen(); }
            else { opening = false; pendingPassword.reset(); setStatus(L"已取消打开"); }
            if (IsWindow(focus)) SetFocus(focus);
            return;
        }
        if (event->kind == ReaderEvent::Error) { busy = opening = false; pendingPassword.reset(); requestedPage = state.page; setStatus(event->message); showError(window, event->message); return; }
        if (event->kind == ReaderEvent::Opened) { entries = std::move(event->entries); opening = false; pendingPassword.reset(); pendingPasswordReplacement = 0; store->setSetting("lastBook", bookPath.wstring()); }
        // 解码结果只带回阅读位置；全局开关和当前缩放以界面的最新输入为准。
        state.page = event->state.page; state.entry = event->state.entry; state.half = event->state.half; preferences.apply(state);
        requestedPage = state.page; first = std::move(event->first); second = std::move(event->second);
        busy = false; bitmapsDirty = true; panX = panY = 0; saveReading(); updateStatus(); updateReadingControls(); InvalidateRect(view, nullptr, FALSE);
    }
    void acceptCatalog(std::unique_ptr<CatalogEvent> event) {
        if (event->generation != catalogGeneration) return;
        std::vector<fs::path> selected;
        fs::path focused, topPath;
        if (listedDirectory == directory) {
            for (int i = -1; (i = ListView_GetNextItem(files, i, LVNI_SELECTED)) != -1;)
                if (i < static_cast<int>(rows.size())) selected.push_back(rows[i].path);
            int index = ListView_GetNextItem(files, -1, LVNI_FOCUSED);
            if (index >= 0 && index < static_cast<int>(rows.size())) focused = rows[index].path;
            index = ListView_GetTopIndex(files);
            if (index >= 0 && index < static_cast<int>(rows.size())) topPath = rows[index].path;
        }
        ListView_SetItemState(files, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
        rows = std::move(event->rows); ListView_SetItemCountEx(files, static_cast<int>(rows.size()), LVSICF_NOSCROLL); InvalidateRect(files, nullptr, TRUE);
        listedDirectory = directory;
        for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
            UINT flags = std::find(selected.begin(), selected.end(), rows[i].path) != selected.end() ? LVIS_SELECTED : 0;
            if (rows[i].path == focused) flags |= LVIS_FOCUSED;
            if (flags) ListView_SetItemState(files, i, flags, LVIS_SELECTED | LVIS_FOCUSED);
            if (rows[i].path == topPath) {
                RECT item{};
                if (ListView_GetItemRect(files, i, &item, LVIR_BOUNDS)) ListView_Scroll(files, 0, (i - ListView_GetTopIndex(files)) * (item.bottom - item.top));
            }
        }
        if (!event->error.empty()) setStatus(event->error);
        else if (!busy) setStatus(L"目录中 " + std::to_wstring(rows.size()) + L" 个匹配压缩包" + (first ? L"　|　" + std::to_wstring(state.page + 1) + L" / " + std::to_wstring(entries.size()) + L" 页" : L""));
        requestTagContext();
    }
    void fullScreen() {
        sidebar.closeDirectory();
        pressing = dragging = false; if (GetCapture() == view) ReleaseCapture();
        full = !full;
        if (full) {
            fullscreenFocus = GetFocus();
            GetWindowPlacement(window, &placement); MONITORINFO info{sizeof(info)}; GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &info);
            SetMenu(window, nullptr); SetWindowLongPtrW(window, GWL_STYLE, GetWindowLongPtrW(window, GWL_STYLE) & ~WS_OVERLAPPEDWINDOW);
            SetWindowPos(window, HWND_TOP, info.rcMonitor.left, info.rcMonitor.top, info.rcMonitor.right - info.rcMonitor.left, info.rcMonitor.bottom - info.rcMonitor.top, SWP_FRAMECHANGED);
        } else {
            SetWindowLongPtrW(window, GWL_STYLE, GetWindowLongPtrW(window, GWL_STYLE) | WS_OVERLAPPEDWINDOW); SetMenu(window, mainMenu); SetWindowPlacement(window, &placement); SetWindowPos(window, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
        }
        layout();
        SetFocus(!full && IsWindow(fullscreenFocus) && IsWindowVisible(fullscreenFocus) ? fullscreenFocus : view);
    }
    void passwordDialog();
    void command(int id) {
        if (id == Exit) { SendMessageW(window, WM_CLOSE, 0, 0); return; }
        if (id == Fullscreen) { fullScreen(); return; }
        if (id == About) { MessageBoxW(window, L"ComicViewerEx\n\n单击列表打开漫画，上下键连续切换；Ctrl/Shift 多选用于打标。\n点击画布左半边上一页、右半边下一页；左键拖动平移。\n←/→、PgUp/PgDn 或滚轮翻页；Ctrl+滚轮缩放。\n鼠标中键或 F11 切换全屏，Esc 退出全屏或取消读取。\n1/2/3 切换单页、双页、宽图分页；F/W/H/0 调整适应方式。\n阅读模式、方向和适应方式全局保存，进度和缩放倍数按书保存。\n\n拖动侧栏标题至窗口左右边缘切换停靠，拖动分隔线调整宽度。\n点击路径下拉目录树，选择目录后收起。\n筛选页签勾选立即生效；打标页签勾选后添加或移除，可同时关联多个标签。底部气泡显示选中文件标签，点击切换筛选。\n密码以明文保存在程序旁 data/library.db。\n固实压缩包使用上限 512 MiB 临时缓存。\n\n使用 7-Zip、SQLite 和 libwebp，许可见随附 licenses 目录。", L"使用说明", MB_OK); return; }
        if (!store) return;
        if (id == Open || id == ReadFolder || id == Library) { auto path = selectPath(window, id != Open); if (!path.empty()) { if (id == Library) { directory = path; scan(); } else openPath(path); } }
        else if (id == DirectoryDropdown) sidebar.toggleDirectory();
        else if (id == Previous) turn(-1);
        else if (id == Next) turn(1);
        else if ((id >= Single && id <= Direction) || (id >= FitPage && id <= Actual)) {
            auto changed = preferences;
            if (id >= Single && id <= Split) changed.mode = id - Single;
            else if (id == Direction) changed.rtl = !changed.rtl;
            else changed.fit = id - FitPage;
            store->saveReadingPreferences(changed); preferences = changed; preferences.apply(state); updateReadingControls();
            if (id >= Single && id <= Split) {
                state.half = 0; second.reset(); bitmap2.Reset(); bitmapsDirty = true;
                if (opening) submitOpen(); else refreshPage();
            } else if (id >= FitPage && id <= Actual) { state.zoom = 1; panX = panY = 0; }
            saveReading(); InvalidateRect(view, nullptr, FALSE);
        }
        else if (id == ZoomIn || id == ZoomOut) { state.zoom = std::clamp(state.zoom * (id == ZoomIn ? 1.2 : 1 / 1.2), .05, 16.0); saveReading(); InvalidateRect(view, nullptr, FALSE); }
        else if (id == Jump && !entries.empty()) { Input input{L"跳转", L"页码（1–" + std::to_wstring(entries.size()) + L"）", std::to_wstring(state.page + 1)}; if (prompt(window, input)) { size_t end{}; int page{}; try { page = std::stoi(input.value, &end); } catch (...) { throw std::runtime_error("请输入有效页码"); } if (end != input.value.size() || page < 1 || page > static_cast<int>(entries.size())) throw std::runtime_error("页码超出范围"); requestedPage = page - 1; state.half = 0; refreshPage(); } }
        else if (id == CancelRead) { readerGeneration = reader->cancel(); busy = opening = false; pendingPassword.reset(); requestedPage = state.page; if (full) fullScreen(); setStatus(L"已取消读取"); }
        else if (id == Refresh || id == Recursive || id == MatchMode) scan();
        else if (id == Passwords) passwordDialog();
        else if (id == TagClear) { filterTags.clear(); syncFilterChecks(); scan(); }
        else if (id == TagFilter) { filterTags = selectedTags(true); requestTagContext(); scan(); }
        else if (id == TagCreate) { Input input{L"新建标签", L"标签名称", L""}; if (prompt(window, input)) { store->createTag(input.value); refreshTags(); } }
        else if (id == TagRename || id == TagDelete) {
            HWND list = sidebar.tagging() ? tags : sidebar.filterTags;
            int index = ListView_GetNextItem(list, -1, LVNI_FOCUSED);
            if (index < 0 || index >= static_cast<int>(tagItems.size())) throw std::runtime_error("请先选中一个要管理的标签");
            auto tag = tagItems[index];
            if (id == TagRename) { Input input{L"重命名标签", L"新的标签名称", tag.name}; if (prompt(window, input)) store->renameTag(tag.id, input.value); }
            else if (MessageBoxW(window, (L"删除标签“" + tag.name + L"”及关联？漫画文件不会改变。").c_str(), L"删除标签", MB_YESNO | MB_ICONQUESTION) == IDYES) store->deleteTag(tag.id);
            refreshTags(); scan();
        } else if (id == TagAdd || id == TagRemove) {
            auto selected = selectedTags(); if (selected.empty()) throw std::runtime_error("请在“打标”页签勾选一个或多个待操作标签");
            std::vector<std::string> books; for (int i = -1; (i = ListView_GetNextItem(files, i, LVNI_SELECTED)) != -1;) if (i < static_cast<int>(rows.size())) books.push_back(store->ensureBook(rows[i].path));
            if (books.empty()) throw std::runtime_error("请先选择漫画文件（Ctrl/Shift 可多选）");
            store->assignTags(books, selected, id == TagAdd); requestTagContext(); scan();
        }
    }
    void render() {
        PAINTSTRUCT paint{}; BeginPaint(view, &paint);
        RECT rect{}; GetClientRect(view, &rect);
        if (!first) {
            HBRUSH background = CreateSolidBrush(RGB(37,40,45)); FillRect(paint.hdc, &rect, background); DeleteObject(background);
            SetBkMode(paint.hdc, TRANSPARENT); SetTextColor(paint.hdc, RGB(185,190,198)); HGDIOBJ oldFont = SelectObject(paint.hdc, font);
            const std::wstring welcome = busy ? L"正在读取漫画…" : L"打开压缩包或图片目录，开始阅读　·　Ctrl+O 打开　·　Ctrl+L 浏览目录";
            DrawTextW(paint.hdc, welcome.c_str(), -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            SelectObject(paint.hdc, oldFont); EndPaint(view, &paint); return;
        }
        if (!factory || !rect.right || !rect.bottom) { EndPaint(view, &paint); return; }
        if (!target) {
            auto props = D2D1::RenderTargetProperties(); props.dpiX = props.dpiY = 96;
            factory->CreateHwndRenderTarget(props, D2D1::HwndRenderTargetProperties(view, D2D1::SizeU(rect.right, rect.bottom)), &target); bitmapsDirty = true;
        }
        if (!target) { EndPaint(view, &paint); return; }
        if (bitmapsDirty) {
            bitmap1.Reset(); bitmap2.Reset();
            auto create = [&](const auto& image, ComPtr<ID2D1Bitmap>& bitmap) { if (image) target->CreateBitmap(D2D1::SizeU(image->width, image->height), image->bgra.data(), image->width * 4, D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)), &bitmap); };
            create(first, bitmap1); create(second, bitmap2); bitmapsDirty = false;
        }
        target->BeginDraw(); target->Clear(D2D1::ColorF(0x25282d));
        float vw = static_cast<float>(rect.right), vh = static_cast<float>(rect.bottom);
        if (bitmap1 && first) {
            bool split = state.mode == 2 && first->width > first->height;
            float w1 = static_cast<float>(first->originalWidth) / (split ? 2.f : 1.f), h1 = static_cast<float>(first->originalHeight);
            float w2 = second ? static_cast<float>(second->originalWidth) : 0, h2 = second ? static_cast<float>(second->originalHeight) : 0;
            float totalW = w1 + w2, totalH = std::max(h1, h2);
            double fit = state.fit == 0 ? std::min(vw / totalW, vh / totalH) : state.fit == 1 ? vw / totalW : state.fit == 2 ? vh / totalH : 1.0;
            float scale = static_cast<float>(fit * state.zoom);
            double maxX = std::max(0.f, (totalW * scale - vw) / 2), maxY = std::max(0.f, (totalH * scale - vh) / 2);
            panX = std::clamp(panX, -maxX, maxX); panY = std::clamp(panY, -maxY, maxY);
            float left = (vw - totalW * scale) / 2 + static_cast<float>(panX), top = (vh - totalH * scale) / 2 + static_cast<float>(panY);
            float offset = split && ((state.half == 0) == state.rtl) ? w1 : 0;
            auto draw = [&](ID2D1Bitmap* bitmap, float x, float width, float height, float crop) {
                auto dest = D2D1::RectF(x, top + (totalH - height) * scale / 2, x + width * scale, top + (totalH + height) * scale / 2);
                auto bitmapSize = bitmap->GetSize(); float fraction = bitmap == bitmap1.Get() && split ? .5f : 1.f;
                float sourceOffset = crop > 0 ? bitmapSize.width * .5f : 0.f;
                auto src = D2D1::RectF(sourceOffset, 0, sourceOffset + bitmapSize.width * fraction, bitmapSize.height);
                target->DrawBitmap(bitmap, &dest, 1, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, &src);
            };
            if (second && state.rtl) { draw(bitmap2.Get(), left, w2, h2, 0); draw(bitmap1.Get(), left + w2 * scale, w1, h1, offset); }
            else { draw(bitmap1.Get(), left, w1, h1, offset); if (bitmap2) draw(bitmap2.Get(), left + w1 * scale, w2, h2, 0); }
        } else if (textFormat) {
            ComPtr<ID2D1SolidColorBrush> brush; target->CreateSolidColorBrush(D2D1::ColorF(0xb9bec6), &brush);
            textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER); textFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            const std::wstring text = busy ? L"正在读取漫画…" : L"ComicViewerEx\n\n打开压缩包或图片目录，开始阅读\nCtrl+O 打开  ·  Ctrl+L 浏览目录";
            if (brush) target->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), textFormat.Get(), D2D1::RectF(20, 20, vw - 20, vh - 20), brush.Get());
        }
        if (target->EndDraw() == D2DERR_RECREATE_TARGET) { bitmap1.Reset(); bitmap2.Reset(); target.Reset(); }
        EndPaint(view, &paint);
    }
    void shutdown() {
        shuttingDown = true;
        try { saveReading(); } catch (const std::exception& e) { showError(window, wide(e.what())); }
        catalog.reset(); reader.reset();
        MSG message{};
        while (PeekMessageW(&message, window, WM_READER, WM_CATALOG, PM_REMOVE)) { if (message.message == WM_READER) delete reinterpret_cast<ReaderEvent*>(message.lParam); else delete reinterpret_cast<CatalogEvent*>(message.lParam); }
        store.reset(); first.reset(); second.reset(); bitmap1.Reset(); bitmap2.Reset(); target.Reset();
        sidebar.destroy();
        tagBar.destroy();
        if (font) DeleteObject(font);
        SetMenu(window, nullptr); if (mainMenu) DestroyMenu(mainMenu);
    }
};

struct PasswordDialog { Application* app; std::vector<Password> rows; };
void fillPasswords(HWND window, PasswordDialog& data) {
    data.rows = data.app->store->passwords(); SendDlgItemMessageW(window, IDC_PASSWORD_LIST, LB_RESETCONTENT, 0, 0);
    for (auto& row : data.rows) SendDlgItemMessageW(window, IDC_PASSWORD_LIST, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(row.value.c_str()));
}
INT_PTR CALLBACK passwordsProc(HWND window, UINT message, WPARAM w, LPARAM l) {
    auto data = reinterpret_cast<PasswordDialog*>(GetWindowLongPtrW(window, DWLP_USER));
    try {
        if (message == WM_INITDIALOG) { data = reinterpret_cast<PasswordDialog*>(l); SetWindowLongPtrW(window, DWLP_USER, l); fillPasswords(window, *data); return TRUE; }
        if (message == WM_COMMAND) {
            int id = LOWORD(w);
            if (id == IDOK || id == IDCANCEL) { EndDialog(window, id); return TRUE; }
            if (id == IDC_PASSWORD_EDIT || id == IDC_PASSWORD_DELETE) {
                auto selected = SendDlgItemMessageW(window, IDC_PASSWORD_LIST, LB_GETCURSEL, 0, 0); if (selected < 0 || selected >= static_cast<LRESULT>(data->rows.size())) return TRUE;
                auto row = data->rows[selected];
                if (id == IDC_PASSWORD_DELETE) { data->app->store->deletePassword(row.id); }
                else {
                    if (data->app->bookPath.empty() || data->app->busy) throw std::runtime_error("请先打开用于验证的加密漫画，再修改密码");
                    Input input{L"修改密码", L"将使用当前漫画验证，成功后才保存", row.value};
                    if (prompt(window, input)) {
                        auto app = data->app; app->saveReading(); app->opening = app->busy = true;
                        app->pendingPassword = input.value; app->pendingRemember = false; app->pendingPasswordReplacement = row.id; app->submitOpen();
                        app->setStatus(L"正在验证密码修改…"); EndDialog(window, IDOK); return TRUE;
                    }
                }
                fillPasswords(window, *data); return TRUE;
            }
        }
    } catch (const std::exception& e) { showError(window, wide(e.what())); }
    return FALSE;
}
void Application::passwordDialog() { PasswordDialog data{this}; DialogBoxParamW(instance, MAKEINTRESOURCEW(IDD_PASSWORDS), window, passwordsProc, reinterpret_cast<LPARAM>(&data)); }

LRESULT CALLBACK filesProc(HWND window, UINT message, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR data) {
    auto app = reinterpret_cast<Application*>(data);
    if (message == WM_NCDESTROY) RemoveWindowSubclass(window, filesProc, id);
    if (message == WM_KEYDOWN && (w == VK_UP || w == VK_DOWN)) {
        const bool plain = GetKeyState(VK_CONTROL) >= 0 && GetKeyState(VK_SHIFT) >= 0;
        // 先让原生列表更新选择，再打开新焦点项；不监听程序产生的选择变更。
        auto result = DefSubclassProc(window, message, w, l);
        if (plain) {
            try { app->openListItem(ListView_GetNextItem(window, -1, LVNI_FOCUSED)); }
            catch (const std::exception& error) { showError(app->window, wide(error.what())); }
        }
        return result;
    }
    return DefSubclassProc(window, message, w, l);
}

LRESULT CALLBACK canvasProc(HWND window, UINT message, WPARAM w, LPARAM l) {
    auto app = reinterpret_cast<Application*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) { app = static_cast<Application*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams); SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app)); }
    if (!app) return DefWindowProcW(window, message, w, l);
    try {
        switch (message) {
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: app->render(); return 0;
        case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK:
            SetFocus(window); SetCapture(window); app->pressing = true; app->dragging = false;
            app->pressPoint = app->dragPoint = {GET_X_LPARAM(l), GET_Y_LPARAM(l)}; return 0;
        case WM_MOUSEMOVE:
            if (app->pressing && GetCapture() == window) {
                POINT point{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
                if (!app->dragging && (std::abs(point.x - app->pressPoint.x) > GetSystemMetricsForDpi(SM_CXDRAG, GetDpiForWindow(window)) ||
                    std::abs(point.y - app->pressPoint.y) > GetSystemMetricsForDpi(SM_CYDRAG, GetDpiForWindow(window)))) app->dragging = true;
                if (app->dragging) { app->panX += point.x - app->dragPoint.x; app->panY += point.y - app->dragPoint.y; app->dragPoint = point; InvalidateRect(window, nullptr, FALSE); }
            }
            return 0;
        case WM_CAPTURECHANGED: case WM_CANCELMODE:
            app->pressing = app->dragging = false; if (GetCapture() == window) ReleaseCapture(); return 0;
        case WM_LBUTTONUP: {
            bool click = app->pressing && !app->dragging && GetCapture() == window;
            app->pressing = app->dragging = false; if (GetCapture() == window) ReleaseCapture();
            RECT area{}; GetClientRect(window, &area); POINT point{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            if (click && PtInRect(&area, point)) app->turn(point.x < area.right / 2 ? -1 : 1);
            return 0;
        }
        case WM_MBUTTONUP: SetFocus(window); app->fullScreen(); return 0;
        case WM_MOUSEWHEEL: if (GET_KEYSTATE_WPARAM(w) & MK_CONTROL) app->command(GET_WHEEL_DELTA_WPARAM(w) > 0 ? ZoomIn : ZoomOut); else app->turn(GET_WHEEL_DELTA_WPARAM(w) > 0 ? -1 : 1); return 0;
        }
    } catch (const std::exception& e) { showError(app->window, wide(e.what())); }
    return DefWindowProcW(window, message, w, l);
}
LRESULT CALLBACK mainProc(HWND window, UINT message, WPARAM w, LPARAM l) {
    auto app = reinterpret_cast<Application*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) { app = static_cast<Application*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams); app->window = window; SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app)); }
    if (!app) return DefWindowProcW(window, message, w, l);
    try {
        switch (message) {
        case WM_CREATE: app->create(); PostMessageW(window, WM_INITIALIZE, 0, 0); return 0;
        case WM_INITIALIZE: app->initialize(); return 0;
        case WM_SIZE: app->layout(); return 0;
        case WM_DPICHANGED: { auto r = reinterpret_cast<RECT*>(l); app->refreshFonts(); SetWindowPos(window, nullptr, r->left, r->top, r->right-r->left, r->bottom-r->top, SWP_NOZORDER); app->layout(); return 0; }
        case WM_GETMINMAXINFO: reinterpret_cast<MINMAXINFO*>(l)->ptMinTrackSize = {app->px(850), app->px(650)}; return 0;
        case WM_SIDEBAR_LAYOUT: app->layout(); if (w) app->saveSidebar(); return 0;
        case WM_DIRECTORY_SELECTED: app->directory = app->sidebar.directory(); app->scan(); return 0;
        case WM_TAG_CONTEXT: app->refreshTagContext(); return 0;
        case WM_TAG_FILTER_TOGGLE: app->toggleTagFilter(static_cast<int64_t>(w)); return 0;
        case WM_COMMAND: {
            if (LOWORD(w) == MatchMode && HIWORD(w) != CBN_SELCHANGE) return 0;
            app->command(LOWORD(w)); return 0;
        }
        case WM_NOTIFY: {
            auto header = reinterpret_cast<NMHDR*>(l);
            if (header->hwndFrom == app->files) {
                if (header->code == LVN_GETDISPINFOW) {
                    auto info = reinterpret_cast<NMLVDISPINFOW*>(l); int i = info->item.iItem;
                    if (i >= 0 && i < static_cast<int>(app->rows.size()) && (info->item.mask & LVIF_TEXT)) {
                        auto& row = app->rows[i]; std::wstring text = info->item.iSubItem == 0 ? row.path.filename().wstring() : info->item.iSubItem == 1 ? row.tags : std::to_wstring(row.size / 1024 / 1024) + L" MiB";
                        wcsncpy_s(info->item.pszText, info->item.cchTextMax, text.c_str(), _TRUNCATE);
                    }
                } else if (header->code == NM_CLICK) app->openListItem(reinterpret_cast<NMITEMACTIVATE*>(l)->iItem);
                else if (header->code == LVN_KEYDOWN && reinterpret_cast<NMLVKEYDOWN*>(l)->wVKey == VK_RETURN) app->openListItem(ListView_GetNextItem(app->files, -1, LVNI_FOCUSED));
                else if (header->code == LVN_ITEMCHANGED || header->code == LVN_ODSTATECHANGED) app->requestTagContext();
            } else if (header->hwndFrom == app->sidebar.filterTags && header->code == LVN_ITEMCHANGED && !app->applyingTags) {
                auto change = reinterpret_cast<NMLISTVIEW*>(l);
                if ((change->uChanged & LVIF_STATE) && ((change->uOldState ^ change->uNewState) & LVIS_STATEIMAGEMASK)) app->command(TagFilter);
            }
            return 0;
        }
        case WM_DROPFILES: { HDROP drop = reinterpret_cast<HDROP>(w); UINT length = DragQueryFileW(drop, 0, nullptr, 0); std::wstring path(length+1, L'\0'); DragQueryFileW(drop, 0, path.data(), length+1); DragFinish(drop); path.resize(length); app->openPath(path); return 0; }
        case WM_READER: app->acceptReader(std::unique_ptr<ReaderEvent>(reinterpret_cast<ReaderEvent*>(l))); return 0;
        case WM_CATALOG: app->acceptCatalog(std::unique_ptr<CatalogEvent>(reinterpret_cast<CatalogEvent*>(l))); return 0;
        case WM_CONTEXTMENU: {
            HWND list = reinterpret_cast<HWND>(w);
            if (list != app->tags && list != app->sidebar.filterTags) break;
            POINT point{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            if (point.x != -1 || point.y != -1) {
                LVHITTESTINFO hit{}; hit.pt = point; ScreenToClient(list, &hit.pt);
                int index = ListView_HitTest(list, &hit); if (index < 0) return 0;
                ListView_SetItemState(list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
                ListView_SetItemState(list, index, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            } else {
                RECT area{}; GetWindowRect(list, &area); point = {area.left + app->px(20), area.top + app->px(20)};
            }
            HMENU menu = CreatePopupMenu(); AppendMenuW(menu, MF_STRING, TagRename, L"重命名标签"); AppendMenuW(menu, MF_STRING, TagDelete, L"删除标签");
            int id = TrackPopupMenu(menu, TPM_RETURNCMD, point.x, point.y, 0, window, nullptr); DestroyMenu(menu);
            if (id) app->command(id); return 0;
        }
        case WM_CLOSE: app->shutdown(); DestroyWindow(window); return 0;
        case WM_DESTROY: PostQuitMessage(0); return 0;
        }
    } catch (const std::exception& e) { app->setStatus(wide(e.what())); showError(window, wide(e.what())); }
    return DefWindowProcW(window, message, w, l);
}
}

int WINAPI wWinMain(HINSTANCE module, HINSTANCE, PWSTR, int show) {
    instance = module;
    // One instance per portable directory; prevents concurrent cache cleanup/database mutation.
    const std::wstring mutexName = L"Local\\ComicViewerEx-" + std::to_wstring(std::hash<std::wstring>{}(lower(executableDir().wstring())));
    HANDLE mutex = CreateMutexW(nullptr, TRUE, mutexName.c_str());
    if (GetLastError() == ERROR_ALREADY_EXISTS) { showError(nullptr, L"此便携目录中的阅读器已在运行。"); if (mutex) CloseHandle(mutex); return 0; }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_WIN95_CLASSES | ICC_STANDARD_CLASSES}; InitCommonControlsEx(&controls);
    WNDCLASSEXW wc{sizeof(wc)}; wc.lpfnWndProc = mainProc; wc.hInstance = instance; wc.hCursor = LoadCursorW(nullptr, IDC_ARROW); wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION); wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE+1); wc.lpszClassName = L"ComicViewerEx.Main"; RegisterClassExW(&wc);
    wc.lpfnWndProc = canvasProc; wc.lpszClassName = L"ComicViewerEx.Canvas"; wc.style = CS_DBLCLKS; wc.hbrBackground = nullptr; RegisterClassExW(&wc);
    Application app;
    int argc{}; auto argv = CommandLineToArgvW(GetCommandLineW(), &argc); if (argc > 1) app.commandLine = argv[1]; LocalFree(argv);
    HWND window = CreateWindowExW(0, L"ComicViewerEx.Main", L"ComicViewerEx", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, 1280, 860, nullptr, nullptr, instance, &app);
    if (!window) { CoUninitialize(); CloseHandle(mutex); return 1; }
    ShowWindow(window, show); UpdateWindow(window);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (app.sidebar.preTranslate(message)) continue;
        if (message.message == WM_KEYDOWN && (message.hwnd == window || IsChild(window, message.hwnd))) {
            bool ctrl = GetKeyState(VK_CONTROL) < 0; int id = 0;
            if (ctrl) { if (message.wParam == 'O') id = Open; else if (message.wParam == 'D') id = ReadFolder; else if (message.wParam == 'L') id = Library; else if (message.wParam == 'G') id = Jump; }
            else if (message.wParam == VK_F11) id = Fullscreen;
            else if (message.wParam == VK_ESCAPE) id = CancelRead;
            else if (message.wParam == VK_F5) id = Refresh;
            else if (message.hwnd == app.view || message.hwnd == window) {
                switch (message.wParam) {
                case VK_RIGHT: case VK_NEXT: case VK_SPACE: id = Next; break;
                case VK_LEFT: case VK_PRIOR: id = Previous; break;
                case '1': id = Single; break; case '2': id = Dual; break; case '3': id = Split; break;
                case 'F': id = FitPage; break; case 'W': id = FitWidth; break; case 'H': id = FitHeight; break; case '0': id = Actual; break;
                case VK_ADD: case VK_OEM_PLUS: id = ZoomIn; break; case VK_SUBTRACT: case VK_OEM_MINUS: id = ZoomOut; break;
                }
            }
            if (id) { SendMessageW(window, WM_COMMAND, id, 0); continue; }
        }
        TranslateMessage(&message); DispatchMessageW(&message);
    }
    CoUninitialize(); if (mutex) { ReleaseMutex(mutex); CloseHandle(mutex); } return static_cast<int>(message.wParam);
}
