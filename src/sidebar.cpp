#include "sidebar.hpp"
#include <windowsx.h>

namespace cv {
int Sidebar::px(int value) const { return MulDiv(value, GetDpiForWindow(owner_), 96); }

HWND Sidebar::control(const wchar_t* type, const wchar_t* text, DWORD style, int id, DWORD extended) {
    HWND child = CreateWindowExW(extended, type, text, WS_CHILD | WS_VISIBLE | style,
        0, 0, 100, 28, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
    SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    return child;
}

void Sidebar::create(HINSTANCE instance, HWND owner, HFONT font, Controls ids) {
    instance_ = instance; owner_ = owner; font_ = font; ids_ = ids;
    WNDCLASSEXW wc{sizeof(wc)}; wc.lpfnWndProc = procedure; wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW); wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"ComicViewerEx.Sidebar"; RegisterClassExW(&wc);
    wc.lpszClassName = L"ComicViewerEx.DirectoryPopup"; RegisterClassExW(&wc);
    window = CreateWindowExW(0, L"ComicViewerEx.Sidebar", nullptr,
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, 0, 0, 1, 1, owner, nullptr, instance, this);
    title_ = control(L"STATIC", L"漫画目录 · 拖动至左侧或右侧", SS_CENTER | SS_CENTERIMAGE | SS_NOTIFY, 0);
    SetWindowSubclass(title_, childProcedure, 1, reinterpret_cast<DWORD_PTR>(this));
    splitter_ = CreateWindowExW(0, L"STATIC", nullptr, WS_CHILD | WS_VISIBLE | SS_NOTIFY,
        0, 0, 1, 1, owner, nullptr, instance, nullptr);
    SetWindowSubclass(splitter_, childProcedure, 2, reinterpret_cast<DWORD_PTR>(this));
    pathLabel_ = control(L"STATIC", L"选择目录…", SS_PATHELLIPSIS | SS_CENTERIMAGE | SS_NOTIFY, 0, WS_EX_STATICEDGE);
    SetWindowSubclass(pathLabel_, childProcedure, 4, reinterpret_cast<DWORD_PTR>(this));
    dropdown = control(L"BUTTON", L"▾", BS_PUSHBUTTON | WS_TABSTOP, ids.dropdown);
    recursive = control(L"BUTTON", L"包含子目录", BS_AUTOCHECKBOX | WS_TABSTOP, ids.recursive);
    match = control(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP, ids.match);
    SendMessageW(match, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"全部标签匹配"));
    SendMessageW(match, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"任一标签匹配"));
    SendMessageW(match, CB_SETCURSEL, 0, 0);
    tagTabs = control(WC_TABCONTROLW, L"", WS_TABSTOP, ids.tagTabs);
    for (auto title : {L"筛选", L"打标"}) {
        TCITEMW item{}; item.mask = TCIF_TEXT; item.pszText = const_cast<wchar_t*>(title);
        TabCtrl_InsertItem(tagTabs, TabCtrl_GetItemCount(tagTabs), &item);
    }
    auto checklist = [&](int id) {
        HWND list = control(WC_LISTVIEWW, L"", LVS_REPORT | LVS_NOCOLUMNHEADER | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_TABSTOP, id, WS_EX_CLIENTEDGE);
        ListView_SetExtendedListViewStyle(list, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        LVCOLUMNW column{LVCF_TEXT | LVCF_WIDTH}; column.pszText = const_cast<wchar_t*>(L"标签"); column.cx = px(200);
        ListView_InsertColumn(list, 0, &column); return list;
    };
    tags = checklist(ids.tags); filterTags = checklist(ids.filterTags);
    tagHint_ = control(L"STATIC", L"勾选立即筛选 · 已选 0 个标签", SS_LEFT | SS_CENTERIMAGE, 0);
    control(L"BUTTON", L"新建标签", BS_PUSHBUTTON | WS_TABSTOP, ids.create);
    control(L"BUTTON", L"清除筛选", BS_PUSHBUTTON | WS_TABSTOP, ids.clear);
    files = control(WC_LISTVIEWW, L"", LVS_REPORT | LVS_OWNERDATA | LVS_SHOWSELALWAYS | WS_TABSTOP, ids.files, WS_EX_CLIENTEDGE);
    ListView_SetExtendedListViewStyle(files, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    const wchar_t* columns[] = {L"文件名", L"标签", L"大小"};
    for (int i = 0; i < 3; ++i) {
        LVCOLUMNW column{LVCF_TEXT | LVCF_WIDTH}; column.pszText = const_cast<wchar_t*>(columns[i]); column.cx = px(100);
        ListView_InsertColumn(files, i, &column);
    }
    control(L"BUTTON", L"添加所选标签", BS_PUSHBUTTON | WS_TABSTOP, ids.add);
    control(L"BUTTON", L"移除所选标签", BS_PUSHBUTTON | WS_TABSTOP, ids.remove);
    popup_ = CreateWindowExW(WS_EX_TOOLWINDOW, L"ComicViewerEx.DirectoryPopup", nullptr,
        WS_POPUP | WS_BORDER | WS_CLIPCHILDREN, 0, 0, 1, 1, owner, nullptr, instance, this);
    tree_ = CreateWindowExW(0, WC_TREEVIEWW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
        TVS_HASBUTTONS | TVS_HASLINES | TVS_LINESATROOT | TVS_SHOWSELALWAYS,
        0, 0, 1, 1, popup_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ids.tree)), instance, nullptr);
    SetWindowSubclass(tree_, childProcedure, 3, reinterpret_cast<DWORD_PTR>(this));
    SendMessageW(tree_, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    wchar_t drives[512]{}; GetLogicalDriveStringsW(512, drives);
    for (wchar_t* drive = drives; *drive; drive += wcslen(drive) + 1) addDirectory(TVI_ROOT, drive, drive);
}

