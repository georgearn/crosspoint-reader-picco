#pragma once

#include "components/themes/lyra/LyraTheme.h"

class GfxRenderer;

// Lyra with a perspective cover carousel on the home screen: the focused book
// sits centered at full size, neighbours fan out to both sides and shrink
// towards the screen edges. Everything except the home cover area is Lyra.
namespace LyraCarouselMetrics {
constexpr int kVisibleSideCovers = 2;  // covers drawn on each side of the focus
constexpr ThemeMetrics values = [] {
  ThemeMetrics v = LyraMetrics::values;
  v.homeCoverTileHeight = 300;
  v.homeRecentBooksCount = 2 * kVisibleSideCovers + 1;
  return v;
}();
}  // namespace LyraCarouselMetrics

class LyraCarouselTheme : public LyraTheme {
 public:
  void drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                           const int selectorIndex, bool& coverRendered, bool& coverBufferStored, bool& bufferRestored,
                           std::function<bool()> storeCoverBuffer, int progress) const override;

 private:
  // Stays on the last selected book while the selection is in the menu below,
  // so the carousel does not jump around.
  mutable int focusedBook_ = 0;
  mutable int drawnFocus_ = -1;  // focus the cached cover snapshot was drawn for
};
