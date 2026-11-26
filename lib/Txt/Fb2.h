#pragma once

#include <Print.h>

#include <cstdint>
#include <string>
#include <vector>

// FictionBook 2 (.fb2) reader support. The book is converted to HTML on the fly and read through the same
// single-spine pipeline as TXT/MD; see Txt.cpp.
class Fb2 {
 public:
  struct TocItem {
    std::string title;
    std::string anchor;  // id of the section's <div> in the generated HTML
    uint8_t level;
  };

  struct Info {
    std::string title;
    std::string author;
    std::string language;
    std::string coverPath;  // cover image extracted into the cache directory, empty if none
    std::vector<TocItem> toc;
  };

  // Pass 1, run once when the cache is built: metadata, table of contents, and the embedded cover (saved as
  // cover.jpg / cover.png in cachePath). A malformed file yields whatever was parsed before the error.
  static bool scan(const std::string& filepath, const std::string& cachePath, Info& info);

  // Pass 2: stream the book body as HTML. Section ids match the anchors reported by scan().
  static bool streamHtml(const std::string& filepath, const char* versionTag, Print& out);

  // Cover extracted by scan(), or an empty string.
  static std::string findCachedCover(const std::string& cachePath);
};
