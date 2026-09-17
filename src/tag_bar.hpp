#pragma once

#include "common.hpp"

namespace cv {
inline constexpr UINT WM_TAG_FILTER_TOGGLE = WM_APP + 12;

class TagBar {
public:
    HWND window{};

    void create(HINSTANCE instance, HWND owner, HFONT font);
    void setFont(HFONT font);
    void setContent(const std::wstring& context, const std::vector<Tag>& items,
        const std::vector<int64_t>& activeFilters);
    int preferredHeight(int width) const;
    void destroy();

private:
    struct Chip {
        Tag tag;
        RECT bounds{};
    };

    static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM w, LPARAM l);
    static int dpiFor(HWND window);
    int px(int value) const;
    int textHeight() const;
    int chipRowHeight() const;
    int textWidth(const std::wstring& value) const;
    bool isActive(int64_t id) const;
    int hitTest(POINT point) const;
    RECT viewport() const;
    void ensureFocusVisible();
    void rebuildLayout();
    void updateScrollBar(int maxScroll, int page);
    void scrollTo(int value);
    void activate(int index);
    void paint(HDC dc);

    HWND owner_{};
    HINSTANCE instance_{};
    HFONT font_{};
    std::wstring context_;
    std::vector<Chip> chips_;
    std::vector<int64_t> activeFilters_;
    int scrollOffset_{};
    int maxScroll_{};
    int focusIndex_{-1};
    int pressedIndex_{-1};
    int wheelRemainder_{};
};
}
