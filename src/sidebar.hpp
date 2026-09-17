#pragma once
#include "common.hpp"
#include <commctrl.h>
#include <unordered_map>
#include <unordered_set>

namespace cv {
inline constexpr UINT WM_SIDEBAR_LAYOUT = WM_APP + 10;
inline constexpr UINT WM_DIRECTORY_SELECTED = WM_APP + 11;

// 目录、标签和虚拟文件列表共用一个可停靠容器；读取逻辑仍由主窗口负责。
class Sidebar {
public:
    struct Controls { int recursive, match, tags, files, tree, create, clear, filter, add, remove, dropdown, tagTabs, filterTags; };
    HWND window{}, files{}, tags{}, filterTags{}, tagTabs{}, recursive{}, match{}, dropdown{};
    bool right{};
    int widthDip{380};
    void create(HINSTANCE instance, HWND owner, HFONT font, Controls ids);
    RECT layout(int width, int height, int top, int bottom, bool full);
    void setFont(HFONT font);
    void setDirectory(const fs::path& directory);
    bool tagging() const;
    void selectTagPage(bool edit);
    void setTagSummary(int fileCount, int filterCount);
    const fs::path& directory() const { return directory_; }
    void toggleDirectory();
    void closeDirectory(bool restoreFocus = false);
    bool preTranslate(const MSG& message);
    void destroy();
private:
    static LRESULT CALLBACK procedure(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK childProcedure(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    int px(int value) const;
    HWND control(const wchar_t* type, const wchar_t* text, DWORD style, int id, DWORD extended = 0);
    void arrange();
    HTREEITEM addDirectory(HTREEITEM parent, const std::wstring& label, const fs::path& path);
    void expandDirectory(HTREEITEM node);
    void selectCurrentDirectory();
    void confirmDirectory();
    void finishDrag(HWND control, bool accept);
    HWND owner_{}, title_{}, splitter_{}, pathLabel_{}, tagHint_{}, popup_{}, tree_{};
    HINSTANCE instance_{};
    HFONT font_{};
    Controls ids_{};
    fs::path directory_;
    std::unordered_map<HTREEITEM, fs::path> paths_;
    std::unordered_set<HTREEITEM> expanded_;
    bool sizing_{}, docking_{}, populating_{}, closingDirectoryClick_{};
    int selectedFiles_{}, selectedFilters_{};
};
}
