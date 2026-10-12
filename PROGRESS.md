# MacAssist — Progress Tracker

*Living status doc. Read alongside `PLAN.md` (the authoritative design). Last updated 2026-10-12.*

MacAssist is a "smarter Finder" for macOS: a background daemon keeps a rich index of the
user's files; a Spotlight-style ⌥Space panel answers natural-language queries in under ~2s.
See `PLAN.md` for the full design and rationale.

---

## Status at a glance

| Phase | Scope | Status |
|---|---|---|
| **Phase 1** | Vertical slice: hotkey → panel → daemon → filename FTS → open | ✅ Done (on `main`) |
| **Phase 2** | Real indexing: live sweeper + content extraction | 🚧 In progress — bricks 1–3 of 6 done |
| **Phase 3** | Intelligence: AFM `SearchIntent` + CoreML embeddings + two-stage query | ⬜ Not started |
| **Phase 4** | Product polish: settings, onboarding, launchd, signing/notarization/DMG | ⬜ Not started |
| **Phase 5** | Stretch: voice, transcription, chunked embeddings, auto-open | ⬜ Not started |

**Current branch:** `phase2-indexing` (pushed to `origin`). **Schema version:** v3.

---

## Facets / subsystems

### Daemon — `macassistd` (C++20, CMake)
| Module | Responsibility | Status |
|---|---|---|
| `ipc/socket_server` | Unix-socket accept loop, length-prefixed JSON frames | ✅ works (single-threaded, one conn at a time) |
| `ipc/request_router` | Routes `ping`/`status`/`query`; reads live sweep status | ✅ (`action`/`reindex`/`config` ⬜ not implemented) |
| `index/database` | SQLite open, WAL, schema+migrations, connection roles, meta get/set | ✅ schema v3, writer/reader split |
| `sweep/crawler` | Full incremental walk (upsert + fingerprint-skip + tombstone) | ✅ |
| `sweep/indexer` | Shared per-file upsert / tombstone (cached statements) | ✅ |
| `sweep/writer` | Long-lived writer thread: owns RW conn, runs crawl, drains command queue | ✅ |
| `sweep/fs_watcher` | FSEvents live updates (ObjC++/`.mm`, GCD dispatch queue) | ✅ |
| `query/query_engine` | FTS5 BM25 over name + path tokens; excludes tombstones | ✅ (content/tags cols empty until brick 5) |
| `extract/` | ObjC++ bridges: PDFKit text, media tags, (later OCR) | ⬜ brick 5 |
| `ai/` | Swift interop: AFM session, CoreML embedder | ⬜ Phase 3 |

### App — `MacAssist.app` (Swift, SwiftPM)
Menu-bar resident, global ⌥Space hotkey, non-activating blurred `NSPanel`, debounced live
search, reveal-in-Finder / show-all-as-folder actions. ✅ **Phase 1 complete.** Settings UI,
onboarding, Full Disk Access flow ⬜ Phase 4.

### `mactl` (CLI, speaks the socket protocol)
`status`, `query <text>` work. ✅ Primary dev/test harness.

### Build & distribution
CMake (daemon + `mactl`) + SwiftPM (app), via `scripts/build.sh`. `.app` bundling via
`scripts/make-app.sh`. ⬜ launchd agent, code signing, notarization, DMG (Phase 4). ⬜ Xcode
workspace migration (Phase 3, when Swift↔C++ interop forces it).

---

## What we've done

### Phase 1 (on `main`) — vertical slice
End-to-end: ⌥Space → type → ranked filename results → reveal in Finder / open a folder of all
hits. Daemon did a one-shot stat-only crawl at startup; name-only FTS. (See git history.)

### Phase 2 so far (branch `phase2-indexing`)
- **Brick 1 — schema v2 + connection-per-role** (`a465e26`). `Database::Open(path, Access)`:
  one read-write connection (writer) + one read-only connection (query path); WAL (not a
  lock) coordinates them. Version-gated migration (pre-release = nuke-and-rebuild). New
  tables `roots`, `exclusions`; `files` gains `root_id`, `missing`.
