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
    case UiIcon::Crop: return L'\xea85';             // crop
    case UiIcon::Adjustments: return L'\xea03';      // adjustments
    case UiIcon::Debug: return L'\xea48';            // bug
    case UiIcon::Maximize: return L'\xeaea';         // maximize
    case UiIcon::YouTube: return L'\xec90';          // brand-youtube
    case UiIcon::Warning: return L'\xea06';          // alert-triangle
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
