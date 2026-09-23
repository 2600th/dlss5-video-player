#pragma once

#include <algorithm>
#include <cstddef>
#include <span>
#include <vector>

// Where the groups of the keyboard cheat sheet go. Groups are never split: a
// menu's shortcuts read as that menu, and half of Playback at the bottom of
// one column and half at the top of the next is two half-menus.
//
// Groups flow down a column in menu order until the next would not fit, then
// start the next column, so the sheet uses as few columns as the height
// allows and reads in the same order as the menu bar.
namespace shortcut_sheet {

struct Metrics {
    int rowHeight{};
    int headerHeight{};
    int groupGap{};
    int columnWidth{};
    int columnGap{};
    int padding{};
    // Title above the columns and the closing hint below them.
    int titleHeight{};
    int footerHeight{};
};

struct GroupPlacement {
    int column{};
    int top{};   // relative to the top of the column area
    bool visible{true};
};

struct Layout {
    std::vector<GroupPlacement> groups;
    int columns{};
    int width{};
    int height{};
    int columnTop{};   // where the column area starts, below the title
};

inline int GroupHeight(size_t rows, const Metrics& metrics)
{
    return metrics.headerHeight + static_cast<int>(rows) * metrics.rowHeight;
}

// `maxWidth` and `maxHeight` are what the sheet may cover - the player's
// client area. A group that no column can hold, or that would need a column
// past `maxWidth`, is marked not visible rather than drawn off the sheet; the
// Help menu entry reaches the same content, so nothing is lost for good.
inline Layout LayoutSheet(std::span<const size_t> groupRows, const Metrics& metrics, int maxWidth, int maxHeight)
{
    Layout layout{};
    layout.columnTop = metrics.padding + metrics.titleHeight;
    const int available = std::max(0, maxHeight - layout.columnTop - metrics.footerHeight - metrics.padding);
    const int maxColumns = std::max(1, (maxWidth - 2 * metrics.padding + metrics.columnGap) /
                                           std::max(1, metrics.columnWidth + metrics.columnGap));
    int column = 0, y = 0, tallest = 0;
    for (const size_t rows : groupRows) {
        const int height = GroupHeight(rows, metrics);
        if (y > 0 && y + metrics.groupGap + height > available) { ++column; y = 0; }
        const int top = y == 0 ? 0 : y + metrics.groupGap;
        const bool visible = column < maxColumns && top + height <= available;
        layout.groups.push_back(GroupPlacement{column, top, visible});
        if (visible) {
            y = top + height;
            tallest = std::max(tallest, y);
            layout.columns = std::max(layout.columns, column + 1);
        } else if (y == 0) {
            // Taller than a whole column: skip it and keep the column free.
            continue;
        }
    }
    layout.columns = std::max(1, layout.columns);
    layout.width = 2 * metrics.padding + layout.columns * metrics.columnWidth + (layout.columns - 1) * metrics.columnGap;
    layout.height = layout.columnTop + tallest + metrics.footerHeight + metrics.padding;
    return layout;
}

} // namespace shortcut_sheet
