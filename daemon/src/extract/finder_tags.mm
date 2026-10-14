#include "extract/finder_tags.hpp"

#import <Foundation/Foundation.h>

#include <sys/xattr.h>

#include <vector>

#include <nlohmann/json.hpp>

namespace macassist {

std::string ReadFinderTagsJson(const std::string& path) {
  @autoreleasepool {
    static const char* kAttr = "com.apple.metadata:_kMDItemUserTags";

    // Finder stores tags as a binary-plist array of strings in an xattr.
    ssize_t len = getxattr(path.c_str(), kAttr, nullptr, 0, 0, 0);
    if (len <= 0) return "";  // no tags, or unreadable
    std::vector<char> buf(static_cast<size_t>(len));
    len = getxattr(path.c_str(), kAttr, buf.data(), buf.size(), 0, 0);
    if (len <= 0) return "";

    NSData* data = [NSData dataWithBytes:buf.data()
                                 length:static_cast<NSUInteger>(len)];
    id plist = [NSPropertyListSerialization propertyListWithData:data
                                                         options:NSPropertyListImmutable
                                                          format:nil
                                                           error:nil];
    if (![plist isKindOfClass:[NSArray class]]) return "";

    nlohmann::json arr = nlohmann::json::array();
    for (id entry in (NSArray*)plist) {
      if (![entry isKindOfClass:[NSString class]]) continue;
      NSString* s = (NSString*)entry;

      // Each entry is "Name\nColorIndex"; the "\nColorIndex" suffix is
      // optional. Split on the last newline and only treat the tail as a
      // color when it is a pure integer -- otherwise the whole string is the
      // name (a tag name could, in theory, contain a newline of its own).
      NSString* name = s;
      int color = 0;
      NSRange nl = [s rangeOfString:@"\n" options:NSBackwardsSearch];
      if (nl.location != NSNotFound) {
        NSString* tail = [s substringFromIndex:nl.location + 1];
        NSScanner* sc = [NSScanner scannerWithString:tail];
        int c = 0;
        if ([sc scanInt:&c] && [sc isAtEnd]) {
          color = c;
          name = [s substringToIndex:nl.location];
        }
      }

      const char* nm = [name UTF8String];
      if (nm == nullptr) continue;
      arr.push_back({{"name", std::string(nm)}, {"color", color}});
    }

    if (arr.empty()) return "";
    return arr.dump();
  }
}

}  // namespace macassist
