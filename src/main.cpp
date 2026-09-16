#include "reader.hpp"
#include "resource.h"
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
enum Command {
    Open = 200, ReadFolder, Library, Previous, Next, Single, Dual, Split, Direction,
    FitPage, FitWidth, FitHeight, Actual, ZoomIn, ZoomOut, Fullscreen, Jump,
    Passwords, CancelRead, Refresh, TagCreate, TagRename, TagDelete, TagAdd, TagRemove, TagClear,
    Exit, About, Recursive, MatchMode, TagsList, FilesList, DirectoryTree, TagFilter
};
HINSTANCE instance;

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
    HWND window{}, view{}, tree{}, files{}, tags{}, recursive{}, match{}, pathLabel{}, status{}, toolbar{};
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
    std::unordered_map<HTREEITEM, fs::path> treePaths;
    std::shared_ptr<Pixels> first, second;
    ReadingState state;
    uint64_t readerGeneration{}, catalogGeneration{};
    bool opening{}, busy{}, full{}, applyingTags{}, shuttingDown{}, treeChanging{};
    int requestedPage{};
    double panX{}, panY{};
    POINT dragPoint{};
    bool dragging{};
    WINDOWPLACEMENT placement{sizeof(WINDOWPLACEMENT)};
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
        item(read, ZoomIn, L"放大\t+ / Ctrl+滚轮"); item(read, ZoomOut, L"缩小\t-"); item(read, Fullscreen, L"全屏\tF11 / 双击");
        item(tag, TagCreate, L"新建标签…"); item(tag, TagRename, L"重命名选中标签…"); item(tag, TagDelete, L"删除选中标签");
        AppendMenuW(tag, MF_SEPARATOR, 0, nullptr); item(tag, TagAdd, L"为选中文件添加所选标签"); item(tag, TagRemove, L"从选中文件移除所选标签"); item(tag, TagClear, L"清除标签筛选");
        item(help, About, L"使用说明与关于");
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(file), L"文件"); AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(read), L"阅读"); AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(tag), L"标签"); AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(help), L"帮助"); return menu;
    }
    void create() {
        font = CreateFontW(-MulDiv(10, GetDpiForWindow(window), 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");
        mainMenu = makeMenu(); SetMenu(window, mainMenu);
        const std::pair<int, const wchar_t*> labels[] = {{Open,L"打开"},{Library,L"目录"},{Previous,L"上一页"},{Next,L"下一页"},{Single,L"单页"},{Dual,L"双页"},{Split,L"分页"},{FitPage,L"适合页面"},{Fullscreen,L"全屏"},{CancelRead,L"取消"}};
        for (auto& [id, text] : labels) buttons.push_back(control(L"BUTTON", text, BS_PUSHBUTTON, id));
        pathLabel = control(L"STATIC", L"选择目录以浏览漫画", SS_PATHELLIPSIS, 0);
        tree = control(WC_TREEVIEWW, L"", TVS_HASBUTTONS | TVS_HASLINES | TVS_LINESATROOT | TVS_SHOWSELALWAYS, DirectoryTree, WS_EX_CLIENTEDGE);
        recursive = control(L"BUTTON", L"包含子目录", BS_AUTOCHECKBOX, Recursive);
        match = control(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST, MatchMode);
        SendMessageW(match, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"全部标签匹配")); SendMessageW(match, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"任一标签匹配")); SendMessageW(match, CB_SETCURSEL, 0, 0);
        tags = control(WC_LISTBOXW, L"", LBS_EXTENDEDSEL | LBS_NOTIFY | WS_VSCROLL, TagsList, WS_EX_CLIENTEDGE);
        control(L"BUTTON", L"新建标签", BS_PUSHBUTTON, TagCreate); control(L"BUTTON", L"清除筛选", BS_PUSHBUTTON, TagClear); control(L"BUTTON", L"应用筛选", BS_PUSHBUTTON, TagFilter);
        files = control(WC_LISTVIEWW, L"", LVS_REPORT | LVS_OWNERDATA | LVS_SHOWSELALWAYS, FilesList, WS_EX_CLIENTEDGE);
        ListView_SetExtendedListViewStyle(files, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        const wchar_t* columns[] = {L"文件名", L"标签", L"大小"}; int widths[] = {190, 100, 76};
        for (int i = 0; i < 3; ++i) { LVCOLUMNW column{LVCF_TEXT | LVCF_WIDTH}; column.pszText = const_cast<wchar_t*>(columns[i]); column.cx = widths[i]; ListView_InsertColumn(files, i, &column); }
        control(L"BUTTON", L"添加标签", BS_PUSHBUTTON, TagAdd); control(L"BUTTON", L"移除标签", BS_PUSHBUTTON, TagRemove);
        view = CreateWindowExW(0, L"ComicViewerEx.Canvas", nullptr, WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 0, 0, 100, 100, window, nullptr, instance, this);
        status = CreateWindowExW(0, STATUSCLASSNAMEW, L"就绪", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window, nullptr, instance, nullptr);
        SendMessageW(status, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory.GetAddressOf());
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(writeFactory.GetAddressOf()));
        if (writeFactory) writeFactory->CreateTextFormat(L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, static_cast<float>(px(18)), L"zh-CN", &textFormat);
        DragAcceptFiles(window, TRUE); layout();
    }
    int px(int n) { return MulDiv(n, GetDpiForWindow(window), 96); }
    void layout() {
        RECT rect{}; GetClientRect(window, &rect); int width = rect.right, height = rect.bottom;
        SendMessageW(status, WM_SIZE, 0, 0);
        const int top = full ? 0 : px(42), bottom = full ? 0 : px(24), side = full ? 0 : std::min(px(380), width / 2);
        int x = px(8);
        for (size_t i = 0; i < buttons.size(); ++i) { ShowWindow(buttons[i], full ? SW_HIDE : SW_SHOW); int w = px(i == 7 ? 86 : 65); MoveWindow(buttons[i], x, px(7), w, px(28), TRUE); x += w + px(5); }
        for (HWND child : {tree, files, tags, recursive, match, pathLabel, GetDlgItem(window, TagCreate), GetDlgItem(window, TagClear), GetDlgItem(window, TagFilter), GetDlgItem(window, TagAdd), GetDlgItem(window, TagRemove), status}) ShowWindow(child, full ? SW_HIDE : SW_SHOW);
        if (!full) {
            int inner = side - px(16), y = top + px(7), treeHeight = std::clamp((height - px(330)) / 3, px(75), px(200));
            MoveWindow(pathLabel, px(8), y, inner, px(23), TRUE); y += px(26);
            MoveWindow(tree, px(8), y, inner, treeHeight, TRUE); y += treeHeight + px(7);
            MoveWindow(recursive, px(8), y, px(140), px(25), TRUE); MoveWindow(match, px(155), y, std::max(px(100), inner - px(147)), px(160), TRUE); y += px(30);
            MoveWindow(tags, px(8), y, inner, px(74), TRUE); y += px(79);
            MoveWindow(GetDlgItem(window, TagCreate), px(8), y, px(100), px(26), TRUE); MoveWindow(GetDlgItem(window, TagClear), px(116), y, px(100), px(26), TRUE); MoveWindow(GetDlgItem(window, TagFilter), px(224), y, px(100), px(26), TRUE); y += px(33);
            int listHeight = std::max(px(60), height - bottom - y - px(40));
            MoveWindow(files, px(8), y, inner, listHeight, TRUE); y += listHeight + px(5);
            ListView_SetColumnWidth(files, 0, std::max(px(110), inner - px(180))); ListView_SetColumnWidth(files, 1, px(100)); ListView_SetColumnWidth(files, 2, px(72));
            MoveWindow(GetDlgItem(window, TagAdd), px(8), y, px(100), px(27), TRUE); MoveWindow(GetDlgItem(window, TagRemove), px(116), y, px(100), px(27), TRUE);
        }
        MoveWindow(view, side, top, std::max(1, width - side), std::max(1, height - top - bottom), TRUE);
        if (target) { RECT area{}; GetClientRect(view, &area); target->Resize(D2D1::SizeU(area.right, area.bottom)); }
        InvalidateRect(view, nullptr, FALSE);
    }
    void initialize() {
        store = std::make_unique<Store>(executableDir() / L"data" / L"library.db");
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
        refreshTags(); buildTree();
        directory = store->setting("directory");
        if (!directory.empty() && fs::is_directory(directory, ec)) scan();
        if (!commandLine.empty()) openPath(commandLine);
        else { const auto last = store->setting("lastBook"); if (!last.empty() && fs::exists(fs::path(last), ec)) openPath(last); }
    }
    void refreshTags() {
        if (!store) return;
        auto selected = selectedTags(); applyingTags = true; tagItems = store->tags(); SendMessageW(tags, LB_RESETCONTENT, 0, 0);
        for (size_t i = 0; i < tagItems.size(); ++i) { SendMessageW(tags, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(tagItems[i].name.c_str())); if (std::find(selected.begin(), selected.end(), tagItems[i].id) != selected.end()) SendMessageW(tags, LB_SETSEL, TRUE, i); }
        applyingTags = false;
    }
    std::vector<int64_t> selectedTags() {
        std::vector<int64_t> result; for (size_t i = 0; i < tagItems.size(); ++i) if (SendMessageW(tags, LB_GETSEL, i, 0) > 0) result.push_back(tagItems[i].id); return result;
    }
    void scan() {
        if (!catalog || directory.empty()) return;
        SetWindowTextW(pathLabel, directory.c_str()); setStatus(L"正在扫描目录…");
        catalogGeneration = catalog->scan(directory, Button_GetCheck(recursive) == BST_CHECKED, filterTags, SendMessageW(match, CB_GETCURSEL, 0, 0) == 0);
        store->setSetting("directory", directory.wstring());
    }
    HTREEITEM addTree(HTREEITEM parent, const std::wstring& label, const fs::path& path) {
        TVINSERTSTRUCTW item{}; item.hParent = parent; item.hInsertAfter = TVI_SORT; item.item.mask = TVIF_TEXT | TVIF_CHILDREN; item.item.pszText = const_cast<wchar_t*>(label.c_str()); item.item.cChildren = 1;
        HTREEITEM handle = TreeView_InsertItem(tree, &item); treePaths[handle] = path; return handle;
    }
    void buildTree() {
        treeChanging = true; TreeView_DeleteAllItems(tree); treePaths.clear();
        wchar_t drives[512]{}; GetLogicalDriveStringsW(512, drives);
        for (wchar_t* drive = drives; *drive; drive += wcslen(drive) + 1) addTree(TVI_ROOT, drive, drive);
        treeChanging = false;
    }
    void expandTree(HTREEITEM node) {
        if (TreeView_GetChild(tree, node) || !treePaths.contains(node)) return;
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(treePaths[node], fs::directory_options::skip_permission_denied, ec)) if (entry.is_directory(ec) && !entry.is_symlink(ec)) addTree(node, entry.path().filename().wstring(), entry.path());
    }
    void saveReading() {
        if (store && !bookId.empty() && first && !opening) store->saveReading(bookId, state);
    }
    void openPath(fs::path path) {
        if (!reader) return;
        saveReading();
        std::error_code ec;
        std::wstring chosenImage;
        if (isImage(path)) { chosenImage = path.filename().wstring(); path = path.parent_path(); }
        if (!fs::exists(path, ec)) throw std::runtime_error("文件或目录不存在");
        if (!fs::is_directory(path, ec) && !isArchive(path)) throw std::runtime_error("不支持此文件格式");
        bookPath = fs::absolute(path).lexically_normal(); bookId = store->ensureBook(bookPath); state = store->reading(bookId);
        if (!chosenImage.empty()) state.entry = chosenImage;
        state.mode = std::clamp(state.mode, 0, 2); state.fit = std::clamp(state.fit, 0, 3); state.half = std::clamp(state.half, 0, 1); state.zoom = std::clamp(state.zoom, .05, 16.0);
        entries.clear(); first.reset(); second.reset(); bitmap1.Reset(); bitmap2.Reset(); opening = busy = true; requestedPage = state.page;
        readerGeneration = reader->open(bookPath, bookId, state); setStatus(L"正在打开漫画… 可按 Esc 取消");
        directory = fs::is_directory(path, ec) ? path : path.parent_path(); scan();
        SetWindowTextW(window, (bookPath.filename().wstring() + L" — ComicViewerEx").c_str()); InvalidateRect(view, nullptr, FALSE);
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
            if (prompt(window, input)) { opening = busy = true; readerGeneration = reader->open(bookPath, bookId, state, input.value, input.remember); setStatus(L"正在验证密码…"); }
            else { opening = false; setStatus(L"已取消打开"); }
            return;
        }
        if (event->kind == ReaderEvent::Error) { busy = opening = false; requestedPage = state.page; setStatus(event->message); showError(window, event->message); return; }
        if (event->kind == ReaderEvent::Opened) { entries = std::move(event->entries); opening = false; store->setSetting("lastBook", bookPath.wstring()); }
        state = event->state; requestedPage = state.page; first = std::move(event->first); second = std::move(event->second);
        busy = false; bitmapsDirty = true; panX = panY = 0; saveReading(); updateStatus(); InvalidateRect(view, nullptr, FALSE); SetFocus(view);
        CheckMenuRadioItem(GetMenu(window), Single, Split, Single + state.mode, MF_BYCOMMAND); CheckMenuItem(GetMenu(window), Direction, state.rtl ? MF_CHECKED : MF_UNCHECKED);
        CheckMenuRadioItem(GetMenu(window), FitPage, Actual, FitPage + state.fit, MF_BYCOMMAND);
    }
    void acceptCatalog(std::unique_ptr<CatalogEvent> event) {
        if (event->generation != catalogGeneration) return;
        rows = std::move(event->rows); ListView_SetItemCountEx(files, static_cast<int>(rows.size()), LVSICF_NOSCROLL); InvalidateRect(files, nullptr, TRUE);
        if (!event->error.empty()) setStatus(event->error);
        else if (!busy) setStatus(L"目录中 " + std::to_wstring(rows.size()) + L" 个匹配压缩包" + (first ? L"　|　" + std::to_wstring(state.page + 1) + L" / " + std::to_wstring(entries.size()) + L" 页" : L""));
    }
    void fullScreen() {
        full = !full;
        if (full) {
            GetWindowPlacement(window, &placement); MONITORINFO info{sizeof(info)}; GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &info);
            SetMenu(window, nullptr); SetWindowLongPtrW(window, GWL_STYLE, GetWindowLongPtrW(window, GWL_STYLE) & ~WS_OVERLAPPEDWINDOW);
            SetWindowPos(window, HWND_TOP, info.rcMonitor.left, info.rcMonitor.top, info.rcMonitor.right - info.rcMonitor.left, info.rcMonitor.bottom - info.rcMonitor.top, SWP_FRAMECHANGED);
        } else {
            SetWindowLongPtrW(window, GWL_STYLE, GetWindowLongPtrW(window, GWL_STYLE) | WS_OVERLAPPEDWINDOW); SetMenu(window, mainMenu); SetWindowPlacement(window, &placement); SetWindowPos(window, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
        }
        layout();
    }
    void passwordDialog();
    void command(int id) {
        if (id == Exit) { SendMessageW(window, WM_CLOSE, 0, 0); return; }
        if (id == Fullscreen) { fullScreen(); return; }
        if (id == About) { MessageBoxW(window, L"ComicViewerEx 1.0\n\n打开 ZIP/CBZ、RAR/CBR、7z 或图片目录。\n←/→、PgUp/PgDn 或滚轮翻页；Ctrl+滚轮缩放；鼠标拖动平移。\n1/2/3 切换单页、双页、宽图分页；F/W/H/0 调整适应方式。\nF11 或双击阅读区进入全屏，Esc 退出全屏或取消读取。\n\n左侧标签支持 Ctrl 多选；选中文件后点击“添加标签”。\n密码以明文保存在程序旁 data/library.db。\n固实压缩包使用上限 512 MiB 临时缓存。\n\n使用 7-Zip、SQLite 和 libwebp，许可见随附 licenses 目录。", L"使用说明", MB_OK); return; }
        if (!store) return;
        if (id == Open || id == ReadFolder || id == Library) { auto path = selectPath(window, id != Open); if (!path.empty()) { if (id == Library) { directory = path; scan(); } else openPath(path); } }
        else if (id == Previous) turn(-1);
        else if (id == Next) turn(1);
        else if (id >= Single && id <= Split) { state.mode = id - Single; state.half = 0; refreshPage(); }
        else if (id == Direction) { state.rtl = !state.rtl; saveReading(); CheckMenuItem(GetMenu(window), Direction, state.rtl ? MF_CHECKED : MF_UNCHECKED); InvalidateRect(view, nullptr, FALSE); }
        else if (id >= FitPage && id <= Actual) { state.fit = id - FitPage; state.zoom = 1; panX = panY = 0; saveReading(); CheckMenuRadioItem(GetMenu(window), FitPage, Actual, id, MF_BYCOMMAND); InvalidateRect(view, nullptr, FALSE); }
        else if (id == ZoomIn || id == ZoomOut) { state.zoom = std::clamp(state.zoom * (id == ZoomIn ? 1.2 : 1 / 1.2), .05, 16.0); saveReading(); InvalidateRect(view, nullptr, FALSE); }
        else if (id == Jump && !entries.empty()) { Input input{L"跳转", L"页码（1–" + std::to_wstring(entries.size()) + L"）", std::to_wstring(state.page + 1)}; if (prompt(window, input)) { size_t end{}; int page{}; try { page = std::stoi(input.value, &end); } catch (...) { throw std::runtime_error("请输入有效页码"); } if (end != input.value.size() || page < 1 || page > static_cast<int>(entries.size())) throw std::runtime_error("页码超出范围"); requestedPage = page - 1; state.half = 0; refreshPage(); } }
        else if (id == CancelRead) { readerGeneration = reader->cancel(); busy = opening = false; requestedPage = state.page; if (full) fullScreen(); setStatus(L"已取消读取"); }
        else if (id == Refresh || id == Recursive || id == MatchMode) scan();
        else if (id == Passwords) passwordDialog();
        else if (id == TagClear) { SendMessageW(tags, LB_SETSEL, FALSE, -1); filterTags.clear(); scan(); }
        else if (id == TagFilter) { filterTags = selectedTags(); scan(); }
        else if (id == TagCreate) { Input input{L"新建标签", L"标签名称", L""}; if (prompt(window, input)) { store->createTag(input.value); refreshTags(); } }
        else if (id == TagRename || id == TagDelete) {
            auto selected = selectedTags(); if (selected.size() != 1) throw std::runtime_error("请只选择一个标签");
            auto tag = *std::find_if(tagItems.begin(), tagItems.end(), [&](auto& t) { return t.id == selected[0]; });
            if (id == TagRename) { Input input{L"重命名标签", L"新的标签名称", tag.name}; if (prompt(window, input)) store->renameTag(tag.id, input.value); }
            else if (MessageBoxW(window, (L"删除标签“" + tag.name + L"”及关联？漫画文件不会改变。").c_str(), L"删除标签", MB_YESNO | MB_ICONQUESTION) == IDYES) store->deleteTag(tag.id);
            refreshTags(); std::erase_if(filterTags, [&](auto id) { return std::none_of(tagItems.begin(), tagItems.end(), [&](auto& tag) { return tag.id == id; }); }); scan();
        } else if (id == TagAdd || id == TagRemove) {
            auto selected = selectedTags(); if (selected.empty()) throw std::runtime_error("请先选择标签（Ctrl 可多选）");
            std::vector<std::string> books; for (int i = -1; (i = ListView_GetNextItem(files, i, LVNI_SELECTED)) != -1;) if (i < static_cast<int>(rows.size())) books.push_back(store->ensureBook(rows[i].path));
            if (books.empty()) throw std::runtime_error("请先选择漫画文件（Ctrl/Shift 可多选）");
            store->assignTags(books, selected, id == TagAdd); scan();
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
                        app->readerGeneration = app->reader->open(app->bookPath, app->bookId, app->state, input.value, false, row.id);
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

LRESULT CALLBACK canvasProc(HWND window, UINT message, WPARAM w, LPARAM l) {
    auto app = reinterpret_cast<Application*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) { app = static_cast<Application*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams); SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app)); }
    if (!app) return DefWindowProcW(window, message, w, l);
    try {
        switch (message) {
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: app->render(); return 0;
        case WM_LBUTTONDOWN: SetFocus(window); SetCapture(window); app->dragging = true; app->dragPoint = {GET_X_LPARAM(l), GET_Y_LPARAM(l)}; return 0;
        case WM_MOUSEMOVE: if (app->dragging) { POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)}; app->panX += p.x - app->dragPoint.x; app->panY += p.y - app->dragPoint.y; app->dragPoint = p; InvalidateRect(window, nullptr, FALSE); } return 0;
        case WM_CAPTURECHANGED: app->dragging = false; return 0;
        case WM_LBUTTONUP: app->dragging = false; ReleaseCapture(); return 0;
        case WM_LBUTTONDBLCLK: app->fullScreen(); return 0;
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
        case WM_DPICHANGED: { auto r = reinterpret_cast<RECT*>(l); SetWindowPos(window, nullptr, r->left, r->top, r->right-r->left, r->bottom-r->top, SWP_NOZORDER); app->layout(); return 0; }
        case WM_GETMINMAXINFO: reinterpret_cast<MINMAXINFO*>(l)->ptMinTrackSize = {850, 650}; return 0;
        case WM_COMMAND: {
            if (LOWORD(w) == TagsList && HIWORD(w) == LBN_SELCHANGE) return 0;
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
                } else if (header->code == LVN_ITEMACTIVATE) { int i = ListView_GetNextItem(app->files, -1, LVNI_SELECTED); if (i >= 0 && i < static_cast<int>(app->rows.size())) app->openPath(app->rows[i].path); }
                else if (header->code == LVN_KEYDOWN && reinterpret_cast<NMLVKEYDOWN*>(l)->wVKey == VK_RETURN) { int i = ListView_GetNextItem(app->files, -1, LVNI_SELECTED); if (i >= 0 && i < static_cast<int>(app->rows.size())) app->openPath(app->rows[i].path); }
            } else if (header->hwndFrom == app->tree && !app->treeChanging) {
                auto info = reinterpret_cast<NMTREEVIEWW*>(l);
                if (header->code == TVN_ITEMEXPANDINGW) app->expandTree(info->itemNew.hItem);
                if (header->code == TVN_SELCHANGEDW && app->treePaths.contains(info->itemNew.hItem)) { app->directory = app->treePaths[info->itemNew.hItem]; app->scan(); }
            }
            return 0;
        }
        case WM_DROPFILES: { HDROP drop = reinterpret_cast<HDROP>(w); UINT length = DragQueryFileW(drop, 0, nullptr, 0); std::wstring path(length+1, L'\0'); DragQueryFileW(drop, 0, path.data(), length+1); DragFinish(drop); path.resize(length); app->openPath(path); return 0; }
        case WM_READER: app->acceptReader(std::unique_ptr<ReaderEvent>(reinterpret_cast<ReaderEvent*>(l))); return 0;
        case WM_CATALOG: app->acceptCatalog(std::unique_ptr<CatalogEvent>(reinterpret_cast<CatalogEvent*>(l))); return 0;
        case WM_CONTEXTMENU: if (reinterpret_cast<HWND>(w) == app->tags) { HMENU menu = CreatePopupMenu(); AppendMenuW(menu, MF_STRING, TagRename, L"重命名标签"); AppendMenuW(menu, MF_STRING, TagDelete, L"删除标签"); int id = TrackPopupMenu(menu, TPM_RETURNCMD, GET_X_LPARAM(l), GET_Y_LPARAM(l), 0, window, nullptr); DestroyMenu(menu); if (id) app->command(id); return 0; } break;
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
