# MacAssist — Design & Implementation Plan

A macOS application that acts as a smarter Finder: it maintains rich metadata about the
user's files and answers natural-language queries ("show me my resume", "pull up my Drake
mp4s", "open my_photo.png") in under two seconds, via a Spotlight-style hotkey panel.

*Finalized 2026-08-30 after design discussion. Decisions marked ⚙ are changeable defaults.*

---

## 1. Locked decisions

| Area | Decision |
|---|---|
| Target | Apple Silicon only, macOS 26 (Tahoe) — required by FoundationModels |
| Architecture | Background daemon (launchd agent) + thin UI panel app |
| Daemon language | **C++ owns the daemon** (event loop, index, query engine, sweeps); Apple-framework calls via ObjC++ bridges and Swift↔C++ interop |
| UI | Swift + AppKit/SwiftUI non-activating floating panel, global hotkey |
| NL query parsing | **Apple Foundation Models** (on-device LLM, Swift-only API → reached via Swift/C++ interop) |
| Semantic search | Bundled ~100 MB sentence-embedding model (bge-small class) via CoreML; vectors in `sqlite-vec` |
| Storage | Single SQLite database: relational tables + FTS5 + sqlite-vec |
| Index depth | Content-aware: PDF/doc text extraction, Vision OCR on images, media tags (ID3/MP4/EXIF). No audio transcription in v1 |
| File types v1 | Documents (pdf, docx, txt, md, pages, rtf), Images (png, jpg, heic, gif, webp), Audio/Video (mp3, m4a, wav, mp4, mov, mkv) |
| Scan scope | Configurable roots; defaults: ~/Documents, ~/Desktop, ~/Downloads, ~/Movies, ~/Music, ~/Pictures + user exclusions |
| Sweep model | Initial full crawl → FSEvents live updates → nightly reconciliation pass |
| IPC | Unix domain socket, length-prefixed JSON (version-tagged) |
| Result UX | Ranked top-5 list; Enter opens top hit; per-result actions: open, reveal in Finder, copy path, **create symlink** (on demand, to Desktop) |
| Voice | Phase 6 (later) — Speech framework feeding the same query pipeline |
| First milestone | Vertical slice: hotkey → panel → daemon → FTS filename search → open |

---

## 2. High-level architecture

```
┌────────────────────────────────────────────────────────────┐
│  MacAssist.app  (Swift, menu-bar resident)                 │
│  • Global hotkey (Carbon RegisterEventHotKey — no          │
│    Accessibility permission needed)                        │
│  • Centered NSPanel (non-activating, blur material)        │
│  • Results list + actions; Settings window (roots, hotkey) │
│  • Installs/updates the launchd agent on first run         │
└──────────────▲─────────────────────────────────────────────┘
               │ Unix domain socket (JSON, length-prefixed)
               │ ~/Library/Application Support/MacAssist/daemon.sock
┌──────────────▼─────────────────────────────────────────────┐
│  macassistd  (C++ launchd agent, runs 24/7)                │
│                                                            │
│  ipc/       socket server, protocol codec                  │
│  sweep/     crawler, FSEvents listener, reconciler,        │
│             work queue + extraction thread pool            │
│  extract/   ObjC++ bridges: PDFKit, Vision OCR,            │
│             AVFoundation/ID3, EXIF, plain text             │
│  index/     SQLite layer: schema, FTS5, sqlite-vec         │
│  query/     structured-query executor, rank fusion         │
│  ai/        Swift interop shims:                           │
│             • AFM session → SearchIntent (guided gen)      │
│             • CoreML embedding model → float[384]          │
└──────────────┬─────────────────────────────────────────────┘
               │
        ~/Library/Application Support/MacAssist/index.db
```

**Why this shape:** the daemon keeps the index warm and sweeps running with the UI closed;
the panel app stays tiny and instant. C++ owns everything that is pure logic; the only
Swift/ObjC in the daemon is at the Apple-framework edges (extraction + AI), compiled into
the same binary via ObjC++ and Swift/C++ interop (Swift ≥5.9 feature — no wrappers process).

A CLI tool (`mactl`) speaks the same socket protocol for development and testing:
`mactl query "my resume"`, `mactl status`, `mactl reindex`.

---

## 3. Data model (SQLite)

One database, WAL mode. Approximate schema:

