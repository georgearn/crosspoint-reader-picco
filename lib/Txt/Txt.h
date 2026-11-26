#pragma once

#include <Print.h>

#include <memory>
#include <string>
#include <string_view>

class BookMetadataCache;

class Txt {
 public:
  // True for every format converted to HTML and read as a single-spine book: TXT, MD and FB2.
  static bool isTxtOrMd(std::string_view path);
  static bool isFb2(std::string_view path);
  static bool validateCache(const std::string& filepath, const std::string& cachePath, size_t cachedSize);
  static void invalidateCache(const std::string& cachePath);
  static bool streamTxtToHtml(const std::string& filepath, Print& out);
  static std::string findCompanionCoverImage(const std::string& filepath);
  // Cover for the cover thumbnail: the image extracted from an FB2 at cache build, else the companion image.
  static std::string findCoverImage(const std::string& filepath, const std::string& cachePath);
  static bool convertCoverImageToBmp(const std::string& imagePath, const std::string& destBmpPath, int thumbHeight = 0,
                                     bool cropped = false, bool originalThresholds = false);
  static bool buildTxtCache(const std::string& filepath, const std::string& cachePath,
                            std::unique_ptr<BookMetadataCache>& bookMetadataCache);
};
