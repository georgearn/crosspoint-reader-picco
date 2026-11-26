#include "Fb2.h"

#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <XmlParserUtils.h>
#include <strings.h>

#include <cstring>

namespace {

// windows-1251 is the usual non-UTF-8 encoding of Russian FB2 files; expat only knows UTF-8/16 and Latin-1.
constexpr uint16_t kCp1251Upper[64] = {
    0x0402, 0x0403, 0x201A, 0x0453, 0x201E, 0x2026, 0x2020, 0x2021, 0x20AC, 0x2030, 0x0409, 0x2039, 0x040A,
    0x040C, 0x040B, 0x040F, 0x0452, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x0000, 0x2122,
    0x0459, 0x203A, 0x045A, 0x045C, 0x045B, 0x045F, 0x00A0, 0x040E, 0x045E, 0x0408, 0x00A4, 0x0490, 0x00A6,
    0x00A7, 0x0401, 0x00A9, 0x0404, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x0407, 0x00B0, 0x00B1, 0x0406, 0x0456,
    0x0491, 0x00B5, 0x00B6, 0x00B7, 0x0451, 0x2116, 0x0454, 0x00BB, 0x0458, 0x0405, 0x0455, 0x0457};

int XMLCALL unknownEncoding(void*, const XML_Char* name, XML_Encoding* info) {
  if (strcasecmp(name, "windows-1251") != 0 && strcasecmp(name, "cp1251") != 0) return XML_STATUS_ERROR;
  for (int i = 0; i < 128; ++i) info->map[i] = i;
  for (int i = 0; i < 64; ++i) info->map[0x80 + i] = kCp1251Upper[i] != 0 ? kCp1251Upper[i] : -1;
  for (int i = 0; i < 64; ++i) info->map[0xC0 + i] = 0x0410 + i;
  info->data = nullptr;
  info->convert = nullptr;
  info->release = nullptr;
  return XML_STATUS_OK;
}

constexpr int kMaxDepth = 64;
constexpr int kMaxSectionDepth = 32;
constexpr size_t kReadChunk = 4096;
constexpr size_t kFlushSize = 4096;

constexpr const char* kHeadOpen[6] = {"<h1>", "<h2>", "<h3>", "<h4>", "<h5>", "<h6>"};
constexpr const char* kHeadClose[6] = {"</h1>", "</h2>", "</h3>", "</h4>", "</h5>", "</h6>"};

enum class Mode : uint8_t { Scan, Emit };

enum Kind : uint8_t {
  K_OTHER,
  K_DESCRIPTION,
  K_TITLE_INFO,
  K_CAPTURE,  // text goes to Parser::cap
  K_AUTHOR,
  K_COVERPAGE,
  K_BINARY,
  K_BODY,
  K_SECTION,
  K_TITLE,
  K_TEXT,  // element whose character data is book text
};

struct Frame {
  Kind kind;
  const char* close;
};

int base64Value(const char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+' || c == '-') return 62;
  if (c == '/' || c == '_') return 63;
  return -1;
}

std::string collapseSpace(const std::string& in) {
  std::string out;
  out.reserve(in.size());
  bool pendingSpace = false;
  for (const char c : in) {
    if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
      pendingSpace = !out.empty();
    } else {
      if (pendingSpace) out.push_back(' ');
      pendingSpace = false;
      out.push_back(c);
    }
  }
  return out;
}

const char* attrValue(const char** atts, const char* key) {
  for (int i = 0; atts[i]; i += 2) {
    if (strcmp(atts[i], key) == 0) return atts[i + 1];
  }
  return nullptr;
}

// href, l:href and xlink:href all count.
const char* hrefValue(const char** atts) {
  for (int i = 0; atts[i]; i += 2) {
    if (strcmp(xmlLocalName(atts[i]), "href") == 0) return atts[i + 1];
  }
  return nullptr;
}

class Fb2Parser {
 public:
  Fb2Parser(const Mode mode, Print* out, const std::string* cachePath, Fb2::Info* info)
      : mode(mode), out(out), cachePath(cachePath), info(info) {}

  bool run(HalFile& src);

  void writeHeader(const char* versionTag, const std::string& stem) {
    put("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n");
    put(versionTag);
    put("\n<!DOCTYPE html>\n<html>\n<head><title>");
    putEscaped(stem.data(), stem.size());
    put("</title></head>\n<body>\n");
  }

  void writeFooter() {
    put("\n</body>\n</html>\n");
    flush();
  }

  bool outputOk = true;

 private:
  const Mode mode;
  Print* const out;
  const std::string* const cachePath;
  Fb2::Info* const info;

