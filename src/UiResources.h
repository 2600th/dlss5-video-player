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
    Upscaling,
    FrameGeneration,
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
// The user's In/Out selection, and the part of a source that already has cached
// neural frames. The selection wins the loud colour: it is what gets acted on.
inline constexpr COLORREF MarkedRange = RGB(158, 112, 240);
inline constexpr COLORREF MarkedRangeEdge = RGB(206, 178, 255);
inline constexpr COLORREF NeuralCoverage = RGB(72, 196, 178);

} // namespace ui_palette

// `working` is the state this had no way to say. A feature pill has four:
// unavailable, off, on, and busy doing something that takes minutes - and with
// only `enabled` and `active` the last one looked exactly like the one before
// it. That matters most at the narrow widths where the toolbar drops to icons
// and the label, which is the only other thing carrying state, is gone.
//
// Precedence is unavailable, then working, then on, then off: a conversion
// that is running is the most important thing the pill can tell you, and a
// control you cannot use is the only thing that outranks it.
struct ButtonState {
    bool enabled{true};
    bool active{false};
    bool working{false};
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

enum class ButtonPresentation {
    IconAndLabel,
    LabelOnly,
};

wchar_t GlyphForIcon(UiIcon icon);
ButtonVisual ResolveButtonVisual(ButtonState state);
ButtonPresentation ResolveButtonPresentation(bool iconFontAvailable);

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
