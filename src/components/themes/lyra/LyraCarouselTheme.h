#pragma once

#include "components/themes/lyra/LyraTheme.h"

class GfxRenderer;

// Lyra with a perspective cover carousel on the home screen: the focused book
// sits centered at full size, neighbours fan out to both sides and shrink
// towards the screen edges. Everything except the home cover area is Lyra.
namespace LyraCarouselMetrics {
constexpr int kVisibleSideCovers = 2;  // covers drawn on each side of the focus
constexpr int kCoverHeight = 400;      // focused cover; width is 3/5 of this
constexpr int kFramePad = 8;           // selection frame around the focused cover
constexpr int kTitleBlock = 64;        // title lines above the cover
constexpr int kDotsBlock = 18;         // page dots under the cover
constexpr int kSubBlock = 30;          // author / progress line
constexpr int kMenuRowHeight = 92;     // horizontal icon row under the carousel
constexpr ThemeMetrics values = [] {
  ThemeMetrics v = LyraMetrics::values;
  v.homeCoverHeight = kCoverHeight;
  v.homeCoverTileHeight = kTitleBlock + kCoverHeight + 2 * kFramePad + kDotsBlock + kSubBlock;
  v.homeMenuTopOffset = 12;
  v.menuRowHeight = kMenuRowHeight;
  v.homeRecentBooksCount = 2 * kVisibleSideCovers + 1;
  return v;
}();

// Which cover a touch hits, from its horizontal offset to the tile centre: 0 is the focused cover, -2..2 the
// covers around it. Mirrors the widths LyraCarouselTheme::drawRecentBookCover gives each cover.
constexpr int coverStepAt(const int dx, const int contentWidth) {
  const int frameHalf = kCoverHeight * 3 / 10 + kFramePad;
  const int dist = dx < 0 ? -dx : dx;
  if (dist <= frameHalf) return 0;
  const int usable = contentWidth / 2 - frameHalf - 8;
  const int nearEnd = frameHalf + 4 + (usable > 0 ? usable * 62 / 100 : 0) + 4;
  const int step = dist <= nearEnd ? 1 : 2;
  return dx < 0 ? -step : step;
}
}  // namespace LyraCarouselMetrics

class LyraCarouselTheme : public LyraTheme {
 public:
  void drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                           const int selectorIndex, bool& coverRendered, bool& coverBufferStored, bool& bufferRestored,
                           std::function<bool()> storeCoverBuffer, int progress) const override;
  // Icons side by side in one row (label under each icon) instead of Lyra's vertical list.
  void drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                      const std::function<std::string(int index)>& buttonLabel,
                      const std::function<UIIcon(int index)>& rowIcon) const override;

 private:
  // Stays on the last selected book while the selection is in the menu below,
  // so the carousel does not jump around.
  mutable int focusedBook_ = 0;
  mutable int drawnFocus_ = -1;  // focus the cached cover snapshot was drawn for
};
