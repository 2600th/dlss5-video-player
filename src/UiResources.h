#pragma once

#include <windows.h>

enum class UiIcon {
    Open,
    Rewind,
    Play,
    Pause,
    Stop,
    FastForward,
    Volume,
    VolumeOff,
    Sparkles,
    Crop,
    Adjustments,
    Debug,
    Maximize,
    YouTube,
    Warning,
};

namespace ui_palette {

inline constexpr COLORREF Window = RGB(18, 19, 21);
inline constexpr COLORREF ControlSurface = RGB(27, 28, 31);
inline constexpr COLORREF Inactive = RGB(47, 49, 53);
inline constexpr COLORREF Hover = RGB(62, 65, 70);
inline constexpr COLORREF PrimaryBlue = RGB(55, 139, 226);
inline constexpr COLORREF PrimaryText = RGB(240, 240, 242);
inline constexpr COLORREF SecondaryText = RGB(160, 164, 172);

} // namespace ui_palette

struct ButtonState {
    bool enabled{true};
    bool active{false};
    bool hover{false};
    bool pressed{false};
    bool focus{false};
};

struct ButtonVisual {
    COLORREF fill{};
    COLORREF border{};
    COLORREF text{};
    bool drawFocus{false};
};

wchar_t GlyphForIcon(UiIcon icon);
ButtonVisual ResolveButtonVisual(ButtonState state);

class UiResources {
public:
    UiResources() = default;
    ~UiResources();

    UiResources(const UiResources&) = delete;
    UiResources& operator=(const UiResources&) = delete;

    bool Load(HINSTANCE instance);
    [[nodiscard]] bool IsLoaded() const { return m_fontResource != nullptr; }
    [[nodiscard]] HFONT CreateIconFont(UINT dpi) const;

private:
    HANDLE m_fontResource{};
};