void Sidebar::setFont(HFONT font) {
    font_ = font;
    EnumChildWindows(window, [](HWND child, LPARAM value) -> BOOL { SendMessageW(child, WM_SETFONT, value, TRUE); return TRUE; }, reinterpret_cast<LPARAM>(font));
    SendMessageW(tree_, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

RECT Sidebar::layout(int width, int height, int top, int bottom, bool full) {
    closeDirectory();
    ShowWindow(window, full ? SW_HIDE : SW_SHOW); ShowWindow(splitter_, full ? SW_HIDE : SW_SHOW);
    if (full) return {0, 0, width, height};
    const int divider = px(5);
    const int maximum = std::max(0, width - px(320) - divider);
    const int side = std::clamp(px(widthDip), std::min(px(280), maximum), maximum);
    const int x = right ? width - side : 0;
    MoveWindow(window, x, top, side, std::max(0, height - top - bottom), TRUE);
    MoveWindow(splitter_, right ? x - divider : side, top, divider, std::max(0, height - top - bottom), TRUE);
    arrange();
    return {right ? 0 : side + divider, top, right ? x - divider : width, std::max(top, height - bottom)};
}

void Sidebar::arrange() {
    if (!files) return;
    RECT area{}; GetClientRect(window, &area);
    int inner = std::max(1, static_cast<int>(area.right) - px(16)), y = px(5);
    auto move = [](HWND h, int x, int y, int w, int height) { MoveWindow(h, x, y, std::max(1, w), std::max(1, height), TRUE); };
    move(title_, px(8), y, inner, px(24)); y += px(29);
    move(pathLabel_, px(8), y, inner - px(32), px(28));
    move(dropdown, px(8) + inner - px(28), y, px(28), px(28)); y += px(35);
    move(recursive, px(8), y, px(126), px(25));
    move(match, px(138), y, inner - px(130), px(160)); y += px(32);
    const bool edit = tagging();
    ShowWindow(match, edit ? SW_HIDE : SW_SHOW);
    move(tagTabs, px(8), y, inner, px(27)); y += px(29);
    move(tags, px(8), y, inner, px(82)); move(filterTags, px(8), y, inner, px(82)); y += px(86);
    ShowWindow(tags, edit ? SW_SHOW : SW_HIDE); ShowWindow(filterTags, edit ? SW_HIDE : SW_SHOW);
    ListView_SetColumnWidth(tags, 0, std::max(1, inner - px(24))); ListView_SetColumnWidth(filterTags, 0, std::max(1, inner - px(24)));
    move(tagHint_, px(8), y, inner, px(23)); y += px(27);
    int x = px(8);
    for (int id : {ids_.create, ids_.clear, ids_.add, ids_.remove}) {
        const bool visible = id == ids_.create || (edit ? id != ids_.clear : id == ids_.clear);
        ShowWindow(GetDlgItem(window, id), visible ? SW_SHOW : SW_HIDE);
        if (!visible) continue;
        if (x > px(8) && x + px(100) > px(8) + inner) { x = px(8); y += px(32); }
        move(GetDlgItem(window, id), x, y, px(100), px(26)); x += px(108);
    }
    y += px(33);
    const int listHeight = std::max(px(60), static_cast<int>(area.bottom) - y - px(8));
    move(files, px(8), y, inner, listHeight);
    ListView_SetColumnWidth(files, 0, std::max(px(110), inner - px(180)));
    ListView_SetColumnWidth(files, 1, px(100)); ListView_SetColumnWidth(files, 2, px(72));
}

bool Sidebar::tagging() const { return TabCtrl_GetCurSel(tagTabs) == 1; }

void Sidebar::selectTagPage(bool edit) {
    TabCtrl_SetCurSel(tagTabs, edit ? 1 : 0);
    setTagSummary(selectedFiles_, selectedFilters_); arrange();
}

void Sidebar::setTagSummary(int fileCount, int filterCount) {
    selectedFiles_ = fileCount; selectedFilters_ = filterCount;
    auto hint = tagging() ? L"已选 " + std::to_wstring(fileCount) + L" 个文件 · 勾选待操作标签" :
        L"勾选立即筛选 · 已选 " + std::to_wstring(filterCount) + L" 个标签";
    SetWindowTextW(tagHint_, hint.c_str());
    EnableWindow(GetDlgItem(window, ids_.add), fileCount > 0); EnableWindow(GetDlgItem(window, ids_.remove), fileCount > 0);
}

void Sidebar::setDirectory(const fs::path& directory) {
    directory_ = directory;
    SetWindowTextW(pathLabel_, directory.empty() ? L"选择目录…" : directory.c_str());
}

HTREEITEM Sidebar::addDirectory(HTREEITEM parent, const std::wstring& label, const fs::path& path) {
    TVINSERTSTRUCTW item{}; item.hParent = parent; item.hInsertAfter = TVI_SORT;
    item.item.mask = TVIF_TEXT | TVIF_CHILDREN; item.item.pszText = const_cast<wchar_t*>(label.c_str()); item.item.cChildren = 1;
    auto node = TreeView_InsertItem(tree_, &item); paths_[node] = path; return node;
}

void Sidebar::expandDirectory(HTREEITEM node) {
    if (!paths_.contains(node) || expanded_.contains(node)) return;
    std::error_code error;
    fs::directory_iterator entries(paths_.at(node), error);
    if (error) throw std::runtime_error("无法访问目录：" + utf8(paths_.at(node).wstring()));
    std::vector<fs::path> children;
    for (auto end = fs::directory_iterator{}; entries != end; entries.increment(error)) {
        if (error) break;
        std::error_code itemError;
        if (entries->is_directory(itemError) && !entries->is_symlink(itemError)) children.push_back(entries->path());
    }
    if (error) throw std::runtime_error("读取目录失败：" + utf8(paths_.at(node).wstring()));
    for (const auto& child : children) addDirectory(node, child.filename().wstring(), child);
    expanded_.insert(node);
}

void Sidebar::selectCurrentDirectory() {
    if (directory_.empty()) return;
    auto path = directory_.lexically_normal();
    HTREEITEM selected{};
    auto find = [&](const fs::path& candidate) {
        for (const auto& [node, value] : paths_) if (lower(value.lexically_normal().wstring()) == lower(candidate.wstring())) return node;
        return static_cast<HTREEITEM>(nullptr);
    };
    auto parent = path.root_path();
    selected = find(parent);
    if (!selected) selected = addDirectory(TVI_ROOT, parent.wstring(), parent);
    for (const auto& part : path.relative_path()) {
        expandDirectory(selected);
        TreeView_Expand(tree_, selected, TVE_EXPAND);
        parent /= part;
        auto child = find(parent); if (!child) break; selected = child;
    }
    TreeView_SelectItem(tree_, selected); TreeView_EnsureVisible(tree_, selected);
}

void Sidebar::toggleDirectory() {
    if (IsWindowVisible(popup_)) { closeDirectory(true); return; }
    RECT button{}, arrow{}; GetWindowRect(pathLabel_, &button); GetWindowRect(dropdown, &arrow); button.right = arrow.right; button.bottom = arrow.bottom;
    MONITORINFO monitor{sizeof(monitor)}; GetMonitorInfoW(MonitorFromWindow(dropdown, MONITOR_DEFAULTTONEAREST), &monitor);
    const int width = std::min(static_cast<int>(monitor.rcWork.right - monitor.rcWork.left), std::max(px(280), static_cast<int>(button.right - button.left)));
    const int height = std::min(px(320), static_cast<int>(monitor.rcWork.bottom - monitor.rcWork.top));
    const int x = std::clamp(static_cast<int>(button.left), static_cast<int>(monitor.rcWork.left), static_cast<int>(monitor.rcWork.right) - width);
    const int y = std::clamp(static_cast<int>(button.bottom), static_cast<int>(monitor.rcWork.top), static_cast<int>(monitor.rcWork.bottom) - height);
    SetWindowPos(popup_, HWND_TOP, x, y, width, height, SWP_SHOWWINDOW);
    populating_ = true;
    try { selectCurrentDirectory(); } catch (...) { populating_ = false; closeDirectory(true); throw; }
    populating_ = false; SetFocus(tree_);
}

void Sidebar::closeDirectory(bool restoreFocus) {
    if (!popup_ || !IsWindowVisible(popup_)) return;
    ShowWindow(popup_, SW_HIDE);
    if (restoreFocus) { SetActiveWindow(owner_); SetFocus(dropdown); }
}

void Sidebar::confirmDirectory() {
    auto node = TreeView_GetSelection(tree_);
    if (!paths_.contains(node)) return;
    std::error_code error;
    if (!fs::is_directory(paths_.at(node), error)) throw std::runtime_error("目录不存在或无法访问");
    fs::directory_iterator accessible(paths_.at(node), error);
    if (error) throw std::runtime_error("无法读取此目录");
    setDirectory(paths_.at(node)); closeDirectory(true);
    SendMessageW(owner_, WM_DIRECTORY_SELECTED, 0, 0);
}

bool Sidebar::preTranslate(const MSG& message) {
    if (closingDirectoryClick_ && message.message == WM_LBUTTONUP) {
        closingDirectoryClick_ = false;
        if (message.hwnd == dropdown || message.hwnd == pathLabel_) return true;
    }
    if (message.message == WM_LBUTTONDOWN) closingDirectoryClick_ = false;
    if (!popup_ || !IsWindowVisible(popup_)) return false;
    if (message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE) { closeDirectory(true); return true; }
    if ((message.message == WM_LBUTTONDOWN || message.message == WM_RBUTTONDOWN || message.message == WM_MBUTTONDOWN || message.message == WM_NCLBUTTONDOWN) &&
        message.hwnd != popup_ && !IsChild(popup_, message.hwnd)) {
        bool entrance = message.hwnd == dropdown || message.hwnd == pathLabel_;
        closeDirectory(entrance);
        if (entrance && message.message == WM_LBUTTONDOWN) { closingDirectoryClick_ = true; return true; }
    }
    return false;
}

void Sidebar::finishDrag(HWND control, bool accept) {
    bool save = sizing_;
    if (docking_ && accept) {
        POINT point{}; GetCursorPos(&point); ScreenToClient(owner_, &point);
        RECT area{}; GetClientRect(owner_, &area);
        if (point.y >= 0 && point.y < area.bottom) {
            if (point.x >= 0 && point.x <= px(48)) { right = false; save = true; }
            else if (point.x >= area.right - px(48) && point.x < area.right) { right = true; save = true; }
        }
    }
    sizing_ = docking_ = false;
    if (GetCapture() == control) ReleaseCapture();
    if (save) SendMessageW(owner_, WM_SIDEBAR_LAYOUT, 1, 0);
}

LRESULT CALLBACK Sidebar::childProcedure(HWND child, UINT message, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR data) {
    auto self = reinterpret_cast<Sidebar*>(data);
    try {
        if (message == WM_NCDESTROY) RemoveWindowSubclass(child, childProcedure, id);
        if (id == 4) {
            if (message == WM_LBUTTONUP) { self->toggleDirectory(); return 0; }
        } else if (id == 3) {
            if (message == WM_KEYDOWN && w == VK_ESCAPE) { self->closeDirectory(true); return 0; }
            if (message == WM_KEYDOWN && w == VK_RETURN) { self->confirmDirectory(); return 0; }
            if (message == WM_LBUTTONUP) {
                auto result = DefSubclassProc(child, message, w, l);
                TVHITTESTINFO hit{}; hit.pt = {GET_X_LPARAM(l), GET_Y_LPARAM(l)};
                TreeView_HitTest(child, &hit);
                if (hit.hItem && (hit.flags & (TVHT_ONITEMLABEL | TVHT_ONITEMICON))) { TreeView_SelectItem(child, hit.hItem); self->confirmDirectory(); }
                return result;
            }
        } else {
            if (message == WM_SETCURSOR) { SetCursor(LoadCursorW(nullptr, id == 1 ? IDC_SIZEALL : IDC_SIZEWE)); return TRUE; }
            if (message == WM_LBUTTONDOWN) { self->closeDirectory(); self->sizing_ = id == 2; self->docking_ = id == 1; SetCapture(child); return 0; }
            if (message == WM_MOUSEMOVE && self->sizing_ && GetCapture() == child) {
                POINT point{}; GetCursorPos(&point); ScreenToClient(self->owner_, &point);
                RECT area{}; GetClientRect(self->owner_, &area);
                int maximum = std::max(self->px(280), static_cast<int>(area.right) - self->px(325));
                int width = std::clamp(self->right ? static_cast<int>(area.right - point.x) : static_cast<int>(point.x), self->px(280), maximum);
                self->widthDip = MulDiv(width, 96, GetDpiForWindow(self->owner_));
                SendMessageW(self->owner_, WM_SIDEBAR_LAYOUT, 0, 0); return 0;
            }
            if (message == WM_LBUTTONUP) { self->finishDrag(child, true); return 0; }
            if (message == WM_CAPTURECHANGED || message == WM_CANCELMODE) { self->finishDrag(child, false); return 0; }
        }
    } catch (const std::exception& error) { MessageBoxW(self->owner_, wide(error.what()).c_str(), L"ComicViewerEx", MB_OK | MB_ICONWARNING); return 0; }
    return DefSubclassProc(child, message, w, l);
}

LRESULT CALLBACK Sidebar::procedure(HWND window, UINT message, WPARAM w, LPARAM l) {
    auto self = reinterpret_cast<Sidebar*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) { self = static_cast<Sidebar*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams); SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self)); }
    if (!self) return DefWindowProcW(window, message, w, l);
    try {
        bool popup = (GetWindowLongPtrW(window, GWL_STYLE) & WS_POPUP) != 0;
        if (message == WM_SIZE) {
            if (popup) { RECT area{}; GetClientRect(window, &area); MoveWindow(self->tree_, 0, 0, area.right, area.bottom, TRUE); }
            else self->arrange();
            return 0;
        }
        if (popup) {
            // 点击宿主时，保留可见状态至 preTranslate 判别点击入口，避免关闭后又被同一次点击重新打开。
            if (message == WM_ACTIVATE && LOWORD(w) == WA_INACTIVE && reinterpret_cast<HWND>(l) != self->owner_) self->closeDirectory();
            if (message == WM_CLOSE) { self->closeDirectory(true); return 0; }
            if (message == WM_NOTIFY) {
                auto info = reinterpret_cast<NMHDR*>(l);
                if (info->code == TVN_ITEMEXPANDINGW && !self->populating_) self->expandDirectory(reinterpret_cast<NMTREEVIEWW*>(l)->itemNew.hItem);
                return 0;
            }
        } else {
            if (message == WM_NOTIFY) {
                auto info = reinterpret_cast<NMHDR*>(l);
                if (info->hwndFrom == self->tagTabs && info->code == TCN_SELCHANGE) {
                    self->setTagSummary(self->selectedFiles_, self->selectedFilters_); self->arrange(); return 0;
                }
            }
            if (message == WM_COMMAND || message == WM_NOTIFY || message == WM_CONTEXTMENU) return SendMessageW(self->owner_, message, w, l);
        }
    } catch (const std::exception& error) { MessageBoxW(self->owner_, wide(error.what()).c_str(), L"ComicViewerEx", MB_OK | MB_ICONWARNING); return TRUE; }
    return DefWindowProcW(window, message, w, l);
}

void Sidebar::destroy() {
    if (popup_) { DestroyWindow(popup_); popup_ = nullptr; }
    if (splitter_) { DestroyWindow(splitter_); splitter_ = nullptr; }
    if (window) { DestroyWindow(window); window = nullptr; }
}
}