  Frame stack[kMaxDepth];
  int depth = 0;
  int overflow = 0;  // elements opened beyond kMaxDepth

  bool inDescription = false;
  bool inTitleInfo = false;
  bool titleInfoSeen = false;
  bool inCoverpage = false;
  bool inBinary = false;
  int bodyDepth = 0;
  bool notesBody = false;
  int textDepth = 0;
  int sectionDepth = 0;
  int sectionCount = 0;
  std::string sectionAnchorNames[kMaxSectionDepth];
  bool inTitle = false;
  int titleParagraphs = 0;
  std::string titleText;

  std::string* cap = nullptr;
  std::string first, middle, last, nick;
  std::string coverId;

  HalFile coverFile;
  bool coverOpen = false;
  std::string coverTmpPath;
  uint32_t b64Acc = 0;
  int b64Bits = 0;
  uint8_t coverBuf[256];
  size_t coverBufLen = 0;
  size_t coverBytes = 0;

  std::string outBuf;

  void put(const char* s) {
    if (mode != Mode::Emit || !s) return;
    outBuf.append(s);
    maybeFlush();
  }

  void putEscaped(const char* s, const size_t len) {
    if (mode != Mode::Emit) return;
    for (size_t i = 0; i < len; ++i) {
      const char c = s[i];
      switch (c) {
        case '&':
          outBuf.append("&amp;");
          break;
        case '<':
          outBuf.append("&lt;");
          break;
        case '>':
          outBuf.append("&gt;");
          break;
        case '"':
          outBuf.append("&quot;");
          break;
        default:
          if (static_cast<unsigned char>(c) >= 0x20 || c == '\n' || c == '\t') outBuf.push_back(c);
          break;
      }
    }
    maybeFlush();
  }

  void maybeFlush() {
    if (outBuf.size() >= kFlushSize) flush();
  }

  void flush() {
    if (outBuf.empty() || !out) return;
    const size_t written = out->write(reinterpret_cast<const uint8_t*>(outBuf.data()), outBuf.size());
    outputOk = outputOk && written == outBuf.size();
    outBuf.clear();
  }

  void push(const Kind kind, const char* close) {
    if (depth < kMaxDepth) {
      stack[depth++] = Frame{kind, close};
    } else {
      ++overflow;
    }
  }

  void startBody(const char* name, const char** atts);
  void startDescription(const char* name, const char** atts);
  void startBinary(const char** atts);
  void endFrame(const Frame& f);
  void decodeBase64(const char* s, int len);
  void finishCover();
  void flushCoverBuf();
  static const char* coverExtension(const char* type, const char* id);

  static void XMLCALL onStart(void* ud, const XML_Char* name, const XML_Char** atts) {
    static_cast<Fb2Parser*>(ud)->start(name, atts);
  }
  static void XMLCALL onEnd(void* ud, const XML_Char*) { static_cast<Fb2Parser*>(ud)->end(); }
  static void XMLCALL onChars(void* ud, const XML_Char* s, int len) { static_cast<Fb2Parser*>(ud)->chars(s, len); }

  void start(const char* name, const char** atts);
  void end();
  void chars(const char* s, int len);
  void closeOpenFrames();
};

void Fb2Parser::start(const char* name, const char** atts) {
  if (inBinary || overflow > 0) {
    push(K_OTHER, nullptr);
    return;
  }
  if (bodyDepth > 0) {
    startBody(name, atts);
  } else if (strcmp(name, "body") == 0) {
    ++bodyDepth;
    const char* bodyName = attrValue(atts, "name");
    notesBody = bodyName && strcmp(bodyName, "notes") == 0;
    put("<div>");
    push(K_BODY, "</div>");
  } else if (strcmp(name, "binary") == 0) {
    startBinary(atts);
  } else {
    startDescription(name, atts);
  }
}

