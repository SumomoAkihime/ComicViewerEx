#include "tag_bar.hpp"

#include <windowsx.h>
#include <cstdlib>

namespace cv {
namespace {
constexpr wchar_t kClassName[] = L"ComicViewerEx.TagBar";

struct PaintFont {
    HDC dc{};
    HFONT old{};

    PaintFont(HDC value, HFONT font) : dc(value) {
        if (font) old = static_cast<HFONT>(SelectObject(dc, font));
    }
    ~PaintFont() {
        if (old) SelectObject(dc, old);
    }
    PaintFont(const PaintFont&) = delete;
    PaintFont& operator=(const PaintFont&) = delete;
};

void deleteGdiObject(HGDIOBJ object) {
    if (object) DeleteObject(object);
}
}

int TagBar::dpiFor(HWND window) {
    return window ? static_cast<int>(GetDpiForWindow(window)) : 96;
}

int TagBar::px(int value) const {
    return MulDiv(value, dpiFor(owner_ ? owner_ : window), 96);
}

void TagBar::create(HINSTANCE instance, HWND owner, HFONT font) {
    destroy();
    instance_ = instance;
    owner_ = owner;
    font_ = font;

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = procedure;
    wc.hInstance = instance_;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);

    window = CreateWindowExW(0, kClassName, nullptr,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | WS_CLIPCHILDREN,
        0, 0, 1, 1, owner_, nullptr, instance_, this);
    if (window && font_) SendMessageW(window, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
}

void TagBar::setFont(HFONT font) {
    font_ = font;
    if (window) {
        SendMessageW(window, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        rebuildLayout();
        InvalidateRect(window, nullptr, TRUE);
    }
}

void TagBar::setContent(const std::wstring& context, const std::vector<Tag>& items,
    const std::vector<int64_t>& activeFilters) {
    const int64_t focusedId = focusIndex_ >= 0 && focusIndex_ < static_cast<int>(chips_.size()) ? chips_[focusIndex_].tag.id : 0;
    bool same = items.size() == chips_.size();
    for (size_t i = 0; same && i < items.size(); ++i) same = items[i].id == chips_[i].tag.id && items[i].name == chips_[i].tag.name;
    const bool changedContext = context_ != context;
    context_ = context;
    chips_.clear();
    chips_.reserve(items.size());
    for (const auto& item : items) chips_.push_back({item, {}});
    activeFilters_ = activeFilters;
    if (!same) scrollOffset_ = 0;
    focusIndex_ = chips_.empty() ? -1 : std::clamp(focusIndex_, 0, static_cast<int>(chips_.size()) - 1);
    for (int i = 0; i < static_cast<int>(chips_.size()); ++i) if (chips_[i].tag.id == focusedId) focusIndex_ = i;
    if (!same || changedContext) {
        pressedIndex_ = -1;
        if (window && GetCapture() == window) ReleaseCapture();
        wheelRemainder_ = 0;
    }
    if (window) {
        rebuildLayout();
        InvalidateRect(window, nullptr, TRUE);
    }
}

int TagBar::textHeight() const {
    HDC dc = GetDC(window);
    if (!dc) return px(16);
    int result = px(16);
    {
        PaintFont selected(dc, font_);
        TEXTMETRICW metrics{};
        if (GetTextMetricsW(dc, &metrics)) result = static_cast<int>(metrics.tmHeight);
    }
    ReleaseDC(window, dc);
    return result;
}

int TagBar::chipRowHeight() const {
    return std::max(px(26), textHeight() + px(10));
}

int TagBar::textWidth(const std::wstring& value) const {
    HDC dc = GetDC(window);
    if (!dc) return 0;
    int result = 0;
    {
        PaintFont selected(dc, font_);
        SIZE size{};
        GetTextExtentPoint32W(dc, value.c_str(), static_cast<int>(value.size()), &size);
        result = size.cx;
    }
    ReleaseDC(window, dc);
    return result;
}

bool TagBar::isActive(int64_t id) const {
    return std::find(activeFilters_.begin(), activeFilters_.end(), id) != activeFilters_.end();
}

int TagBar::preferredHeight(int) const {
    return chipRowHeight() + px(8);
}

void TagBar::updateScrollBar(int maxScroll, int page) {
    maxScroll_ = std::max(0, maxScroll);
    scrollOffset_ = std::clamp(scrollOffset_, 0, maxScroll_);
    SCROLLINFO info{sizeof(info), SIF_RANGE | SIF_PAGE | SIF_POS | SIF_DISABLENOSCROLL};
    info.nMin = 0;
    info.nMax = maxScroll_ + std::max(1, page) - 1;
    info.nPage = static_cast<UINT>(std::max(1, page));
    info.nPos = scrollOffset_;
    SetScrollInfo(window, SB_VERT, &info, TRUE);
}

void TagBar::rebuildLayout() {
    if (!window) return;
    RECT area{};
    GetClientRect(window, &area);
    const int gap = px(6);
    const int rowHeight = chipRowHeight();
    const auto clip = viewport();
    const int contentTop = clip.top;
    const int width = std::max(1L, clip.right - clip.left);
    const int viewportHeight = std::max(1L, clip.bottom - clip.top);

    for (auto& chip : chips_) chip.bounds = {};
    if (chips_.empty()) {
        const int contentHeight = rowHeight;
        const int previousScroll = scrollOffset_;
        updateScrollBar(std::max(0, contentHeight - viewportHeight), viewportHeight);
        if (previousScroll != scrollOffset_) rebuildLayout();
        return;
    }

    int x = clip.left;
    int y = contentTop - scrollOffset_;
    for (auto& chip : chips_) {
        const int desired = std::min(width, std::max(px(34), textWidth(chip.tag.name) + px(20)));
        if (x != clip.left && x + desired > clip.right) {
            x = clip.left;
            y += rowHeight;
        }
        const int left = x;
        chip.bounds = {left, y, left + desired, y + rowHeight};
        x += desired + gap;
    }
    const int rows = std::max(1, (y - (contentTop - scrollOffset_)) / rowHeight + 1);
    const int contentHeight = rows * rowHeight;
    const int previousScroll = scrollOffset_;
    updateScrollBar(std::max(0, contentHeight - viewportHeight), viewportHeight);
    if (previousScroll != scrollOffset_) rebuildLayout();
}

void TagBar::scrollTo(int value) {
    const int target = std::clamp(value, 0, maxScroll_);
    if (target == scrollOffset_) return;
    scrollOffset_ = target;
    rebuildLayout();
    InvalidateRect(window, nullptr, TRUE);
}

int TagBar::hitTest(POINT point) const {
    const auto clip = viewport();
    if (!PtInRect(&clip, point)) return -1;
    for (int i = 0; i < static_cast<int>(chips_.size()); ++i) {
        if (PtInRect(&chips_[i].bounds, point)) return i;
    }
    return -1;
}

RECT TagBar::viewport() const {
    RECT area{}; GetClientRect(window, &area);
    const int margin = px(8), top = px(4);
    const int titleWidth = context_.empty() ? 0 : std::min(px(280), static_cast<int>(area.right) / 3) + margin;
    const int left = margin + titleWidth;
    return {left, top, std::max(static_cast<LONG>(left), area.right - margin),
        std::max(static_cast<LONG>(top), std::min(area.bottom - top, static_cast<LONG>(top + chipRowHeight())))};
}

void TagBar::ensureFocusVisible() {
    if (focusIndex_ < 0 || focusIndex_ >= static_cast<int>(chips_.size())) return;
    const auto clip = viewport(); const auto bounds = chips_[focusIndex_].bounds;
    if (bounds.top < clip.top) scrollTo(scrollOffset_ + bounds.top - clip.top);
    else if (bounds.bottom > clip.bottom) scrollTo(scrollOffset_ + bounds.bottom - clip.bottom);
}

void TagBar::activate(int index) {
    if (index < 0 || index >= static_cast<int>(chips_.size()) || !owner_) return;
    PostMessageW(owner_, WM_TAG_FILTER_TOGGLE, static_cast<WPARAM>(chips_[index].tag.id), 0);
}

void TagBar::paint(HDC dc) {
    RECT area{};
    GetClientRect(window, &area);
    const int margin = px(8);
    const auto clip = viewport();

    HBRUSH background = CreateSolidBrush(GetSysColor(COLOR_WINDOW));
    FillRect(dc, &area, background);
    deleteGdiObject(background);
    PaintFont selected(dc, font_);

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
    if (!context_.empty()) {
        RECT title{margin, 0, clip.left - margin, area.bottom};
        DrawTextW(dc, context_.c_str(), static_cast<int>(context_.size()), &title,
            DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    }

    SaveDC(dc);
    IntersectClipRect(dc, clip.left, clip.top, clip.right, clip.bottom);
    for (int i = 0; i < static_cast<int>(chips_.size()); ++i) {
        const auto& chip = chips_[i];
        if (chip.bounds.bottom <= clip.top || chip.bounds.top >= clip.bottom) continue;
        const bool active = isActive(chip.tag.id);
        const bool focused = i == focusIndex_ && GetFocus() == window;
        HBRUSH fill = CreateSolidBrush(active ? RGB(220, 234, 255) : RGB(242, 244, 247));
        HPEN outline = CreatePen(PS_SOLID, px(1), active ? RGB(66, 116, 190) : RGB(180, 186, 196));
        HGDIOBJ oldBrush = SelectObject(dc, fill);
        HGDIOBJ oldPen = SelectObject(dc, outline);
        RoundRect(dc, chip.bounds.left, chip.bounds.top, chip.bounds.right, chip.bounds.bottom,
            px(12), px(12));
        SelectObject(dc, oldPen);
        SelectObject(dc, oldBrush);
        deleteGdiObject(outline);
        deleteGdiObject(fill);

        RECT text = chip.bounds;
        text.left += px(10);
        text.right -= px(10);
        SetTextColor(dc, active ? RGB(32, 77, 143) : GetSysColor(COLOR_WINDOWTEXT));
        DrawTextW(dc, chip.tag.name.c_str(), static_cast<int>(chip.tag.name.size()), &text,
            DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        if (focused) {
            RECT focus = chip.bounds;
            InflateRect(&focus, -px(2), -px(2));
            DrawFocusRect(dc, &focus);
        }
    }
    if (chips_.empty()) {
        RECT empty = clip;
        SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
        DrawTextW(dc, L"暂无标签", -1, &empty, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    }
    RestoreDC(dc, -1);
}

LRESULT CALLBACK TagBar::procedure(HWND window, UINT message, WPARAM w, LPARAM l) {
    auto self = reinterpret_cast<TagBar*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        self = static_cast<TagBar*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self) return DefWindowProcW(window, message, w, l);

    switch (message) {
    case WM_SETFONT:
        self->font_ = reinterpret_cast<HFONT>(w);
        self->rebuildLayout();
        if (l) InvalidateRect(window, nullptr, TRUE);
        return 0;
    case WM_SIZE:
        self->rebuildLayout();
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        self->paint(dc);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_VSCROLL: {
        SCROLLINFO info{sizeof(info), SIF_ALL};
        GetScrollInfo(window, SB_VERT, &info);
        int target = info.nPos;
        switch (LOWORD(w)) {
        case SB_LINEUP: target -= self->chipRowHeight(); break;
        case SB_LINEDOWN: target += self->chipRowHeight(); break;
        case SB_PAGEUP: target -= static_cast<int>(info.nPage); break;
        case SB_PAGEDOWN: target += static_cast<int>(info.nPage); break;
        case SB_THUMBTRACK: target = info.nTrackPos; break;
        case SB_TOP: target = info.nMin; break;
        case SB_BOTTOM: target = info.nMax; break;
        default: break;
        }
        self->scrollTo(target);
        return 0;
    }
    case WM_MOUSEWHEEL:
        self->pressedIndex_ = -1;
        if (GetCapture() == window) ReleaseCapture();
        self->wheelRemainder_ += GET_WHEEL_DELTA_WPARAM(w);
        while (std::abs(self->wheelRemainder_) >= WHEEL_DELTA) {
            const int direction = self->wheelRemainder_ > 0 ? -1 : 1;
            self->wheelRemainder_ -= (self->wheelRemainder_ > 0 ? WHEEL_DELTA : -WHEEL_DELTA);
            self->scrollTo(self->scrollOffset_ + direction * self->chipRowHeight());
        }
        return 0;
    case WM_LBUTTONDOWN: {
        SetFocus(window);
        POINT point{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        self->focusIndex_ = self->hitTest(point);
        self->pressedIndex_ = self->focusIndex_;
        if (self->pressedIndex_ >= 0) {
            SetCapture(window);
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    }
    case WM_MOUSEMOVE:
        if (self->pressedIndex_ >= 0 && GetCapture() == window) {
            POINT point{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            const int hovered = self->hitTest(point);
            if (hovered != self->focusIndex_) {
                self->focusIndex_ = hovered;
                InvalidateRect(window, nullptr, FALSE);
            }
        }
        return 0;
    case WM_LBUTTONUP: {
        POINT point{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        const int pressed = self->pressedIndex_;
        const bool activate = pressed >= 0 && GetCapture() == window && self->hitTest(point) == pressed;
        self->pressedIndex_ = -1;
        if (GetCapture() == window) ReleaseCapture();
        if (activate) self->activate(pressed);
        return 0;
    }
    case WM_CAPTURECHANGED:
    case WM_CANCELMODE:
        self->pressedIndex_ = -1;
        if (message == WM_CANCELMODE && GetCapture() == window) ReleaseCapture();
        return 0;
    case WM_KEYDOWN: {
        if (self->chips_.empty()) return 0;
        int index = self->focusIndex_ < 0 ? 0 : self->focusIndex_;
        const int count = static_cast<int>(self->chips_.size());
        switch (w) {
        case VK_LEFT: case VK_UP: index = std::max(0, index - 1); break;
        case VK_RIGHT: case VK_DOWN: index = std::min(count - 1, index + 1); break;
        case VK_HOME: index = 0; break;
        case VK_END: index = count - 1; break;
        case VK_RETURN: case VK_SPACE: self->activate(index); return 0;
        default: return 0;
        }
        self->focusIndex_ = index;
        self->ensureFocusVisible();
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    }
    case WM_SETFOCUS:
        if (self->focusIndex_ < 0 && !self->chips_.empty()) self->focusIndex_ = 0;
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_KILLFOCUS:
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_NCDESTROY:
        if (self->window == window) self->window = nullptr;
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        return DefWindowProcW(window, message, w, l);
    default:
        return DefWindowProcW(window, message, w, l);
    }
}

void TagBar::destroy() {
    if (window) DestroyWindow(window);
    window = nullptr;
    owner_ = nullptr;
    instance_ = nullptr;
    font_ = nullptr;
    chips_.clear();
    activeFilters_.clear();
    context_.clear();
    scrollOffset_ = 0;
    maxScroll_ = 0;
    focusIndex_ = -1;
    pressedIndex_ = -1;
    wheelRemainder_ = 0;
}
}
