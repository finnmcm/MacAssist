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
| **Phase 2** | Real indexing: live sweeper + content extraction | ✅ Done — bricks 1–6 of 6 (test suite landed) |
| **Phase 3** | Intelligence: AFM `SearchIntent` + CoreML embeddings + two-stage query | ⬜ Not started |
| **Phase 4** | Product polish: settings, onboarding, launchd, signing/notarization/DMG | ⬜ Not started |
| **Phase 5** | Stretch: voice, transcription, chunked embeddings, auto-open | ⬜ Not started |

**Current branch:** `main`. **Schema version:** v5.

---

## Facets / subsystems

### Daemon — `macassistd` (C++20, CMake)
| Module | Responsibility | Status |
|---|---|---|
| `ipc/socket_server` | Unix-socket accept loop, length-prefixed JSON frames | ✅ works (single-threaded, one conn at a time) |
| `ipc/request_router` | Routes `ping`/`status`/`query`; reads live sweep status | ✅ (`action`/`reindex`/`config` ⬜ not implemented) |
| `index/database` | SQLite open, WAL, schema+migrations, connection roles, meta get/set | ✅ schema v5, writer/reader split |
| `sweep/crawler` | Full incremental walk (upsert + fingerprint-skip + tombstone) | ✅ |
| `sweep/indexer` | Shared per-file upsert / tombstone (cached statements) | ✅ |
| `sweep/writer` | Long-lived writer thread: owns RW conn, runs crawl + periodic reconciliation, drains command queue | ✅ |
| `sweep/fs_watcher` | FSEvents live updates (ObjC++/`.mm`, GCD dispatch queue) | ✅ |
| `sweep/politeness` | Thermal (NSProcessInfo) + battery (IOKit) sensors; crawl throttle under thermal pressure | ✅ |
| `query/query_engine` | FTS5 BM25 over name + path + content + tags; **direct name matches tiered above content-only**; excludes tombstones | ✅ (content/tags filled by brick 5) |
| `extract/` | Extractor thread (own RO conn, lock-step) + bridges: plain text, PDFKit, ImageIO/AVFoundation tags, Finder color tags | ✅ brick 5 (OCR + office docs → Phase 2.5) |
| `ai/` | Swift interop: AFM session, CoreML embedder | ⬜ Phase 3 |

### App — `MacAssist.app` (Swift, SwiftPM)
Menu-bar resident, global ⌥Space hotkey, non-activating blurred `NSPanel`, debounced live
search, reveal-in-Finder / show-all-as-folder actions. ✅ **Phase 1 complete.** Settings UI,
onboarding, Full Disk Access flow ⬜ Phase 4.

### `mactl` (CLI, speaks the socket protocol)
`status`, `query <text>` work. ✅ Primary dev/test harness.

### Tests — `daemon/tests/` (Catch2 + CTest)
✅ **Brick 6.** One `daemon_tests` binary over `macassistd_core` (the daemon logic is now a
static lib so tests and the executable share identical code). 40 cases / 267 assertions covering
`file_kind`, `database` (schema v4, migration rebuild, meta, counts, RO guard), `indexer`
(upsert/fingerprint/tombstone/`needs_extract`/`ApplyExtraction` race-guard), `crawler`,
`query_engine` (incl. the name-match ranking tier), `plain_text`, `frame`, `protocol`,
`request_router`, and a threaded `Writer`+`Extractor` integration test. Golden fixtures
(`report.pdf`/`camera.jpg`/`song.m4a`) exercise the PDFKit/ImageIO/AVFoundation bridges. Run with
`./scripts/test.sh` (or `ctest --test-dir build`). Catch2 v2 single header vendored at
`third_party/catch2/`.

### Build & distribution
CMake (daemon + `mactl`) + SwiftPM (app), via `scripts/build.sh`. `.app` bundling via
`scripts/make-app.sh`. ⬜ launchd agent, code signing, notarization, DMG (Phase 4). ⬜ Xcode
workspace migration (Phase 3, when Swift↔C++ interop forces it).

---

## What we've done

### Phase 1 (on `main`) — vertical slice
End-to-end: ⌥Space → type → ranked filename results → reveal in Finder / open a folder of all
hits. Daemon did a one-shot stat-only crawl at startup; name-only FTS. (See git history.)

