#include "extract/text_extractors.hpp"

#import <AVFoundation/AVFoundation.h>
#import <Foundation/Foundation.h>

#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>

#include <cmath>

namespace macassist {
namespace {

// Record a typed key/value. When `searchable`, the value is also appended to
// the flattened `tags` blob so FTS can match it (e.g. an artist name). Numbers
// like dimensions/duration go to file_meta only -- they aren't search terms.
void AddMeta(ExtractResult& out, const std::string& key, const std::string& val,
            bool searchable) {
  if (val.empty()) return;
  out.meta.emplace_back(key, val);
  if (searchable) {
    if (!out.tags.empty()) out.tags.push_back(' ');
    out.tags += val;
  }
}

std::string CFToStd(CFStringRef s) {
  if (s == nullptr) return "";
  char buf[512];
  if (CFStringGetCString(s, buf, sizeof(buf), kCFStringEncodingUTF8)) {
    return buf;
  }
  return "";
}

// Read a CFNumber image property as an integer, formatted as a decimal string.
std::string NumProp(CFDictionaryRef d, CFStringRef key) {
  CFTypeRef v = d ? CFDictionaryGetValue(d, key) : nullptr;
  if (v == nullptr || CFGetTypeID(v) != CFNumberGetTypeID()) return "";
  long long n = 0;
  if (!CFNumberGetValue(static_cast<CFNumberRef>(v), kCFNumberLongLongType, &n)) {
    return "";
  }
  return std::to_string(n);
}

std::string StrProp(CFDictionaryRef d, CFStringRef key) {
  CFTypeRef v = d ? CFDictionaryGetValue(d, key) : nullptr;
  if (v == nullptr || CFGetTypeID(v) != CFStringGetTypeID()) return "";
  return CFToStd(static_cast<CFStringRef>(v));
}

void ExtractImage(const std::string& path, ExtractResult& out) {
  CFStringRef p =
      CFStringCreateWithCString(nullptr, path.c_str(), kCFStringEncodingUTF8);
  if (p == nullptr) return;
  CFURLRef url =
      CFURLCreateWithFileSystemPath(nullptr, p, kCFURLPOSIXPathStyle, false);
  CFRelease(p);
  if (url == nullptr) return;
  CGImageSourceRef src = CGImageSourceCreateWithURL(url, nullptr);
  CFRelease(url);
  if (src == nullptr) return;

  CFDictionaryRef props = CGImageSourceCopyPropertiesAtIndex(src, 0, nullptr);
  if (props != nullptr) {
    AddMeta(out, "width", NumProp(props, kCGImagePropertyPixelWidth), false);
    AddMeta(out, "height", NumProp(props, kCGImagePropertyPixelHeight), false);
    auto tiff = static_cast<CFDictionaryRef>(
        CFDictionaryGetValue(props, kCGImagePropertyTIFFDictionary));
    if (tiff != nullptr) {
      AddMeta(out, "make", StrProp(tiff, kCGImagePropertyTIFFMake), true);
      AddMeta(out, "model", StrProp(tiff, kCGImagePropertyTIFFModel), true);
    }
    CFRelease(props);
  }
  CFRelease(src);
}

void ExtractAV(const std::string& path, ExtractResult& out) {
  @autoreleasepool {
    NSString* ns = [NSString stringWithUTF8String:path.c_str()];
    if (ns == nil) return;
    AVURLAsset* asset =
        [AVURLAsset URLAssetWithURL:[NSURL fileURLWithPath:ns] options:nil];
    if (asset == nil) return;

    const Float64 secs = CMTimeGetSeconds(asset.duration);
    if (std::isfinite(secs) && secs > 0) {
      AddMeta(out, "duration", std::to_string(llround(secs)), false);
    }
    // Common metadata is the format-agnostic view (ID3, iTunes, QuickTime all
    // map into it). Synchronous access is fine on this background thread.
    for (AVMetadataItem* item in asset.commonMetadata) {
      NSString* key = item.commonKey;
      NSString* val = item.stringValue;
      if (key == nil || val.length == 0) continue;
      const char* ck = [key UTF8String];
      const char* cv = [val UTF8String];
      if (ck == nullptr || cv == nullptr) continue;
      const std::string k = ck;
      if (k == "title" || k == "artist" || k == "albumName" ||
          k == "creator" || k == "type") {
        AddMeta(out, k, cv, true);
      }
    }
  }
}

}  // namespace

void ExtractMediaTags(const std::string& path, const std::string& kind,
                      ExtractResult& out) {
  if (kind == "image") {
    ExtractImage(path, out);
  } else if (kind == "audio" || kind == "video") {
    ExtractAV(path, out);
  }
}

}  // namespace macassist
