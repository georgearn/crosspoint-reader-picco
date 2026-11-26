#include "LyraCarouselTheme.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Memory.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int kPad = LyraCarouselMetrics::kFramePad;  // frame around the focused cover
constexpr int kCornerRadius = 6;

// Fan geometry of one side cover, counted outwards from the focused cover.
// The inner edge (towards the focus) is taller than the outer edge, which is
// what sells the perspective.
struct SideGeometry {
  int gap;      // space between the previous cover's outer edge (or the selection frame) and this one
  int width;    // projected width
  int hInner;   // height at the inner edge
  int hOuter;   // height at the outer edge
  int fadePct;  // lightening towards white, pushes far covers into the background
};

// Decoded cover thumb, 2 bits per pixel (0 = black .. 3 = white).
struct CoverPixels {
  int width = 0;
  int height = 0;
  int stride = 0;
  std::unique_ptr<uint8_t[]> data;

  uint8_t at(const int x, const int y) const {
    return (data[static_cast<size_t>(y) * stride + (x >> 2)] >> (6 - ((x & 3) * 2))) & 0x3;
  }
};

bool loadCover(const std::string& coverPath, const int thumbHeight, CoverPixels& out) {
  if (coverPath.empty()) return false;
  HalFile file;
  if (!Storage.openFileForRead("HOME", UITheme::getCoverThumbPath(coverPath, thumbHeight), file)) return false;

  bool ok = false;
  {
    Bitmap bitmap(file);
    if (bitmap.parseHeaders() == BmpReaderError::Ok && bitmap.getWidth() > 0 && bitmap.getHeight() > 0) {
      const int w = bitmap.getWidth();
      const int h = bitmap.getHeight();
      const int stride = (w + 3) / 4;
      auto pixels = makeUniqueNoThrow<uint8_t[]>(static_cast<size_t>(stride) * h);
      auto scratch = makeUniqueNoThrow<uint8_t[]>(static_cast<size_t>(bitmap.getRowBytes()));
      if (pixels && scratch) {
        ok = true;
        for (int i = 0; i < h && ok; ++i) {
          // Rows arrive in file order; bottom-up BMPs list the last display row first.
          const int row = bitmap.isTopDown() ? i : h - 1 - i;
          ok =
              bitmap.readNextRow(pixels.get() + static_cast<size_t>(row) * stride, scratch.get()) == BmpReaderError::Ok;
        }
        if (ok) {
          out.width = w;
          out.height = h;
          out.stride = stride;
          out.data = std::move(pixels);
        }
      }
    }
  }
  file.close();
  return ok;
}

// Mean luminance 0..255 of the source box [x0,x1) x [y0,y1); outside the image counts as white.
int boxLuma(const CoverPixels& px, const int x0, const int x1, const int y0, const int y1) {
  int sum = 0;
  int count = 0;
  for (int y = y0; y < y1; ++y) {
    for (int x = x0; x < x1; ++x) {
      sum += (x < 0 || y < 0 || x >= px.width || y >= px.height) ? 3 : px.at(x, y);
      ++count;
    }
  }
  return count > 0 ? sum * 85 / count : 255;
}

// 4x4 Bayer matrix: turns a luminance back into a B/W dither pattern after downscaling.
constexpr uint8_t kBayer4[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
inline bool isDark(const int x, const int y, const int luma) { return luma < kBayer4[y & 3][x & 3] * 16 + 8; }

// Draws one fan-shaped side cover. `xStart` is the leftmost column; `innerOnLeft`
// says which edge faces the focused cover. `px` may be null (no cover available).
void drawSideCover(const GfxRenderer& renderer, const CoverPixels* px, const int xStart, const bool innerOnLeft,
                   const int yCenter, const SideGeometry& g, const int slotW, const int slotH) {
  if (g.width < 2) return;
  const int ox = px ? (px->width - slotW) / 2 : 0;
  const int oy = px ? (px->height - slotH) / 2 : 0;

  for (int i = 0; i < g.width; ++i) {  // i counts from the inner edge
    const int x = innerOnLeft ? xStart + i : xStart + g.width - 1 - i;
    const int h = g.hInner + (g.hOuter - g.hInner) * i / (g.width - 1);
    const int top = yCenter - h / 2;

    // The slot maps left-to-right onto the projected width whichever side the cover is on.
    const int col = innerOnLeft ? i : g.width - 1 - i;
    const int sx0 = ox + col * slotW / g.width;
    const int sx1 = ox + std::max(col * slotW / g.width + 1, ((col + 1) * slotW + g.width - 1) / g.width);

    for (int j = 0; j < h; ++j) {
      int luma = 170;  // flat mid-grey for a missing cover
      if (px) {
        const int sy0 = oy + j * slotH / h;
        const int sy1 = oy + std::max(j * slotH / h + 1, ((j + 1) * slotH + h - 1) / h);
        luma = boxLuma(*px, sx0, sx1, sy0, sy1);
      }
      luma += (255 - luma) * g.fadePct / 100;
      renderer.drawPixel(x, top + j, isDark(x, top + j, luma));
    }
    // Keep the cover positive when the whole display runs inverted (night mode).
    if (px) renderer.preserveImagePolarity(x, top, 1, h);
  }

  const int xIn = innerOnLeft ? xStart : xStart + g.width - 1;
  const int xOut = innerOnLeft ? xStart + g.width - 1 : xStart;
  const int topIn = yCenter - g.hInner / 2;
  const int topOut = yCenter - g.hOuter / 2;
  renderer.drawLine(xIn, topIn, xOut, topOut, true);
  renderer.drawLine(xIn, topIn + g.hInner - 1, xOut, topOut + g.hOuter - 1, true);
  renderer.drawLine(xIn, topIn, xIn, topIn + g.hInner - 1, true);
  renderer.drawLine(xOut, topOut, xOut, topOut + g.hOuter - 1, true);
}
}  // namespace

