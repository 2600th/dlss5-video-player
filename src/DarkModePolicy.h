#pragma once

#include <windows.h>

#include <cstddef>

#include "UiResources.h"

// The dark chrome around the player: its menu bar and its dialogs.
//
// The title bar has been dark since DWMWA_USE_IMMERSIVE_DARK_MODE; the menu
// bar and every dialog stayed light Win32 classics beside it. This is the one
// place the colours for both are decided, and the one place the undocumented
// menu-bar messages are named.
namespace dark_mode {

// ---- Dialog colours --------------------------------------------------------
//
// The player's own palette (ui_palette in UiResources.h), not a second one:
// the dialog background is the window's, an edit or a list sits on the
// control surface, as a toolbar button does, and text is the same two greys.
inline constexpr COLORREF DialogBackground = ui_palette::Window;
inline constexpr COLORREF FieldBackground = ui_palette::ControlSurface;
inline constexpr COLORREF Text = ui_palette::PrimaryText;
inline constexpr COLORREF QuietText = ui_palette::SecondaryText;
// A refusal in a dialog (an invalid URL or timecode). The light dialogs drew
// it RGB(180, 36, 36), which is unreadable on this background.
inline constexpr COLORREF ErrorText = RGB(255, 128, 118);
inline constexpr COLORREF Rule = RGB(62, 65, 70);

// ---- The menu bar ------------------------------------------------------------
//
// UNDOCUMENTED. user32 asks the window to draw its menu bar with these two
// messages before drawing it itself (the "UAH" menu drawing uxtheme added in
// Windows 10), and a window that answers them owns the bar's pixels. The IDs
// and the structures below are not in any Windows SDK header; they are the
// layout the community's dark-mode work (win32-darkmode, Notepad++, and
// others) has used unchanged since Windows 10 1809, restated here once.
//
// Guarded three ways so a Windows that changes them costs a light menu bar and
// nothing else: the sizes are pinned below for x64, the handler checks the
// menu handle it is given against the window's own before trusting anything
// else in the structure, and a failed check latches the handler off for the
// life of the window.
inline constexpr UINT WM_UAHDRAWMENU = 0x0091;
inline constexpr UINT WM_UAHDRAWMENUITEM = 0x0092;

struct UAHMENU {
    HMENU hmenu;
    HDC hdc;
    DWORD dwFlags;
};

union UAHMENUITEMMETRICS {
    struct { DWORD cx; DWORD cy; } rgsizeBar[2];
    struct { DWORD cx; DWORD cy; } rgsizePopup[4];
};

struct UAHMENUPOPUPMETRICS {
    DWORD rgcx[4];
    DWORD fUpdateMaxWidths : 2;
};

struct UAHMENUITEM {
    int iPosition;
    UAHMENUITEMMETRICS umim;
    UAHMENUPOPUPMETRICS umpm;
};

struct UAHDRAWMENUITEM {
    DRAWITEMSTRUCT dis;
    UAHMENU um;
    UAHMENUITEM umi;
};

#if defined(_WIN64)
static_assert(sizeof(UAHMENU) == 24, "UAHMENU layout changed");
static_assert(sizeof(UAHMENUITEM) == 56, "UAHMENUITEM layout changed");
static_assert(offsetof(UAHDRAWMENUITEM, um) == sizeof(DRAWITEMSTRUCT), "UAHDRAWMENUITEM layout changed");
#endif

struct MenuItemColors {
    COLORREF fill{};
    COLORREF text{};
};

// A top-level item at rest sits on the bar with no fill of its own; hot (the
// pointer or keyboard is on it) and open (its popup is showing) lift it to the
// hover surface the toolbar buttons use. Disabled reads as the quiet grey.
inline MenuItemColors MenuBarItemColors(UINT itemState)
{
    const bool disabled = (itemState & (ODS_GRAYED | ODS_DISABLED | ODS_INACTIVE)) != 0;
    const bool lifted = (itemState & (ODS_HOTLIGHT | ODS_SELECTED)) != 0;
    return MenuItemColors{lifted && !disabled ? ui_palette::Hover : DialogBackground,
                          disabled ? QuietText : Text};
}

// DT flags for an item's text: centred on one line, with the mnemonic
// underline hidden unless the keyboard asked for it, as the light bar does.
inline UINT MenuBarTextFormat(UINT itemState)
{
    return DT_CENTER | DT_VCENTER | DT_SINGLELINE | ((itemState & ODS_NOACCEL) ? DT_HIDEPREFIX : 0u);
}

// The height a dialog font is given at `dpi`: Segoe UI at 9 pt, Windows'
// own dialog font, which is 12 px at 96 dpi. DEFAULT_GUI_FONT was the same
// size at 96 dpi and the same size at every other dpi too, which is why the
// dialogs were tiny at 200%.
inline int DialogFontHeight(UINT dpi)
{
    return -MulDiv(12, static_cast<int>(dpi == 0 ? USER_DEFAULT_SCREEN_DPI : dpi), USER_DEFAULT_SCREEN_DPI);
}

// A child's rectangle in its parent's client, carried from one dpi to another
// the way Windows scales the window around it. Rounded per edge, so two
// controls that met at one dpi still meet at the next.
inline RECT ScaleRectForDpi(RECT rect, UINT from, UINT to)
{
    const int f = static_cast<int>(from == 0 ? USER_DEFAULT_SCREEN_DPI : from);
    const int t = static_cast<int>(to == 0 ? USER_DEFAULT_SCREEN_DPI : to);
    return RECT{MulDiv(rect.left, t, f), MulDiv(rect.top, t, f), MulDiv(rect.right, t, f), MulDiv(rect.bottom, t, f)};
}

} // namespace dark_mode
