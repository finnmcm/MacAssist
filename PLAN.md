# MacAssist — Design & Implementation Plan

A macOS application that acts as a smarter Finder: it maintains rich metadata about the
user's files and answers natural-language queries ("show me my resume", "pull up my Drake
mp4s", "open my_photo.png") in under two seconds, via a Spotlight-style hotkey panel.

*Design finalized 2026-08-30, revised as the build progressed. Decisions marked ⚙ are
changeable defaults. This is the **design**; live status and per-feature history live in
`PROGRESS.md`.*

---

## 1. Locked decisions

| Area | Decision |
|---|---|
| Target | Apple Silicon only, macOS 26 (Tahoe) |
| Architecture | Background daemon (launchd agent) + thin UI panel app |
| Daemon language | **C++ owns the daemon** (event loop, index, query engine, sweeps); Apple-framework calls via ObjC++ bridges |
| UI | Swift + AppKit/SwiftUI non-activating floating panel, global hotkey |
| Storage | Single SQLite database: relational tables + FTS5 (WAL mode) |
| Index depth | Content-aware: PDF/plain-text extraction, media tags (ID3/MP4/EXIF). OCR + office docs are Phase 2.5; no audio transcription in v1 |
| File types v1 | Documents (pdf, docx, txt, md, pages, rtf), Images (png, jpg, heic, gif, webp), Audio/Video (mp3, m4a, wav, mp4, mov, mkv), Code & config (py, js, ts, swift, …; json/yaml/xml/html/css) — see `EDITOR_PLAN.md`. Folders + `.zip` are first-class kinds |
| Scan scope | Configurable roots; defaults: ~/Documents, ~/Desktop, ~/Downloads, ~/Movies, ~/Music, ~/Pictures + user exclusions |
| Sweep model | Initial full crawl → FSEvents live updates → nightly reconciliation pass |
| IPC | Unix domain socket, length-prefixed JSON (version-tagged), mode 0600 |
| Result UX | Ranked top-5 list; per-result actions: open, reveal in Finder, copy path, **open in editor** (⌘E — see `EDITOR_PLAN.md`), **create symlink** (to Desktop) |
| Search | Keyword / FTS5 + a layerable filter toolbar. On-device LLM / semantic search was **dropped** (2026-10-19; see §4) |
| First milestone | Vertical slice: hotkey → panel → daemon → FTS filename search → open |

---

## 2. High-level architecture

```
┌────────────────────────────────────────────────────────────┐
│  MacAssist.app  (Swift, menu-bar resident)                 │
│  • Global hotkey (Carbon RegisterEventHotKey — no          │
│    Accessibility permission needed)                        │
│  • Centered NSPanel (non-activating, blur material)        │
│  • Results list + actions; Settings (roots, hotkey, editor)│
│  • Installs/updates the launchd agent on first run         │
└──────────────▲─────────────────────────────────────────────┘
               │ Unix domain socket (JSON, length-prefixed)
               │ ~/Library/Application Support/MacAssist/daemon.sock
┌──────────────▼─────────────────────────────────────────────┐
│  macassistd  (C++ launchd agent, runs 24/7)                │
│  ipc/     socket server, protocol codec                    │
│  sweep/   crawler, FSEvents listener, reconciler,          │
│           write-command queue + extraction thread          │
│  extract/ ObjC++ bridges: PDFKit, AVFoundation/ID3,        │
│           EXIF, Finder tags, download date, plain text     │
│  index/   SQLite layer: schema, FTS5                        │
│  query/   structured-query executor + ranking              │
└──────────────┬─────────────────────────────────────────────┘
               │
        ~/Library/Application Support/MacAssist/index.db
```

**Why this shape:** the daemon keeps the index warm and sweeps running with the UI closed;
the panel app stays tiny and instant. C++ owns everything that is pure logic; the only ObjC++ in
the daemon is at the Apple-framework edges (extraction). A CLI tool (`mactl`) speaks the same
socket protocol for development and testing (`mactl query "my resume"`, `mactl status`).

---

## 3. Data model (SQLite)

One database, WAL mode. Core schema:

```sql
CREATE TABLE roots (id INTEGER PRIMARY KEY, path TEXT UNIQUE, enabled INT DEFAULT 1);
CREATE TABLE exclusions (id INTEGER PRIMARY KEY, glob TEXT);

CREATE TABLE files (
  id INTEGER PRIMARY KEY,
  root_id INT REFERENCES roots(id),
  path TEXT UNIQUE NOT NULL,           -- absolute
  name TEXT NOT NULL,                  -- basename
  ext TEXT, kind TEXT,                 -- document|image|audio|video|archive|folder|code
  size INT, created_at INT, modified_at INT, indexed_at INT,
  downloaded_at INT,                   -- quarantine xattr → Spotlight fallback
  tags_json TEXT,                      -- Finder color tags (display-only)
  fingerprint TEXT,                    -- mtime:size, to skip unchanged files
  missing INT DEFAULT 0,               -- tombstone until reconciliation confirms
  seen_gen INT DEFAULT 0,              -- crawl generation last seen (mark-and-sweep)
  needs_extract INT NOT NULL DEFAULT 1);-- 1 = body/metadata not yet extracted; partial-indexed

-- keyword search: standalone FTS5 (rowid == files.id), supports snippet()/highlight()
CREATE VIRTUAL TABLE file_text USING fts5(
  name, path_tokens, content, tags, tokenize='unicode61');

CREATE TABLE file_meta (                 -- typed metadata (EXIF, ID3, dimensions, duration…)
  file_id INT REFERENCES files(id), key TEXT, value TEXT, PRIMARY KEY (file_id, key));

CREATE TABLE meta (key TEXT PRIMARY KEY, value TEXT);
  -- keys: schema_version, crawl_gen, fsevents_last_id
```

**Current state (schema v7).** All of the above is created and populated. `kind` spans
`document|image|audio|video|archive|folder|code`; `content`/`tags`/`file_meta` are filled by the
extractor for text/PDF/media. Office docs, `rtf`, and OCR are name-only until Phase 2.5. A
schema-version bump is nuke-and-rebuild (pre-release policy), which re-extracts everything, so new
parsers re-trigger automatically. **There is no `file_vec` / embedding table** — semantic search
was dropped (§4).

**Extraction caps (v1):** text/PDF bodies capped at 100 KB; PDFs at 200 pages; a curated subset of
media metadata (dimensions, camera make/model, duration, title/artist/album).

---

## 4. Query pipeline

**Shipped design (keyword / FTS).** FTS5 BM25 over `file_text` runs live on every keystroke, with
a **name-match tier**: a direct filename-column match ranks above any content/path-only hit
(`LEFT JOIN` to a name-only FTS match; `name_hit IS NULL` is the primary `ORDER BY` key, bm25 with
name>path>tags>content weights breaking ties within a tier). The tiering is in the `ORDER BY` so it
applies before `LIMIT`, which is how an exact-filename match wins "open my_photo.png". Filters
(kinds / tags / downloaded) layer in as SQL predicates; an empty query with active filters takes a
filters-only recency path. (Gotcha: FTS5 `MATCH` rejects a table alias, so the name-only match is a
derived table, not `alias MATCH ?`.) Target latency **≤1.5 s p50**.

**Dropped: on-device intelligence (2026-10-19).** The original design added a second stage — Apple
Foundation Models guided-generation producing a typed `SearchIntent` (terms / kinds / extensions /
time-range / meta-filters / exact-filename), CoreML sentence embeddings in `sqlite-vec`, a
two-stage progressive query with reciprocal-rank fusion (Stage B deferred to Enter), and a
prewarmed `LanguageModelSession` with a `query → SearchIntent` LRU cache. This was **removed from
the roadmap**: no on-device LLM / embedding stack, no `file_vec`, and the Xcode-workspace migration
that Swift↔C++ interop would have forced is off the table. The full design lives in git history
(the pre-2026-10-19 `PLAN.md`) if it's ever revived.

---

## 5. Sweep subsystem

**Threading & concurrency (locked 2026-10-07).** Coordination is by *topology + SQLite WAL*, never
an application-level lock over the DB:

- **Single writer, connection-per-role.** One *writer* thread owns the write connection and is the
  only thread that writes; full crawls and reconciliation run as batch jobs directly on it.
  Mutations from threads that can't touch the DB (the FSEvents watcher, the extractor) arrive via a
  **write-command queue** the writer drains. The IPC query handler and the extractor each use their
  own *read-only* connection. WAL lets readers run against the last committed snapshot concurrently
  with an in-flight write, so an extraction batch never blocks a query. A `shared_mutex` over the DB
  is explicitly rejected — redundant with WAL, and it would reintroduce the reader-blocks-on-writer
  stall WAL removes.