void LyraCarouselTheme::drawRecentBookCover(GfxRenderer& renderer, Rect rect,
                                            const std::vector<RecentBook>& recentBooks, const int selectorIndex,
                                            bool& coverRendered, bool& coverBufferStored, bool& /*bufferRestored*/,
                                            std::function<bool()> storeCoverBuffer, int progress) const {
  if (recentBooks.empty()) {
    drawEmptyRecents(renderer, rect);
    return;
  }

  const int bookCount = static_cast<int>(recentBooks.size());
  const int coverH = LyraCarouselMetrics::values.homeCoverHeight;
  const int coverW = coverH * 3 / 5;  // the 0.6 aspect slot Lyra uses for thumbs
  const int cx = rect.x + rect.width / 2;
  const int coverX = cx - coverW / 2;
  const int coverY = rect.y + LyraCarouselMetrics::kTitleBlock + kPad;

  if (selectorIndex >= 0 && selectorIndex < bookCount) focusedBook_ = selectorIndex;
  focusedBook_ = std::clamp(focusedBook_, 0, bookCount - 1);
  const int focus = focusedBook_;
  const bool selected = selectorIndex == focus;

  // The cached snapshot holds the carousel for one focus; a new focus needs a redraw.
  if (drawnFocus_ != focus) coverRendered = false;

  if (!coverRendered) {
    // The restored snapshot (if any) shows the previous focus: wipe it first.
    renderer.fillRect(rect.x, rect.y, rect.width, rect.height, false);

    const auto clip = renderer.getClipRect();
    renderer.setClipRect(std::max(rect.x, clip[0]), std::max(rect.y, clip[1]),
                         std::max(0, std::min(rect.x + rect.width, clip[0] + clip[2]) - std::max(rect.x, clip[0])),
                         std::max(0, std::min(rect.y + rect.height, clip[1] + clip[3]) - std::max(rect.y, clip[1])));

    // Side covers, outermost first so nearer ones overlap the farther edge, never the reverse.
    const int yCenter = coverY + coverH / 2;
    SideGeometry side[LyraCarouselMetrics::kVisibleSideCovers];
    {
      // Share the room left of the frame: the near cover gets most of it, the far one tapers to the edge.
      const int avail = std::max(0, rect.width / 2 - (coverW / 2 + kPad));
      constexpr int gap = 4;
      const int usable = std::max(0, avail - 2 * gap);
      const int nearW = usable * 62 / 100;
      side[0] = {gap, nearW, coverH * 88 / 100, coverH * 76 / 100, 0};
      side[1] = {gap, usable - nearW, coverH * 74 / 100, coverH * 62 / 100, 28};
    }
    for (int step = LyraCarouselMetrics::kVisibleSideCovers; step >= 1; --step) {
      for (const int dir : {-1, 1}) {
        const int idx = focus + dir * step;
        if (idx < 0 || idx >= bookCount) continue;

        const SideGeometry& g = side[step - 1];
        // Covers sit edge to edge outwards from the selection frame.
        int xStart = 0;
        if (dir > 0) {
          xStart = coverX + coverW + kPad + side[0].gap;
          if (step == 2) xStart += side[0].width + side[1].gap;
        } else {
          xStart = coverX - kPad - side[0].gap - side[0].width;
          if (step == 2) xStart -= side[1].gap + side[1].width;
        }
        CoverPixels px;
        const bool hasPixels = loadCover(recentBooks[idx].coverBmpPath, coverH, px);
        drawSideCover(renderer, hasPixels ? &px : nullptr, xStart, dir > 0, yCenter, g, coverW, coverH);
      }
    }
    renderer.setClipRect(clip[0], clip[1], clip[2], clip[3]);

    // Focused cover, drawn 1:1 like the other Lyra themes (no rescaling of dithered art).
    bool hasCover = false;
    if (!recentBooks[focus].coverBmpPath.empty()) {
      HalFile file;
      if (Storage.openFileForRead("HOME", UITheme::getCoverThumbPath(recentBooks[focus].coverBmpPath, coverH), file)) {
        Bitmap bitmap(file);
        if (bitmap.parseHeaders() == BmpReaderError::Ok) {
          drawCoverThumbFill(renderer, bitmap, Rect{coverX, coverY, coverW, coverH});
          hasCover = true;
        }
        file.close();
      }
    }
    renderer.drawRect(coverX, coverY, coverW, coverH, true);
    if (!hasCover) drawCoverPlaceholder(renderer, Rect{coverX, coverY, coverW, coverH});

    coverBufferStored = storeCoverBuffer();
    coverRendered = coverBufferStored;
    drawnFocus_ = coverRendered ? focus : -1;
  }

  // Selection frame, redrawn every frame on top of the snapshot.
  if (selected) {
    const int boxX = coverX - kPad;
    const int boxW = coverW + 2 * kPad;
    renderer.fillRoundedRect(boxX, coverY - kPad, boxW, kPad, kCornerRadius, true, true, false, false,
                             Color::LightGray);
    renderer.fillRectDither(boxX, coverY, kPad, coverH, Color::LightGray);
    renderer.fillRectDither(boxX + boxW - kPad, coverY, kPad, coverH, Color::LightGray);
    renderer.fillRoundedRect(boxX, coverY + coverH, boxW, kPad, kCornerRadius, false, false, true, true,
                             Color::LightGray);
  }

  // Title above the focused cover, page dots and author/progress below it.
  const RecentBook& book = recentBooks[focus];
  const int maxTextWidth = rect.width - 2 * LyraCarouselMetrics::values.contentSidePadding;
  const int titleLineH = renderer.getLineHeight(UI_12_FONT_ID);
  const int smallLineH = renderer.getLineHeight(UI_10_FONT_ID);
  const int bottom = rect.y + rect.height;

  const int maxTitleLines = std::max(1, std::min(2, LyraCarouselMetrics::kTitleBlock / std::max(1, titleLineH)));
  const auto titleLines =
      renderer.wrappedText(UI_12_FONT_ID, book.title.c_str(), maxTextWidth, maxTitleLines, EpdFontFamily::BOLD);
  // Bottom-aligned so a one-line title stays next to the cover.
  int y = rect.y + LyraCarouselMetrics::kTitleBlock - static_cast<int>(titleLines.size()) * titleLineH;
  for (const auto& line : titleLines) {
    const int w = renderer.getTextWidth(UI_12_FONT_ID, line.c_str(), EpdFontFamily::BOLD);
    renderer.drawText(UI_12_FONT_ID, cx - w / 2, y, line.c_str(), true, EpdFontFamily::BOLD);
    y += titleLineH;
  }

  // One dot per book, the focused one filled.
  constexpr int dotSpacing = 14;
  const int dotsY = coverY + coverH + kPad + 8;
  const int dotsLeft = cx - (bookCount - 1) * dotSpacing / 2;
  for (int i = 0; i < bookCount; ++i) {
    const int dx = dotsLeft + i * dotSpacing;
    if (i == focus) {
      renderer.fillRect(dx - 3, dotsY - 3, 7, 7, true);
    } else {
      renderer.drawRect(dx - 2, dotsY - 2, 5, 5, true);
    }
  }

  std::string sub = book.author;
  if (focus == 0 && progress >= 0) {
    char pct[8];
    snprintf(pct, sizeof(pct), "%d%%", progress);
    sub = sub.empty() ? std::string(pct) : sub + " - " + pct;
  }
  const int subY = coverY + coverH + kPad + LyraCarouselMetrics::kDotsBlock;
  if (!sub.empty() && subY + smallLineH <= bottom) {
    const std::string fitted = renderer.truncatedText(UI_10_FONT_ID, sub.c_str(), maxTextWidth);
    const int w = renderer.getTextWidth(UI_10_FONT_ID, fitted.c_str());
    renderer.drawText(UI_10_FONT_ID, cx - w / 2, subY, fitted.c_str(), true);
  }
}