void Fb2Parser::startDescription(const char* name, const char** atts) {
  if (strcmp(name, "description") == 0) {
    inDescription = true;
    push(K_DESCRIPTION, nullptr);
    return;
  }
  if (!inDescription) {
    push(K_OTHER, nullptr);
    return;
  }
  if (!inTitleInfo) {
    // The first <title-info> describes the book; <src-title-info> and <document-info> describe other things.
    if (strcmp(name, "title-info") == 0 && !titleInfoSeen) {
      inTitleInfo = true;
      titleInfoSeen = true;
      push(K_TITLE_INFO, nullptr);
    } else {
      push(K_OTHER, nullptr);
    }
    return;
  }
  if (strcmp(name, "book-title") == 0) {
    cap = &info->title;
    push(K_CAPTURE, nullptr);
  } else if (strcmp(name, "lang") == 0) {
    cap = &info->language;
    push(K_CAPTURE, nullptr);
  } else if (strcmp(name, "author") == 0) {
    first.clear();
    middle.clear();
    last.clear();
    nick.clear();
    push(K_AUTHOR, nullptr);
  } else if (strcmp(name, "first-name") == 0 || strcmp(name, "middle-name") == 0 || strcmp(name, "last-name") == 0 ||
             strcmp(name, "nickname") == 0) {
    cap = name[0] == 'f' ? &first : name[0] == 'm' ? &middle : name[0] == 'l' ? &last : &nick;
    push(K_CAPTURE, nullptr);
  } else if (strcmp(name, "coverpage") == 0) {
    inCoverpage = true;
    push(K_COVERPAGE, nullptr);
  } else if (inCoverpage && strcmp(name, "image") == 0) {
    const char* href = hrefValue(atts);
    if (href && coverId.empty()) coverId = href[0] == '#' ? href + 1 : href;
    push(K_OTHER, nullptr);
  } else {
    push(K_OTHER, nullptr);
  }
}

const char* Fb2Parser::coverExtension(const char* type, const char* id) {
  if (type && (strstr(type, "jpeg") || strstr(type, "jpg"))) return ".jpg";
  if (type && strstr(type, "png")) return ".png";
  const size_t n = id ? strlen(id) : 0;
  if (n > 4 && strcasecmp(id + n - 4, ".jpg") == 0) return ".jpg";
  if (n > 5 && strcasecmp(id + n - 5, ".jpeg") == 0) return ".jpg";
  if (n > 4 && strcasecmp(id + n - 4, ".png") == 0) return ".png";
  return nullptr;
}

void Fb2Parser::startBinary(const char** atts) {
  push(K_BINARY, nullptr);
  inBinary = true;
  if (mode != Mode::Scan || !cachePath || coverId.empty() || !info->coverPath.empty()) return;
  const char* id = attrValue(atts, "id");
  if (!id || coverId != id) return;
  const char* ext = coverExtension(attrValue(atts, "content-type"), id);
  if (!ext) return;
  const std::string path = *cachePath + "/cover" + ext;
  if (!Storage.openFileForWrite("FB2", path, coverFile)) {
    LOG_ERR("FB2", "Could not open cover for writing: %s", path.c_str());
    return;
  }
  coverOpen = true;
  coverTmpPath = path;
  b64Acc = 0;
  b64Bits = 0;
  coverBufLen = 0;
  coverBytes = 0;
}

void Fb2Parser::decodeBase64(const char* s, const int len) {
  for (int i = 0; i < len; ++i) {
    const int v = base64Value(s[i]);
    if (v < 0) continue;  // whitespace and '=' padding
    b64Acc = (b64Acc << 6) | static_cast<uint32_t>(v);
    b64Bits += 6;
    if (b64Bits >= 8) {
      b64Bits -= 8;
      coverBuf[coverBufLen++] = static_cast<uint8_t>((b64Acc >> b64Bits) & 0xFF);
      b64Acc &= (1u << b64Bits) - 1;
      if (coverBufLen == sizeof(coverBuf)) flushCoverBuf();
    }
  }
}

void Fb2Parser::flushCoverBuf() {
  if (coverBufLen == 0) return;
  if (coverFile.write(coverBuf, coverBufLen) != coverBufLen) {
    LOG_ERR("FB2", "Cover write failed");
    coverBytes = 0;
    coverOpen = false;
    coverFile.close();
    Storage.remove(coverTmpPath.c_str());
  } else {
    coverBytes += coverBufLen;
  }
  coverBufLen = 0;
}

void Fb2Parser::finishCover() {
  if (!coverOpen) return;
  flushCoverBuf();
  if (!coverOpen) return;  // write error already cleaned up
  coverFile.close();
  coverOpen = false;
  if (coverBytes > 0) {
    info->coverPath = coverTmpPath;
  } else {
    Storage.remove(coverTmpPath.c_str());
  }
}