```sql
CREATE TABLE roots (
  id INTEGER PRIMARY KEY, path TEXT UNIQUE, enabled INT DEFAULT 1);

CREATE TABLE exclusions (id INTEGER PRIMARY KEY, glob TEXT);

CREATE TABLE files (
  id INTEGER PRIMARY KEY,
  root_id INT REFERENCES roots(id),
  path TEXT UNIQUE NOT NULL,           -- absolute
  name TEXT NOT NULL,                  -- basename
  ext TEXT, kind TEXT,                 -- kind: document|image|audio|video
  size INT, created_at INT, modified_at INT,
  indexed_at INT,
  fingerprint TEXT,                    -- mtime:size, to skip unchanged files
  missing INT DEFAULT 0,               -- tombstone until reconciliation confirms
  seen_gen INT DEFAULT 0,              -- crawl generation last seen (mark-and-sweep)
  needs_extract INT NOT NULL DEFAULT 1);-- 1 = body/metadata not yet extracted
  -- partial index CREATE INDEX ... ON files(id) WHERE needs_extract=1 makes the
  -- extractor's "what's still stale?" poll O(pending) rather than a full scan.

-- keyword search: filename tokens, extracted text, media tags flattened.
-- Standalone FTS5 (not external-content): stores the indexed text once and
-- supports snippet()/highlight() for previews; rowid == files.id.
CREATE VIRTUAL TABLE file_text USING fts5(
  name, path_tokens, content, tags, tokenize='unicode61');

-- typed metadata (EXIF, ID3, dimensions, duration, artist, album, camera…)
CREATE TABLE file_meta (
  file_id INT REFERENCES files(id), key TEXT, value TEXT,
  PRIMARY KEY (file_id, key));

-- semantic vectors (sqlite-vec); one vector per file in v1:
-- embed(name + tags + first ~2000 chars of extracted text)
CREATE VIRTUAL TABLE file_vec USING vec0(
  file_id INTEGER PRIMARY KEY, embedding FLOAT[384]);

CREATE TABLE meta (key TEXT PRIMARY KEY, value TEXT);
  -- keys in use: schema_version, crawl_gen, fsevents_last_id (+ stats later)
```

**Current schema state (v5):** `files` (incl. `tags_json` — Finder color tags, read on every
stat, display-only), `file_text` (name/path_tokens/content/tags), `file_meta`, `roots`,
`exclusions`, `meta` are all created and populated. `file_vec` is **not yet created** — it lands
in Phase 3 with embeddings. `content`/`tags`/`file_meta` are filled by
the Brick-5 extractor for text/PDF/media; office docs, `rtf`, and OCR are name-only until Phase
2.5 (schema-version bumps nuke-and-rebuild, which re-extracts everything, so those parsers
re-trigger automatically when added).

Extraction caps (v1): documents — first 100 KB of text into FTS; OCR — skip images
> 20 MP, cap OCR output at 10 KB; one embedding vector per file (chunked embeddings are a
later upgrade for long documents).

---

## 4. Query pipeline (the latency-critical path)

Target: **≤ 1.5 s p50, ≤ 3 s p95** from Enter to results. Two-stage progressive design so
the panel never feels blocked on the LLM:

```
user query ─┬─► Stage A (instant, <50 ms):
            │     raw tokens → FTS5 (name-weighted) → provisional top-5 shown
            │
            └─► Stage B (in parallel, ~300–900 ms):
                  AFM guided generation → SearchIntent struct
                  + embed(query) via CoreML
                  → structured SQL filters + FTS + vector KNN
                  → rank fusion → results replace/refine Stage A list
```

**SearchIntent** (AFM `@Generable` guided generation — AFM returns a typed value, no
prompt-injection-prone free text):

```swift
@Generable struct SearchIntent {
  var terms: [String]          // content/name keywords, e.g. ["Drake"]
  var kinds: [FileKind]        // .document/.image/.audio/.video, empty = any
  var extensions: [String]     // e.g. ["mp4"]
  var timeRange: TimeRange?    // "last week", "from 2023"
  var metaFilters: [MetaFilter]? // e.g. artist == "Drake"
  var exactFilename: String?   // set for "open my_photo.png"
  var semanticQuery: String    // paraphrase for embedding search
}
```

**Retrieval + ranking:**
1. Apply hard filters (kind, extension, time range, meta) as SQL predicates.
2. Candidate generation: FTS5 BM25 over `file_text` (name column boosted) ∪ vector KNN
   (top 50) from `file_vec`.
3. **Reciprocal rank fusion** across the two lists, then boosts: exact-filename match
   (dominant — `open my_photo.png` must win), recency decay, root/kind priors.