- **Extractor pulls, isn't pushed.** A single extractor thread discovers stale files on its own
  connection (`WHERE needs_extract=1`, indexed, freshest `indexed_at` first) rather than via a
  pushed job queue — durable and crash-resumable (the flag is the queue). It runs **lock-step** (one
  batch in flight, waits for the writer to apply before pulling again, bounding queue memory) and is
  **gated on `sweeping`** (parks during a full crawl). A worker pool is the upgrade path if
  single-thread throughput disappoints.
- **Primitives.** Both queues are `mutex` + `condition_variable`; status counters read by the
  `status` IPC are `std::atomic`.

**Pipeline:**

- **Initial crawl:** breadth-first per root, honoring exclusions (default globs: `node_modules`,
  `.git`, `*.app`, caches, `~/Library`). Stat-only pass populates `files` fast (names searchable
  within seconds); extraction follows behind. On folder-indexed roots (Desktop/Downloads/Documents)
  directories are upserted as `folder` rows; package bundles (`.app/.pages/.key/…`) are single
  entries, not descended into. The other roots index files only.
- **Extraction:** one thread pulls stale files, dispatches by type → extract → hands the payload to
  the writer, which writes `content`/`tags` + `file_meta` and clears `needs_extract` **guarded by
  `fingerprint`** (a file edited mid-extraction stays flagged; unsupported/empty/errored files are
  stamped done so nothing loops), in one transaction. Scope: plain text (txt/md/csv), PDF (PDFKit),
  image EXIF + audio/video tags (ImageIO/AVFoundation). Office docs, `rtf`, OCR → Phase 2.5.
- **Politeness:** extractor at `QOS_CLASS_UTILITY`; pauses under thermal pressure; on battery
  defers the backlog (`indexed_at ≥ now−7d`) so only recently-encountered files stay current until
  AC (`--pretend-battery` dev flag forces this path).
- **Live updates:** one FSEvents stream over all roots, 2 s coalescing. Events enqueue re-stat →
  re-extract if fingerprint changed; deletes set `missing=1` (purged at reconciliation).
- **Reconciliation:** nightly (and on daemon start after >24 h downtime): full re-walk, diff vs.
  index, catches FSEvents drops and offline changes. The crawl is the source of truth.

---

## 6. IPC protocol

Framing: 4-byte big-endian length + UTF-8 JSON. Every message: `{"v":1,"type":...}`.
Socket file mode 0600 (user-only — also the security boundary).

| type | direction | payload |
|---|---|---|
| `query` | app→d | `{id, text, filters}` |
| `results` | d→app | `{id, items:[{fileId, path, name, kind, score, snippet, tags, modifiedAt}]}` |
| `status` | app↔d | index counts, sweep progress |
| `action` / `reindex` / `config` | app→d | ⬜ not yet implemented (router handles `ping`/`status`/`query`) |

Open/reveal/copy are done app-side. Version mismatch → app offers daemon reinstall.

---

## 7. UI app details

- Menu-bar item (status, Default Editor submenu, quit); no Dock icon (`LSUIElement`).
- Hotkey ⚙ default **⌥Space** via `RegisterEventHotKey` (no Accessibility permission; rebindable).
- Panel: `NSPanel` (`.nonactivatingPanel`), centered, `NSVisualEffectView` blur, SwiftUI content.
  Escape dismisses; focus returns to previous app.
- Results: icon, name, parent folder, modified date, Finder color-tag dots. ↑↓ navigate,
  **Enter = reveal in Finder**, ⌘Enter = open a folder of all hits, ⌘C = copy path, ⌘E = open in
  editor, ⌘L = symlink to Desktop. A row click only highlights.
- **Filter toolbar:** always-visible filter row + in-field chips (Recently downloaded · Photos ·
  Videos · Music · Documents · Code · Tags ▾), layered OR/AND semantics.
- **Open in Editor:** ⌘E opens code/text/Markdown results in the user's editor — a terminal editor
  (vim default) inside a chosen terminal host (Terminal / iTerm / Ghostty / WezTerm / kitty), or a
  GUI editor (VS Code, Cursor, Zed, …). Configured from the menu-bar **Default Editor** submenu.
  Full design in `EDITOR_PLAN.md`.