### Post-Phase-2 features (on `main`)
- **Finder color tags in results — display-only** (schema v5). A tiny ObjC++ bridge
  (`extract/finder_tags.mm`) reads the `com.apple.metadata:_kMDItemUserTags` xattr (a plist of
  `"Name\nColorIndex"` strings) into a compact JSON array stored in a new `files.tags_json`
  column. Read on **every stat** in `Indexer::UpsertFile`, not during extraction — setting a
  Finder tag changes neither mtime nor size, so the fingerprint can't see it; the "unchanged"
  branch rewrites `tags_json` only when it actually differs (no reconcile write amplification).
  Carried on `FileHit` → emitted as a structured `tags` array on each `results` item → rendered
  as colored dots in the app's result rows (`TagDots` in `SearchView`). Not fed into FTS —
  searching by color is deferred to a planned toolbar of "special selections". Verified live:
  tags surface in query results, and a **tag-only change propagates within ~2s** (FSEvents →
  re-stat → `tags_json` refresh) even though the fingerprint is unchanged.

### Phase 2 (merged to `main`; developed on branch `phase2-indexing`)
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
- **Brick 4 — reconciliation + politeness** (branch). The writer's drain loop became a
  `wait_until(next_reconcile)`: when idle for a full interval it runs a reconciliation crawl
  (the FSEvents-drop / offline backstop), via a `RunFullCrawl(reason)` helper shared by the
  initial crawl, reconcile, and FSEvents full-rescan. Interval is `--reconcile-interval`
  (default 24h). Writer thread runs at `QOS_CLASS_UTILITY`. `Politeness` (sweep/politeness.mm)
  reads thermal (`NSProcessInfo`) + battery (IOKit); the crawl pauses under serious/critical
  thermal pressure. **Decided:** startup always runs a full crawl and the watcher uses
  `sinceNow` (no replay) — the crawl is the source of truth for offline changes.
- **Brick 5 — cheap extraction** (branch; see `BRICK_5.md`). Content/metadata now fill the
  previously-empty `content`/`tags` FTS columns + the new `file_meta` table. A dedicated
  `Extractor` thread (third, read-only connection) discovers stale files via an indexed
  `needs_extract` flag (schema v4), extracts off the writer thread, and funnels results back
  through the write-command queue as a new `kApplyExtract`. **Lock-step** (one batch in flight)
  + a **sweeping gate** keep the queue bounded and avoid re-extracting un-applied rows; the
  writer clears the flag guarded by `fingerprint`, so an edit mid-extraction self-corrects and
  unsupported/empty/errored files are stamped done (no loop). Bridges: `plain_text.cpp`
  (txt/md/csv), `pdf_text.mm` (PDFKit), `media_tags.mm` (ImageIO EXIF + AVFoundation tags).
  On battery the backlog is deferred to AC via an `indexed_at >= now−7d` filter
  (`--pretend-battery` dev flag). **RTF + office docs + OCR deferred to Phase 2.5.**
- **Brick 6 — test suite** (branch). The daemon logic is now a static lib (`macassistd_core`)
  shared by the `macassistd` executable and a `daemon_tests` binary; `main.cpp` is a thin shell.
  Catch2 v2 single-header vendored. 40 cases / 267 assertions across unit + integration tests
  (see the Tests facet above), including threaded `Writer`+`Extractor` coverage and golden
  PDF/JPG/M4A fixtures. `./scripts/test.sh` builds + runs via CTest.
- **Query ranking — name-match tier** (post-brick-5, during testing). `SearchFiles` now tiers a
  **direct filename match above any content/path-only hit** (`LEFT JOIN` to a name-column match;
  `name_hit IS NULL` is the primary `ORDER BY` key, bm25 breaks ties within a tier). The tiering
  is in the `ORDER BY`, so it applies *before* `LIMIT` — a name match can't be truncated out by a
  pile of content hits. Gotcha learned: **FTS5 `MATCH` rejects a table alias** (`nm MATCH ?` →
  "no such column: nm"), which silently zeroed results; the derived-table `LEFT JOIN` avoids it.

**Verified** (ad-hoc, via `mactl` + `sqlite3`): fresh crawl indexes all; a re-crawl re-indexes
only changed/new files and tombstones deletes; and with the daemon **running**, live
create/delete/rename/modify reflect in query results within ~2s. **Brick 5:** on a throwaway
corpus, body-text (txt/md/csv), PDF text-layer, EXIF camera model, and audio title/artist/album
tokens all hit; `file_meta` holds typed dimensions/duration/artist; `pending` drains to 0 incl.
deferred `.rtf` (stamped, not looped); editing a file re-extracts within ~2s; and on battery an
old-`indexed_at` file stays deferred while fresh files extract.

---

## What's next

### Phase 2 — complete
All six bricks landed: incremental sweeper, FSEvents live updates, reconciliation, politeness,
cheap content extraction, and the test suite (brick 6). Remaining extraction depth is Phase 2.5.