- **Brick 2 — background writer + incremental crawl** (`bb8d47f`). Crawl moved off startup
  onto a long-lived `Writer` thread. Destroy-and-rebuild replaced by path-keyed upsert with a
  monotonic generation counter: new→insert, changed (by fingerprint)→update, unchanged→skip;
  unseen rows tombstoned (`missing=1`). Real `status` (sweeping + progress via atomics).
  Schema v3 adds `seen_gen`; `crawl_gen` in `meta`.
- **Brick 3 — FSEvents live updates + command queue** (`06db3bb`). `FsWatcher` (first ObjC++
  bridge) streams changes on a GCD queue; the writer drains a `WriteCommand` queue (its first
  off-thread producer). Idempotent "re-examine path" semantics (stat→upsert or tombstone);
  drop/overflow→full rescan. `lastEventId` persisted in `meta` for replay. Shared `Indexer`
  extracted so the crawl and FSEvents share one copy of the upsert SQL.

**Verified** (ad-hoc, via `mactl` + `sqlite3`): fresh crawl indexes all; a re-crawl re-indexes
only changed/new files and tombstones deletes; and with the daemon **running**, live
create/delete/rename/modify reflect in query results within ~2s.

---

## What's next

### Phase 2 remaining
- **Brick 4 — reconciliation + politeness.** Periodic full re-walk (nightly) as the
  FSEvents-drop backstop — reuses `CrawlIncremental` on the writer thread. Thermal/battery
  pausing (`NSProcessInfo.thermalState` + power source), QoS-utility threads. **Open decision**
  deferred here: quick-restart FSEvents replay (via `fsevents_last_id`) vs. always-full-crawl
  at startup.
- **Brick 5 — cheap extraction.** Worker pool (second queue producer) + ObjC++ bridges for
  PDF text (PDFKit), plain text, and media tags (AVFoundation/EXIF/ID3) → fill the empty
  `content`/`tags` FTS columns + `file_meta`. Honor PLAN §3 caps. **Vision OCR and office-doc
  (docx/pages) parsing are deferred to Phase 2.5** (OCR is the thermal/latency risk).
- **Brick 6 — fixture corpus + tests.** Turn the ad-hoc shell checks into a real
  Catch2/GoogleTest suite + a golden-query set.

### Beyond Phase 2
- **Phase 3 (intelligence):** Swift interop layer, AFM `SearchIntent` guided generation
  (prewarmed session), CoreML embeddings + `sqlite-vec`, two-stage progressive query + rank
  fusion. This is when the Xcode-workspace migration and the Stage-B/query-cache decisions in
  PLAN §4 come into play.
- **Phase 4 (polish):** settings UI, onboarding + FDA flow, launchd install/upgrade, signing +
  notarization + DMG.

---

## How to build & run

```bash
./scripts/build.sh          # daemon + mactl (CMake) and the app (SwiftPM)
./scripts/make-app.sh       # (re)build the MacAssist.app bundle

# Against real files (defaults: ~/Library/Application Support/MacAssist/{daemon.sock,index.db}
# and the six home roots):
./build/daemon/macassistd

# Dev loop against a throwaway corpus — keep the socket path short (< ~104 chars):
./build/daemon/macassistd --socket /tmp/dev.sock --db /tmp/dev.db --root /path/to/corpus
./build/tools/mactl/mactl --socket /tmp/dev.sock status
./build/tools/mactl/mactl --socket /tmp/dev.sock query resume
```

---

## Known caveats
- **Socket path ≤ ~104 chars** (`sockaddr_un` limit) — dev sockets must live in `/tmp`, not the
  long scratchpad path.
- **Menu-bar icon can hide behind the notch** on crowded menu bars (environmental, not a bug).
- **OCR + office docs deferred to Phase 2.5** — scanned/image-only PDFs won't have searchable
  text until then.
- **No launchd agent yet** — the daemon is launched manually (Phase 4).