4. If `exactFilename` matches uniquely, short-circuit to it.

**Implemented in Phase 2 (FTS-only path, pre-fusion):** the "filename dominates" boost already
exists in `query/query_engine`. Results are two-tiered — a **direct name-column match ranks
above any content/path-only hit** (a `LEFT JOIN` to a name-only FTS match; `name_hit IS NULL` is
the primary `ORDER BY` key, bm25 with name>path>tags>content weights breaks ties within a tier).
The tiering is in the `ORDER BY`, so it applies *before* `LIMIT` — a strong pile of content hits
can't bump a filename match off the list. (Implementation note: FTS5 `MATCH` rejects a table
alias, so the name-only match is a derived table, not `alias MATCH ?`.)

**AFM latency discipline:** the daemon keeps one prewarmed `LanguageModelSession`
(instructions preloaded) so per-query cost is generation only. If AFM is unavailable
(model downloading, Apple Intelligence off), Stage A results stand alone and the panel
shows a subtle "basic search" indicator — the app degrades, never breaks.

**Stage trigger (decided 2026-10-06, refines the two-stage design above):** Stage A
(FTS) runs live on every keystroke; **Stage B (AFM + embeddings) is deferred to Enter**,
not run automatically in parallel on each query. The panel stays pure-FTS and instant
while typing, and the expensive on-device LLM only engages when the user commits — saving
battery/compute and keeping typing latency flat. Enter is therefore both "show full
results" and the point where Stage B runs.