- First-run onboarding (⬜ Polish): Full Disk Access deep-link → install launchd agent → initial
  crawl with progress.

---

## 8. Permissions, distribution, ops

- **Full Disk Access** granted to the daemon binary (one-time; onboarding handles the UX). The app
  itself needs no special TCC permissions (launching a terminal editor into Terminal/iTerm triggers
  an Automation prompt — see `EDITOR_PLAN.md`).
- Not sandboxed (arbitrary-filesystem indexing is the product). Hardened runtime, Developer ID
  signed, notarized; distributed as a DMG outside the App Store. (⬜ Polish.)
- launchd agent plist in `~/Library/LaunchAgents/com.finn.macassist.daemon.plist`: `RunAtLoad`,
  `KeepAlive`. App owns install/upgrade of the plist + daemon binary.
- Logs: `os_log` from both processes; `mactl status --verbose` for index health.

---

## 9. Repo layout & build

```
MacAssist/
  PLAN.md  PROGRESS.md  EDITOR_PLAN.md
  daemon/            # C++20; macassistd (+ macassistd_core static lib)
    src/{main,ipc,sweep,index,query}/     # pure C++ (main.cpp = thin shell on the lib)
    src/extract/     # .mm ObjC++ bridges (PDFKit, ImageIO, AVFoundation, xattrs) + plain_text.cpp
    tests/           # Catch2 (vendored single header) + golden fixtures; CTest target
  app/               # Swift app target (panel, menu bar, editor launch)
  shared/            # protocol constants/schemas shared by daemon, app, mactl
  tools/mactl/       # CLI client
  scripts/           # build / test / make-app / dev-run
```

Build: daemon + `mactl` + `daemon_tests` via **CMake** (C++20; `.mm` bridges compile as
ObjC++/ARC); the daemon logic lives in a `macassistd_core` static lib linked by both the executable
and the tests (`./scripts/test.sh` runs the suite via CTest). The Swift app builds with **SwiftPM**;
the two meet only over the socket protocol. Test framework: **Catch2 v2** (vendored, header-only).
Vendored deps: SQLite (custom build with FTS5), nlohmann JSON. The Xcode-workspace migration is
**not planned** — it was only needed for the dropped Swift↔C++ AI interop.

---

## 10. Phased roadmap

**Phase 1 — Vertical slice.** ✅ Hotkey → panel → daemon → name-only FTS → open, end-to-end.

**Phase 2 — Real indexing.** ✅ Background incremental sweeper (upsert + fingerprint-skip +
`missing` tombstones); configurable roots + exclusions; FSEvents live updates; reconciliation;
politeness; cheap extraction (PDF/plain-text + media tags). Test suite (Catch2 + CTest).

**Post-Phase-2 features.** ✅ Finder color tags (display-only); filter toolbar + `downloaded_at`
signal; folders & zips as first-class kinds; Open-in-Editor (`code` kind + ⌘E + configurable
terminal). See `PROGRESS.md`.

**Phase 2.5 — Deeper extraction.** ⬜ Vision OCR; office-doc (docx/pages/pptx/xlsx) + RTF body
text. Re-triggers on existing files via the schema-bump nuke-rebuild.

**Phase 3 — ~~Intelligence~~.** ❌ **Dropped** (2026-10-19) — see §4.

**Phase 4 — Product polish.** ⬜ Settings UI, onboarding + FDA flow, launchd install/upgrade,
QuickLook thumbnails, signing + notarization + DMG.

**Phase 5 — Stretch.** Voice input (Speech → same pipeline); opt-in audio/video transcription;
`action`/`reindex`/`config` IPC verbs; more file types.

---

## 11. Risks & mitigations

| Risk | Mitigation |
|---|---|
| Full Disk Access friction scares users off | Onboarding invests heavily here; app still works on TCC-free folders (~/Downloads etc.) before FDA granted |
| OCR/extraction makes first sweep slow & hot | Stat-first crawl (names searchable immediately), priority queue, thermal/battery pausing, caps per file |
| FSEvents drops events | Nightly reconciliation is the source of truth |
| Single extractor thread stalls on a pathological file | Page/size caps; subprocess isolation is a later hardening |
