#pragma once
#include <string>

namespace macassist {

// Resolves when a file was downloaded, in epoch seconds, writing it to
// *out_epoch and returning true. Returns false (leaving *out_epoch untouched)
// when the file carries no download signal.
//
// Source of truth is Spotlight's kMDItemDownloadedDate (durable); the
// com.apple.quarantine xattr is read first only as a fast path, since it is
// volatile -- cleared once a file is approved/opened and absent for anything
// downloaded before indexing began. The xattr value is
// `flags;hex-epoch;agent;uuid`; the timestamp is the second ';'-separated
// field, hex-encoded epoch seconds (not a plist date). Anything that doesn't
// parse falls through to Spotlight.
//
// Resolved on the extractor thread (off the writer/crawl hot path), once per
// content version: the Spotlight lookup is too costly to run synchronously
// during the stat-first crawl. The download date is immutable, so there is no
// need to re-read it on every stat. Never throws.
bool ReadDownloadDate(const std::string& path, long long* out_epoch);

}  // namespace macassist