**Query caching (decided 2026-10-06):** cache `query → SearchIntent`, keyed by normalized
(lower-cased, trimmed) query text, in a small LRU. `SearchIntent` is the *expensive* part
(the AFM call) and is a pure function of the query string — independent of the file index
— so it is safe to cache indefinitely; a repeat query skips the LLM entirely. Do **not**
durably cache final hit lists: the sweeper mutates the index continuously, so cached hits
go stale. Always recompute retrieval (FTS ∪ vector KNN → fusion) fresh against the live
index using the cached intent. The symlink *result folder* is a disposable output (only
the current search's folder is kept), never a cache — rebuilding it from a hit list is
sub-millisecond, so retaining old folders buys nothing without a deliberate
"recent searches" feature.

---

## 5. Sweep subsystem

**Threading & concurrency (locked 2026-10-07).** Concurrency is handled by *topology + SQLite
WAL*, never by an application-level lock over the database:

- **Single writer, connection-per-role.** Exactly one *writer* thread owns the write
  connection and is the only thread that writes. Full crawls and reconciliation run as batch
  jobs **directly on this thread** (one big transaction, no per-file hand-off); mutations
  from threads that *can't* touch the DB (the FSEvents watcher, the extraction workers)
  arrive via a **write-command queue** the same thread drains. The IPC query handler uses its own
  *read-only* connection. WAL (already enabled) lets readers run against the last committed
  snapshot *concurrently* with an in-flight write and serializes writers internally — so an
  extraction batch never blocks a query. A `std::shared_mutex` over the DB is explicitly
  rejected: redundant with WAL, and it would reintroduce exactly the reader-blocks-on-writer
  stall WAL removes, hurting the ≤1.5 s query path.
- **Extraction reads on its own connection; all writes stay on the writer.** The extractor
  posts finished payloads as apply-extraction commands onto the writer's command queue, so all
  writes stay on one connection — no competing writers, no `SQLITE_BUSY` churn.
  **As built (Brick 5, refines the original "worker pool + push job queue" sketch):** a
  **single** extractor thread with its **own read-only connection** *pulls* stale files from the
  DB (`WHERE needs_extract=1`, indexed) rather than having the crawler push a job queue — durable
  and crash-resumable (the flag is the queue). It runs **lock-step** (one batch in flight, waits
  for the writer to apply before pulling again, bounding queue memory) and is **gated on
  `sweeping`** (parks during a full crawl, since the writer can't drain applied results
  mid-crawl). A multi-thread pool is the upgrade path *if* single-thread throughput disappoints.
- **Primitives.** Both queues are `mutex` + `condition_variable` (producer/consumer — every
  access mutates, so no rwlock). Status counters read by the `status` IPC (`indexedFiles`,
  `sweeping`, progress) are `std::atomic`. In-memory config, *if* ever cached rather than
  read from the DB, would be copy-on-write via an `atomic<shared_ptr>` — still no rwlock.

Pipeline:

- **Initial crawl:** breadth-first per root, honoring exclusions (default globs:
  `node_modules`, `.git`, `*.app`, caches, `~/Library`). Stat-only pass populates `files`
  fast (searchable by name within seconds); extraction jobs queue behind it.
- **Extraction (as built, Brick 5):** one extractor thread pulls stale files
  (`needs_extract=1`, freshest `indexed_at` first) on its own read-only connection, dispatches by
  type → extract → hands the payload to the writer, which writes `file_text.content`/`tags` +
  `file_meta` and clears `needs_extract` **guarded by `fingerprint`** (a file edited
  mid-extraction stays flagged → re-extracted; unsupported/empty/errored files are stamped done
  so nothing loops) in one transaction. Scope: plain text (`txt/md/csv`), PDF text (PDFKit),
  image EXIF + audio/video tags (ImageIO/AVFoundation). **Office docs, `rtf`, OCR → Phase 2.5.**
  Caps per PLAN §3 (100 KB / 200 pages). Embedding batching is Phase 3 (`file_vec` not yet made).
- **Extraction politeness:** extractor thread at `QOS_CLASS_UTILITY`; pauses under thermal
  pressure; on battery defers the backlog (`indexed_at >= now−7d`) so only recently-encountered
  files stay current until AC (`--pretend-battery` dev flag forces this path).
- **Live updates:** one FSEvents stream over all roots, 2 s latency coalescing. Events
  enqueue re-stat → re-extract if fingerprint changed; deletes set `missing=1`
  (purged at reconciliation).
- **Reconciliation:** nightly (and on daemon start after >24 h downtime): full re-walk,
  diff vs. index, catches FSEvents drops and offline changes.
- **Politeness:** extraction pauses on low battery / thermal pressure (IOKit); QoS
  utility class threads; sweep state journaled so restarts resume, not restart.

---

## 6. IPC protocol

Framing: 4-byte big-endian length + UTF-8 JSON. Every message: `{"v":1,"type":...}`.
Socket file mode 0600 (user-only — this is also the security boundary).

| type | direction | payload |
|---|---|---|
| `query` | app→d | `{id, text, stage:"instant"\|"full"}` |
| `results` | d→app | `{id, stage, items:[{fileId, path, name, kind, score, snippet, modifiedAt}]}` |
| `action` | app→d | `{fileId, action:"symlink", dest?}` (open/reveal/copy done app-side) |
| `status` | app↔d | index counts, sweep progress, AFM availability |
| `reindex` / `config` | app→d | force sweep; get/set roots, exclusions |

Both stages of a query return under the same `id`; the panel swaps Stage B in when it
arrives. Version mismatch → app offers daemon reinstall.

---

## 7. UI app details

- Menu-bar item (status, settings, quit); no Dock icon (`LSUIElement`).
- Hotkey ⚙ default **⌥Space** via `RegisterEventHotKey` (works without Accessibility
  permission; rebindable in Settings).
- Panel: `NSPanel` (`.nonactivatingPanel`), centered, `NSVisualEffectView` blur, SwiftUI
  content. Escape dismisses; focus returns to previous app.
- Results: icon (QuickLook thumbnail), name, parent folder, modified date, score-debug in
  dev builds. ↑↓ navigate, **Enter = open**, ⌘Enter = reveal in Finder, ⌘C = copy path,
  ⌘L = symlink to Desktop (collision → `name-2` suffix).
- First-run onboarding: explain Full Disk Access → deep-link to System Settings pane →
  install launchd agent → kick off initial crawl with progress in the panel.

---

## 8. Permissions, distribution, ops

- **Full Disk Access** granted to the daemon binary (user action, one-time; onboarding
  handles the UX). App itself needs no special TCC permissions in v1.
- Not sandboxed (arbitrary-filesystem indexing is the product). Hardened runtime,
  Developer ID signed, notarized; distributed as a DMG outside the App Store.
- launchd agent plist in `~/Library/LaunchAgents/com.finn.macassist.daemon.plist`:
  `RunAtLoad`, `KeepAlive`, socket path in env. App owns install/upgrade of the plist +
  daemon binary (bundled inside the .app, copied out or exec'd in place ⚙).
- Logs: `os_log` from both processes; `mactl status --verbose` for index health.

## 9. Repo layout & build

```
MacAssist/
  PLAN.md
  daemon/            # C++20; macassistd (+ macassistd_core static lib)
    src/{main,ipc,sweep,index,query}/     # pure C++ (main.cpp = thin shell on the lib)
    src/extract/     # .mm ObjC++ bridges (PDFKit, ImageIO, AVFoundation) + plain_text.cpp
    src/ai/          # Swift files (AFM session, CoreML embedder) + interop headers
    tests/           # Catch2 (vendored single header) + golden fixtures; CTest target
  app/               # Swift app target (panel, menu bar, settings, onboarding)
  shared/            # protocol constants/schemas shared by daemon, app, mactl
  tools/mactl/       # CLI client (Swift or C++, trivial either way)
  scripts/           # dev bootstrap, corpus generator, latency bench
```

Build (current): the daemon + `mactl` + the `daemon_tests` suite build with **CMake** (C++20;
the `.mm` bridges compile as ObjC++/ARC). The daemon's logic lives in a `macassistd_core`
static library linked by both the executable and the tests; `./scripts/test.sh` runs the suite
via CTest. Test framework: **Catch2 v2** (vendored single header; chosen over GoogleTest to match
the repo's hand-vendored, header-only dependency style). The Swift app builds with **SwiftPM**.
The two are independent and meet only over the socket protocol. The **Xcode-workspace
migration is deferred to Phase 3**, when Swift↔C++ interop for AFM/CoreML
(`SWIFT_OBJC_INTEROP_MODE`) forces a single mixed-language binary — CMake fights that, but
handles today's C++/ObjC++ daemon fine. Vendored deps: SQLite (custom build with FTS5),
`sqlite-vec`, JSON lib (nlohmann ⚙), embedding model as a compiled `.mlmodelc` resource.

---

## 10. Phased roadmap

**Phase 1 — Vertical slice (walking skeleton).**
Daemon: socket server, stat-only crawl of one hardcoded root, `files` + name-only FTS,
naive keyword query. App: hotkey, panel, results list, Enter-to-open. `mactl` works.
*Done when: hotkey → type "resume" → resume.pdf opens, end-to-end.*

**Phase 2 — Real indexing.** *(scope refined 2026-10-07)*
Background incremental sweeper (upsert + fingerprint-skip + `missing` tombstones);
configurable roots + exclusions schema; FSEvents live updates; reconciliation;
fingerprinting; politeness; **cheap extraction only** — PDF/plain-text + media tags
(EXIF/ID3/MP4). Vision OCR and office-doc (docx/pages) parsing defer to **Phase 2.5** (OCR
is the thermal risk and can't finish during the crawl). Fixture corpus + tests.
*Done when, across three distinct goals:* (liveness) the index reflects moves/renames/deletes
within seconds and survives restarts; (crawl speed) a 50k-file stat pass makes filenames
searchable in minutes; (content) extracted text/tags fill in progressively in the background
without blocking search.*
*Status: **complete** — sweeper + FSEvents + reconciliation + politeness + cheap extraction +
test suite (all six bricks) implemented. Remaining extraction depth is Phase 2.5. See
`PROGRESS.md`.*

**Phase 3 — Intelligence.**
Swift interop layer; AFM `SearchIntent` guided generation with prewarmed session; CoreML
embeddings + `sqlite-vec`; two-stage progressive query; rank fusion; graceful AFM-absent
degradation. Golden query set (~50 NL queries with expected hits) as a regression suite.
*Done when: "pull up my Drake mp4s" and "that lease pdf from last year" rank correctly and p50 ≤ 1.5 s.*

**Phase 4 — Product polish.**
Settings UI, onboarding + FDA flow, launchd install/upgrade, symlink action, QuickLook
thumbnails, menu-bar status, signing + notarization + DMG.

**Phase 5 — Stretch.**
Voice input (Speech framework → same pipeline); opt-in audio/video transcription during
sweeps; chunked embeddings for long docs; confidence-gated auto-open ("open X" skips the
list when the top score clears a threshold); more file types.

## 11. Risks & mitigations

| Risk | Mitigation |
|---|---|
| AFM availability/quality varies by machine & OS state | Stage A works without it; guided generation constrains output; golden-query suite catches regressions |
| Full Disk Access friction scares users off | Onboarding invests heavily here; app still works on TCC-free folders (~/Downloads etc.) before FDA granted |
| OCR/extraction makes first sweep slow & hot | Stat-first crawl (names searchable immediately), priority queue, thermal/battery pausing, caps per file |
| Swift↔C++ interop rough edges | Confine to `src/ai/` behind a tiny C++-visible interface; ObjC++ fallback exists for anything interop can't express |
| FSEvents drops events | Nightly reconciliation is the source of truth |
| sqlite-vec maturity | Vector search is additive (fusion input); FTS path never depends on it |
