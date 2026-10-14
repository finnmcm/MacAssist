#include "extract/download_date.hpp"

#import <CoreServices/CoreServices.h>
#import <Foundation/Foundation.h>

#include <sys/xattr.h>

#include <cstdlib>
#include <string>
#include <vector>

namespace macassist {
namespace {

// Fast path: parse the hex epoch out of the com.apple.quarantine xattr value
// (`flags;hex-epoch;agent;uuid`). Returns true and sets *out on success.
bool FromQuarantine(const std::string& path, long long* out) {
  static const char* kAttr = "com.apple.quarantine";
  ssize_t len = getxattr(path.c_str(), kAttr, nullptr, 0, 0, 0);
  if (len <= 0) return false;
  std::vector<char> buf(static_cast<size_t>(len));
  len = getxattr(path.c_str(), kAttr, buf.data(), buf.size(), 0, 0);
  if (len <= 0) return false;

  const std::string value(buf.data(), static_cast<size_t>(len));
  // Field 1 (zero-based) is the hex timestamp.
  std::size_t first = value.find(';');
  if (first == std::string::npos) return false;
  std::size_t second = value.find(';', first + 1);
  const std::string hex =
      value.substr(first + 1, (second == std::string::npos)
                                  ? std::string::npos
                                  : second - (first + 1));
  if (hex.empty()) return false;

  char* end = nullptr;
  const long long epoch = std::strtoll(hex.c_str(), &end, 16);
  if (end == hex.c_str() || *end != '\0' || epoch <= 0) return false;
  *out = epoch;
  return true;
}

// Durable source of truth: Spotlight's kMDItemDownloadedDate. The value is an
// NSDate or an array of them; take the earliest. Returns true on success.
bool FromSpotlight(const std::string& path, long long* out) {
  @autoreleasepool {
    NSString* ns = [NSString stringWithUTF8String:path.c_str()];
    if (ns == nil) return false;
    MDItemRef item = MDItemCreate(nullptr, (__bridge CFStringRef)ns);
    if (item == nullptr) return false;

    // Not always declared as a public constant; name it by string.
    CFTypeRef raw =
        MDItemCopyAttribute(item, CFSTR("kMDItemDownloadedDate"));
    CFRelease(item);
    if (raw == nullptr) return false;

    id value = (__bridge_transfer id)raw;
    NSDate* date = nil;
    if ([value isKindOfClass:[NSDate class]]) {
      date = (NSDate*)value;
    } else if ([value isKindOfClass:[NSArray class]]) {
      for (id entry in (NSArray*)value) {
        if (![entry isKindOfClass:[NSDate class]]) continue;
        if (date == nil || [(NSDate*)entry compare:date] == NSOrderedAscending) {
          date = (NSDate*)entry;
        }
      }
    }
    if (date == nil) return false;

    const NSTimeInterval secs = [date timeIntervalSince1970];
    if (secs <= 0) return false;
    *out = static_cast<long long>(secs);
    return true;
  }
}

}  // namespace

bool ReadDownloadDate(const std::string& path, long long* out_epoch) {
  long long epoch = 0;
  if (FromQuarantine(path, &epoch) || FromSpotlight(path, &epoch)) {
    *out_epoch = epoch;
    return true;
  }
  return false;
}

}  // namespace macassist
