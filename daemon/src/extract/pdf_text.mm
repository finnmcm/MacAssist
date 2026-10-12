#include "extract/text_extractors.hpp"

#import <Foundation/Foundation.h>
#import <PDFKit/PDFKit.h>

namespace macassist {

// Concatenate the text layer of a PDF, stopping at 100 KB (PLAN §3) or 200
// pages. Scanned/image-only PDFs yield nothing here -- OCR is Phase 2.5.
void ExtractPdfText(const std::string& path, ExtractResult& out) {
  @autoreleasepool {
    NSString* ns = [NSString stringWithUTF8String:path.c_str()];
    if (ns == nil) return;
    PDFDocument* doc = [[PDFDocument alloc] initWithURL:[NSURL fileURLWithPath:ns]];
    if (doc == nil) return;
    // Encrypted-but-unlocked (empty password) PDFs are readable; only a
    // still-locked document can't be read, so skip just those.
    if (doc.isLocked) return;

    constexpr size_t kCap = 100 * 1024;
    const NSUInteger kMaxPages = 200;
    NSUInteger pages = doc.pageCount;
    if (pages > kMaxPages) pages = kMaxPages;

    std::string body;
    for (NSUInteger i = 0; i < pages; ++i) {
      PDFPage* page = [doc pageAtIndex:i];
      NSString* text = page.string;  // nil/empty for image-only pages
      if (text.length == 0) continue;
      const char* c = [text UTF8String];
      if (c == nullptr) continue;
      body.append(c);
      body.push_back('\n');
      if (body.size() >= kCap) {
        body.resize(kCap);
        break;
      }
    }
    out.content = std::move(body);
  }
}

}  // namespace macassist