void Fb2Parser::startBody(const char* name, const char** atts) {
  const char* idAttr = attrValue(atts, "id");

  if (strcmp(name, "section") == 0) {
    ++sectionCount;
    std::string anchor = idAttr && idAttr[0] ? std::string(idAttr) : "s" + std::to_string(sectionCount);
    if (sectionDepth < kMaxSectionDepth) sectionAnchorNames[sectionDepth] = anchor;
    ++sectionDepth;
    put("<div id=\"");
    putEscaped(anchor.data(), anchor.size());
    put("\">");
    push(K_SECTION, "</div>");
  } else if (strcmp(name, "title") == 0) {
    const int level = sectionDepth == 0 ? 0 : (sectionDepth < 5 ? sectionDepth : 5);
    inTitle = true;
    titleParagraphs = 0;
    titleText.clear();
    put(kHeadOpen[level]);
    push(K_TITLE, kHeadClose[level]);
  } else if (strcmp(name, "p") == 0) {
    if (inTitle) {
      if (titleParagraphs++ > 0) {
        put("<br />");
        titleText.push_back(' ');
      }
      ++textDepth;
      push(K_TEXT, nullptr);
    } else {
      if (idAttr && idAttr[0]) {
        put("<p id=\"");
        putEscaped(idAttr, strlen(idAttr));
        put("\">");
      } else {
        put("<p>");
      }
      ++textDepth;
      push(K_TEXT, "</p>");
    }
  } else if (strcmp(name, "v") == 0) {
    put("<p>");
    ++textDepth;
    push(K_TEXT, "</p>");
  } else if (strcmp(name, "subtitle") == 0) {
    put("<p><strong>");
    ++textDepth;
    push(K_TEXT, "</strong></p>");
  } else if (strcmp(name, "text-author") == 0) {
    put("<p><em>");
    ++textDepth;
    push(K_TEXT, "</em></p>");
  } else if (strcmp(name, "epigraph") == 0 || strcmp(name, "cite") == 0) {
    put("<blockquote>");
    push(K_OTHER, "</blockquote>");
  } else if (strcmp(name, "poem") == 0 || strcmp(name, "stanza") == 0 || strcmp(name, "annotation") == 0 ||
             strcmp(name, "table") == 0) {
    put("<div>");
    push(K_OTHER, "</div>");
  } else if (strcmp(name, "emphasis") == 0) {
    put("<em>");
    push(K_OTHER, "</em>");
  } else if (strcmp(name, "strong") == 0) {
    put("<strong>");
    push(K_OTHER, "</strong>");
  } else if (strcmp(name, "strikethrough") == 0) {
    put("<s>");
    push(K_OTHER, "</s>");
  } else if (strcmp(name, "sub") == 0) {
    put("<sub>");
    push(K_OTHER, "</sub>");
  } else if (strcmp(name, "sup") == 0) {
    put("<sup>");
    push(K_OTHER, "</sup>");
  } else if (strcmp(name, "code") == 0) {
    put("<code>");
    push(K_OTHER, "</code>");
  } else if (strcmp(name, "style") == 0) {
    put("<span>");
    push(K_OTHER, "</span>");
  } else if (strcmp(name, "a") == 0) {
    const char* href = hrefValue(atts);
    if (href) {
      put("<a href=\"");
      putEscaped(href, strlen(href));
      put("\">");
    } else {
      put("<a>");
    }
    push(K_OTHER, "</a>");
  } else if (strcmp(name, "empty-line") == 0) {
    put("<br />");
    push(K_OTHER, nullptr);
  } else if (strcmp(name, "tr") == 0) {
    put("<p>");
    push(K_OTHER, "</p>");
  } else if (strcmp(name, "td") == 0 || strcmp(name, "th") == 0) {
    ++textDepth;
    push(K_TEXT, " ");
  } else {
    push(K_OTHER, nullptr);  // image, nested body markup we do not render
  }
}

void Fb2Parser::end() {
  if (overflow > 0) {
    --overflow;
    return;
  }
  if (depth == 0) return;
  const Frame f = stack[--depth];
  endFrame(f);
}

void Fb2Parser::endFrame(const Frame& f) {
  switch (f.kind) {
    case K_DESCRIPTION:
      inDescription = false;
      // Title page ahead of the first section.
      if (mode == Mode::Emit && !info->title.empty()) {
        put("<h1>");
        putEscaped(info->title.data(), info->title.size());
        put("</h1>");
        if (!info->author.empty()) {
          put("<p>");
          putEscaped(info->author.data(), info->author.size());
          put("</p>");
        }
      }
      break;
    case K_TITLE_INFO:
      inTitleInfo = false;
      break;
    case K_CAPTURE:
      cap = nullptr;
      break;
    case K_AUTHOR: {
      std::string name;
      for (const std::string* part : {&first, &middle, &last}) {
        const std::string trimmed = collapseSpace(*part);
        if (trimmed.empty()) continue;
        if (!name.empty()) name.push_back(' ');
        name += trimmed;
      }
      if (name.empty()) name = collapseSpace(nick);
      if (!name.empty()) {
        if (!info->author.empty()) info->author += ", ";
        info->author += name;
      }
      break;
    }
    case K_COVERPAGE:
      inCoverpage = false;
      break;
    case K_BINARY:
      inBinary = false;
      finishCover();
      break;
    case K_BODY:
      --bodyDepth;
      if (bodyDepth == 0) notesBody = false;
      break;
    case K_SECTION:
      --sectionDepth;
      break;
    case K_TITLE: {
      inTitle = false;
      if (mode == Mode::Scan && sectionDepth > 0 && !notesBody && sectionDepth <= kMaxSectionDepth) {
        std::string title = collapseSpace(titleText);
        if (!title.empty()) {
          info->toc.push_back(Fb2::TocItem{std::move(title), sectionAnchorNames[sectionDepth - 1],
                                           static_cast<uint8_t>(sectionDepth - 1)});
        }
      }
      break;
    }
    case K_TEXT:
      --textDepth;
      break;
    default:
      break;
  }
  put(f.close);
}

