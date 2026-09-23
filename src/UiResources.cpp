#include "UiResources.h"

#include "resources.h"

#include <algorithm>

wchar_t GlyphForIcon(UiIcon icon)
{
    // @tabler/icons-webfont 3.46.0. Keep these values aligned with the
    // committed assets/tabler/tabler-icons.css source of truth.
    switch (icon) {
    case UiIcon::Open: return L'\xfaf7';             // folder-open
    case UiIcon::Rewind: return L'\xfaba';           // rewind-backward-10
    case UiIcon::Play: return L'\xed46';             // player-play
    case UiIcon::Pause: return L'\xed45';            // player-pause
    case UiIcon::Stop: return L'\xed4a';             // player-stop
    case UiIcon::FastForward: return L'\xfac2';      // rewind-forward-10
    case UiIcon::Volume: return L'\xeb51';           // volume
    case UiIcon::VolumeOff: return L'\xf1c3';        // volume-off
    case UiIcon::Sparkles: return L'\xf6d7';         // sparkles
    // arrows-maximize: four arrows pushing outward from a centre, which is what
    // Super Resolution does to a picture. Verified present in the committed
    // tabler-icons.ttf cmap (U+EA28, glyph 341).
    //
    // It must not be sparkles. The comment on FrameGeneration below already
    // established why - the feature pills collapse to icon-only at 44/36 dip -
    // and then left Neural Rendering and DLSS Upscaling BOTH on sparkles, so
    // two of the three were still indistinguishable at exactly the width the
    // note was written about. Sparkles stays with the neural model, which is
    // the one that earns it.
    case UiIcon::Upscaling: return L'\xea28';        // arrows-maximize
    // copy-plus: a duplicated frame with a plus, the one glyph in the embedded
    // 3.46.0 set that reads as "more frames out of one frame". Verified present
    // in the committed tabler-icons.ttf cmap (U+FDAE, 15 contours), so it needs
    // no fallback. Picked over keyframes (U+F585, a timeline key row - an
    // editing concept, not a frame count) and multiplier-2x (U+EF44, which
    // states a fixed 2x while the planned multiple is measured per source).
    // It must not be sparkles: the three feature pills collapse to icon-only
    // at 44/36 dip, where three identical sparkles hid which one starts a
    // minutes-long conversion.
    case UiIcon::FrameGeneration: return L'\xfdae'; // copy-plus
    case UiIcon::Crop: return L'\xea85';             // crop
    case UiIcon::Adjustments: return L'\xea03';      // adjustments
    case UiIcon::Debug: return L'\xea48';            // bug
    case UiIcon::Maximize: return L'\xeaea';         // maximize
    case UiIcon::YouTube: return L'\xec90';          // brand-youtube
    case UiIcon::Warning: return L'\xea06';          // alert-triangle
    // layout-columns: two panes side by side, which is what the compare
    // button on the taskbar thumbnail turns on. Verified present in the
    // committed tabler-icons.ttf cmap (U+EAD4, 9 contours).
    case UiIcon::Compare: return L'\xead4';          // layout-columns
    }
    return L'\0';
}

ButtonVisual ResolveButtonVisual(ButtonState state)
{
    if (!state.enabled) {
        return {ui_palette::ControlSurface, ui_palette::Inactive,
                ui_palette::SecondaryText, state.focus};
    }
    if (state.pressed) {
        return {ui_palette::ControlSurface, ui_palette::PrimaryBlue,
                ui_palette::PrimaryText, state.focus};
    }
    // Teal rather than blue, and the same teal the timeline already uses for
    // rendered coverage: the two places the player says "this is being made
    // right now" should not say it in two different colours.
    if (state.working) {
        return {ui_palette::NeuralCoverage, RGB(120, 226, 208),
                ui_palette::Window, state.focus};
    }
    if (state.active) {
        return {ui_palette::PrimaryBlue, RGB(103, 179, 245),
                ui_palette::Window, state.focus};
    }
    if (state.hover) {
        return {ui_palette::Hover, RGB(93, 97, 104),
                ui_palette::PrimaryText, state.focus};
    }
    return {ui_palette::Inactive, RGB(75, 78, 84),
            ui_palette::PrimaryText, state.focus};
}

ButtonPresentation ResolveButtonPresentation(bool iconFontAvailable)
{
    return iconFontAvailable ? ButtonPresentation::IconAndLabel : ButtonPresentation::LabelOnly;
}

UiResources::~UiResources()
{
    if (m_fontResource) RemoveFontMemResourceEx(m_fontResource);
}

bool UiResources::Load(HINSTANCE instance)
{
    if (m_fontResource) return true;
    if (!instance) return false;

    const HRSRC resource = FindResourceW(instance, MAKEINTRESOURCEW(IDR_TABLER_ICONS_FONT), RT_RCDATA);
    if (!resource) return false;
    const HGLOBAL loaded = LoadResource(instance, resource);
    if (!loaded) return false;
    const DWORD size = SizeofResource(instance, resource);
    const void* bytes = LockResource(loaded);
    if (!bytes || size == 0) return false;

    DWORD fontsAdded = 0;
    m_fontResource = AddFontMemResourceEx(const_cast<void*>(bytes), size, nullptr, &fontsAdded);
    if (!m_fontResource || fontsAdded == 0) {
        if (m_fontResource) RemoveFontMemResourceEx(m_fontResource);
        m_fontResource = nullptr;
        return false;
    }
    return true;
}

HFONT UiResources::CreateIconFont(UINT dpi) const
{
    if (!m_fontResource) return nullptr;
    const int logicalHeight = -MulDiv(17, static_cast<int>(dpi == 0 ? USER_DEFAULT_SCREEN_DPI : dpi),
                                      USER_DEFAULT_SCREEN_DPI);
    return CreateFontW(logicalHeight, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"tabler-icons");
}