- **Phase 2.5 (follow-on):** Vision OCR for image-only PDFs/images; office-doc (docx/pages)
  and RTF text extraction — the parsers brick 5 deferred. These re-trigger extraction via the
  schema-bump nuke-rebuild.

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
./scripts/build.sh          # daemon + mactl + daemon_tests (CMake) and the app (SwiftPM)
./scripts/test.sh           # build + run the test suite via CTest ([tag] filters a subset)
./scripts/make-app.sh       # (re)build the MacAssist.app bundle

# Against real files (defaults: ~/Library/Application Support/MacAssist/{daemon.sock,index.db}
# and the six home roots):
./build/daemon/macassistd

# Dev loop against a throwaway corpus — keep the socket path short (< ~104 chars).
# --reconcile-interval <secs> overrides the 24h default (use a few secs to watch reconciles).
# --pretend-battery forces the on-battery extraction policy (defers the >7d-old backlog):
./build/daemon/macassistd --socket /tmp/dev.sock --db /tmp/dev.db --root /path/to/corpus \
    --reconcile-interval 2 [--pretend-battery]
./build/tools/mactl/mactl --socket /tmp/dev.sock status   # watch pending -> 0
./build/tools/mactl/mactl --socket /tmp/dev.sock query resume
```

---

## Known caveats

### Indexing / search coverage
- **Office docs (`docx`/`doc`/`pptx`/`xlsx`/`pages`/`key`), `rtf` are name-only.** They are
  crawled and findable by **filename + folder path**, but their **body text is NOT extracted**
  (the extractor has no bridge for them) — they're stamped `needs_extract=0` so they don't loop.
  Real content extraction is **Phase 2.5**. Searching a phrase that only lives *inside* a `.docx`
  will miss it.
- **OCR deferred to Phase 2.5** — scanned/image-only PDFs have no text layer, so they index with
  empty `content` (name-searchable only).
- **Extraction caps (PLAN §3):** text/PDF bodies capped at 100 KB; PDFs at 200 pages; image/AV
  metadata is a curated subset (dimensions, camera make/model, duration, title/artist/album).

### Extraction engine — things to remember
- **`pending` is the durable truth; `extractedFiles` is not.** `extractedFiles` is an in-memory
  counter that **resets to 0 on daemon restart** (it counts applies since process start). To know
  if extraction is complete, watch `pending → 0` in `mactl status`.
- **Schema bump = nuke-and-rebuild.** Any `kSchemaVersion` change (pre-release policy) drops all
  tables and re-crawls + re-extracts from scratch. Currently **v4**. This is how Phase 2.5
  parsers will re-trigger extraction on existing files.
- **On battery, the old backlog is deferred** (`indexed_at >= now−7d`); only recently-encountered
  files stay current until AC. Freshly-touched/created files still extract on battery. The
  "drains when plugged in" half can't be exercised on battery-only hardware; use
  `--pretend-battery` to force the on-battery path for testing.
- **No per-file extraction timeout** (single extractor thread) — a pathological PDF could stall
  the backlog. Page/size caps are the only mitigation; subprocess isolation is a later hardening.
- **Fingerprint is `mtime(sec):size`** — a same-size edit within the *same wall-clock second* as
  the prior index is not detected (neither live nor on reconcile) until mtime ticks. Coarse by
  design; real edits virtually always change size or cross a second boundary. (Bit us once in a
  test where two bodies were coincidentally equal length.)
- **`CoreGraphics PDF has logged an error`** in the daemon log is harmless noise from PDFKit
  parsing a malformed PDF; our code treats it as empty content and moves on.

### Dev workflow gotchas
- **Quit the old app before relaunching.** `scripts/make-app.sh` does `rm -rf` on the `.app`
  bundle and rebuilds it; a still-running instance keeps the old (now-deleted) binary, and `open`
  on the rebuilt bundle just **reactivates the stale instance** (same bundle id
  `com.finn.macassist`) rather than launching fresh — which can leave the menu-bar status item in
  a bad state. `pkill -x MacAssist` first, then `open`.
- **Restarting the daemon does NOT require a re-crawl unless the schema changed.** Query-side
  changes (e.g. ranking) just need a rebuild + restart; the index persists (WAL files alongside
  `index.db`).
- **FTS5 `MATCH` rejects a table alias** — use the table name or a derived table, never
  `alias MATCH ?` (fails to prepare → silent empty results).
- **Socket path ≤ ~104 chars** (`sockaddr_un` limit) — dev sockets must live in `/tmp`, not the
  long scratchpad path.
- **Menu-bar icon can hide behind the notch** on crowded menu bars (environmental, not a bug);
  ⌘-drag menu-bar items to pull it out.

### Not yet built
- **No launchd agent yet** — the daemon is launched manually (Phase 4).
- **`action`/`reindex`/`config` IPC** not implemented (router handles `ping`/`status`/`query`).