void Fb2Parser::chars(const char* s, const int len) {
  if (cap) {
    cap->append(s, len);
    return;
  }
  if (inBinary) {
    if (coverOpen) decodeBase64(s, len);
    return;
  }
  if (bodyDepth == 0 || textDepth == 0) return;
  if (mode == Mode::Scan) {
    if (inTitle) titleText.append(s, len);
    return;
  }
  putEscaped(s, len);
}

// A truncated or malformed file ends mid-element: close what is open so the HTML stays well formed.
void Fb2Parser::closeOpenFrames() {
  while (depth > 0) {
    const Frame f = stack[--depth];
    put(f.close);
    if (f.kind == K_BINARY) finishCover();
  }
}

bool Fb2Parser::run(HalFile& src) {
  XML_Parser parser = XML_ParserCreate(nullptr);
  if (!parser) {
    LOG_ERR("FB2", "Could not allocate XML parser");
    return false;
  }
  XML_SetUserData(parser, this);
  XML_SetElementHandler(parser, onStart, onEnd);
  XML_SetCharacterDataHandler(parser, onChars);
  XML_SetUnknownEncodingHandler(parser, unknownEncoding, nullptr);

  bool ok = true;
  for (;;) {
    void* buf = XML_GetBuffer(parser, kReadChunk);
    if (!buf) {
      LOG_ERR("FB2", "Could not allocate parse buffer");
      ok = false;
      break;
    }
    const int n = src.read(buf, kReadChunk);
    if (n < 0) {
      LOG_ERR("FB2", "Read error");
      ok = false;
      break;
    }
    if (XML_ParseBuffer(parser, n, n == 0) == XML_STATUS_ERROR) {
      // Keep what was parsed: a book cut short by one bad byte is still readable.
      LOG_ERR("FB2", "Parse error at line %lu: %s", static_cast<unsigned long>(XML_GetCurrentLineNumber(parser)),
              XML_ErrorString(XML_GetErrorCode(parser)));
      break;
    }
    if (n == 0) break;
  }
  destroyXmlParser(parser);
  closeOpenFrames();
  flush();
  return ok;
}

}  // namespace

bool Fb2::scan(const std::string& filepath, const std::string& cachePath, Info& info) {
  HalFile src;
  if (!Storage.openFileForRead("FB2", filepath, src)) {
    LOG_ERR("FB2", "Failed to open %s", filepath.c_str());
    return false;
  }
  auto parser = makeUniqueNoThrow<Fb2Parser>(Mode::Scan, nullptr, &cachePath, &info);
  if (!parser) {
    LOG_ERR("FB2", "OOM: FB2 scan parser");
    return false;
  }
  const bool ok = parser->run(src);
  src.close();
  info.title = collapseSpace(info.title);
  info.language = collapseSpace(info.language);
  return ok;
}

bool Fb2::streamHtml(const std::string& filepath, const char* versionTag, Print& out) {
  HalFile src;
  if (!Storage.openFileForRead("FB2", filepath, src)) {
    LOG_ERR("FB2", "Failed to open %s", filepath.c_str());
    return false;
  }
  Info info;
  auto parser = makeUniqueNoThrow<Fb2Parser>(Mode::Emit, &out, nullptr, &info);
  if (!parser) {
    LOG_ERR("FB2", "OOM: FB2 stream parser");
    return false;
  }
  parser->writeHeader(versionTag, FsHelpers::getFileNameWithoutExtension(filepath));
  const bool ok = parser->run(src);
  parser->writeFooter();
  src.close();
  return ok && parser->outputOk;
}

std::string Fb2::findCachedCover(const std::string& cachePath) {
  for (const char* ext : {".jpg", ".png"}) {
    const std::string path = cachePath + "/cover" + ext;
    if (Storage.exists(path.c_str())) return path;
  }
  return "";
}