void LyraCarouselTheme::drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                                       const std::function<std::string(int index)>& buttonLabel,
                                       const std::function<UIIcon(int index)>& rowIcon) const {
  if (buttonCount <= 0) return;
  constexpr int iconSize = 32;
  const int cellW = rect.width / buttonCount;
  const int rowH = LyraCarouselMetrics::kMenuRowHeight;
  const int labelH = renderer.getLineHeight(SMALL_FONT_ID);
  for (int i = 0; i < buttonCount; ++i) {
    const int cellX = rect.x + i * cellW;
    if (i == selectedIndex) {
      renderer.fillRoundedRect(cellX + 2, rect.y, cellW - 4, rowH, kCornerRadius, Color::LightGray);
    }
    const int blockH = iconSize + 6 + labelH;
    const int top = rect.y + (rowH - blockH) / 2;
    if (rowIcon != nullptr) {
      if (const uint8_t* bitmap = menuIconBitmap(rowIcon(i))) {
        renderer.drawIcon(bitmap, cellX + (cellW - iconSize) / 2, top, iconSize);
      }
    }
    const std::string label = renderer.truncatedText(SMALL_FONT_ID, buttonLabel(i).c_str(), cellW - 6);
    const int w = renderer.getTextWidth(SMALL_FONT_ID, label.c_str());
    renderer.drawText(SMALL_FONT_ID, cellX + (cellW - w) / 2, top + iconSize + 6, label.c_str(), true);
  }
}
