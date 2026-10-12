# Brick 5 — Cheap Extraction

*Phase 2, brick 5 of 6. Plan doc; read alongside `PLAN.md` (authoritative design) and
`PROGRESS.md` (status). Drafted 2026-10-12. Revised 2026-10-12 after a design review that
closed a backpressure bug and two infinite-loop traps — see "Revision notes" at the end.*

Fill the two empty FTS columns (`content`, `tags`) and the `file_meta` table the schema
reserves, so search finally hits file **bodies** and media metadata — not just names/paths.

## Decisions (locked)

| Fork | Decision | Why |
|---|---|---|
| **Work discovery** | **Indexed `needs_extract` flag** (schema v4) + partial index | O(pending) discovery — the staleness predicate `content_fp <> fingerprint` can't use an index, so late in a 50k crawl every poll would full-scan. A flag with `WHERE needs_extract=1` stays cheap |
| **Backpressure** | **Lock-step, one batch in flight** | The flag is only cleared *after the writer applies*; without a wait the extractor re-selects and re-extracts the same rows, piling duplicate ≤100 KB payloads into the queue. Lock-step bounds queue memory to `N×100 KB` |
| **Terminal stamping** | **Always clear `needs_extract`, no retry** | Unsupported types, encrypted PDFs, empty files, and I/O errors all still clear the flag (guarded by fingerprint) so nothing loops forever. Phase 2.5 parsers re-trigger via the schema-bump nuke-rebuild |
| **Parallelism** | **Single extractor thread** | Simplest; matches "cheap" scope. Upgrade to a pool only if backlog throughput disappoints |
| **Scope** | **Text + PDF + media** | Full PLAN "cheap extraction" in one brick; two new `.mm` bridges |
| **Power gating** | **Thermal pause; on battery, defer the backlog** | Pause on serious+ thermal. On battery the pull query adds `indexed_at >= now − 7d` so newly *encountered* files stay current and the old archive waits for AC |

### Why there is no `content_fp` column
The first draft used a `content_fp TEXT` column (fresh iff `content_fp == fingerprint`) both
to *discover* stale rows and to *guard* the mid-extraction race. Choosing the indexed
`needs_extract` flag for discovery makes the separate column redundant: the race guard reuses
the **`fingerprint` already on `files`**. The extractor passes the fingerprint it saw; the
writer clears the flag with `... WHERE id=? AND fingerprint=?`. If the file changed
mid-extraction the fingerprint no longer matches, the clear no-ops, the flag stays set, and the
row is re-extracted — self-correcting regardless of command order in the batch.

### What `content` / `tags` / `file_meta` each hold
- `content` — body text (text-file contents, PDF text), capped at 100 KB per PLAN §3. FTS.
- `tags` — media metadata *flattened to searchable text* ("Radiohead OK Computer", camera model). FTS.
- `file_meta` — the *same* media metadata but typed key/value (`duration=214`, `width=4032`,
  `artist=…`) for display/filtering later. Media info lands in both — searchable vs structured.

## Topology (consequence of the locked single-writer model)

```
 ┌─ writer thread (RW conn) ──────────────┐     ┌─ extractor thread (own RO conn) ─┐
 │ crawl / FSEvents → UpsertFile          │     │ pull needs_extract=1 batch       │
 │ ins/upd set needs_extract=1            │ ──► │ read file off disk, extract      │
 │ ApplyExtraction: write FTS + file_meta │ ◄── │ enqueue kApplyExtract result     │
 │   + clear flag WHERE fingerprint=seen  │     │ WAIT for Notify (lock-step)      │
 │ Notify() extractor after apply/crawl   │ ──► │ …then pull the next batch        │
 └────────────────────────────────────────┘     └──────────────────────────────────┘
```

Extraction never touches the RW connection. It reads via a **third connection** (ReadOnly,
WAL lets readers run free) to find + read pending work, and funnels results back through the
existing write-command queue. **Lock-step:** after enqueuing a batch the extractor blocks until
the writer `Notify()`s that the batch was applied — one batch in flight, so it never re-selects
rows the writer hasn't cleared and queue memory is bounded.

## Steps

### Step 1 — Schema v4 (`database.cpp`)
- `files` gains `needs_extract INTEGER NOT NULL DEFAULT 1` (new rows need extraction).
- `CREATE INDEX idx_files_needs_extract ON files(id) WHERE needs_extract=1;` — partial index so
  discovery is O(pending), not a full scan.
- Add `file_meta(file_id INTEGER REFERENCES files(id), key TEXT, value TEXT,
  PRIMARY KEY(file_id,key))` — PLAN §3 reserves it but the current rebuild never created it.
- `DROP TABLE IF EXISTS file_meta;` added to the rebuild (the index drops with its table).
- Bump `kSchemaVersion` → 4 (nuke-and-rebuild, pre-release policy). `file_vec` stays Phase 3.

### Step 2 — Indexer freshness + apply path (`indexer.cpp`)
- Insert leaves `needs_extract` at its `DEFAULT 1` (`ins_` doesn't list it). `touch_` (unchanged)
  leaves it as-is. **`upd_` now also sets `needs_extract=1`** — an edited file re-extracts. *(the
  only change to an existing statement)*
- New `ApplyExtraction(const ExtractResult&)`:
  - `UPDATE file_text SET content=?,tags=? WHERE rowid=?` (FTS5 preserves `name`/`path_tokens`).
  - `DELETE FROM file_meta WHERE file_id=?` then re-insert the typed rows.
  - `UPDATE files SET needs_extract=0 WHERE id=? AND fingerprint=?` — the race guard.

### Step 3 — Extractors
- `extract/plain_text.cpp` — pure C++, read first 100 KB (PLAN cap). Covers `txt/md/csv`.
- `extract/pdf_text.mm` — PDFKit; iterate pages, concat text, **stop at 100 KB or 200 pages**;
  skip locked PDFs (encrypted-but-unlocked is fine to read).
- `extract/media_tags.mm` — ImageIO for images (→ `file_meta`: pixel dimensions, camera
  make/model; `tags`: make/model); AVFoundation for audio/video (→ `file_meta`: duration;
  `tags`: title/artist/album/creator/type).
- **Deferred (content stays empty, still name-searchable):** `rtf` and all office formats
  (`doc/docx/pages/key/ppt/xls…`) — those are the Phase 2.5 parsers. They are still pulled and
  **stamped done** (empty) so they don't loop.
- Extractors never throw; on any failure they leave outputs empty and the file is stamped done.

### Step 4 — Extractor class (`extract/extractor.{hpp,cpp}`)
Own thread at **`QOS_CLASS_UTILITY`** (matching the writer — `BACKGROUND` throttles disk I/O on
macOS and would starve a file-reading loop). Loop:

```
if stop: return
while politeness.ShouldPauseSweep():  WaitForWork(1s); recheck   # thermal pause
batch = SELECT id,path,ext,kind,fingerprint FROM files
        WHERE missing=0 AND needs_extract=1
        [+ AND indexed_at >= now−7d  when on battery]           # step 6
        ORDER BY indexed_at DESC LIMIT N
if empty:  WaitForWork(idle_timeout)                            # backstop re-poll
else:      for each → dispatch by ext/kind → enqueue kApplyExtract(id, …, fingerprint)
           WaitForWork(apply_timeout)                           # LOCK-STEP: wait for apply
```

`WaitForWork` waits on the extractor's own cv with a predicate (`stop || notified`), clearing
`notified` on wake — robust against lost wakeups (a `Notify()` between enqueue and wait is not
missed). The writer `Notify()`s after applying a batch and after each crawl.

### Step 5 — Queue + writer wiring
- `WriteCommand` gains `kApplyExtract` + a `std::shared_ptr<ExtractResult>` (keeps the common
  FSEvents command small; only extract carries the ≤100 KB payload). Added as the **last**
  member so the existing aggregate-init call sites still compile.
- `ApplyBatch` **branches on `kind`**: `kApplyExtract` → `idx.ApplyExtraction(...)`;
  else the existing upsert/tombstone. (Without the branch a `kApplyExtract`'s empty `path` would
  fall into `stat("")→kMissing→TombstoneByPath("")`.) A co-batched `kFullRescan` still supersedes
  and drops any extract results in that batch — harmless, they stay flagged and re-extract.
- `Writer::Start` constructs + starts the `Extractor` after `Open`+`Migrate` (schema exists → RO
  conn is safe), passing an enqueue callback + politeness. Writer `Notify()`s the extractor after
  `ApplyBatch` and after each `RunFullCrawl`.
- `Writer::Stop` stops the watcher, then **stops *and joins* the extractor before** setting the
  writer's `stop_` — otherwise the extractor could keep enqueuing and the writer's
  "drain until empty, then exit" loop would never terminate.

### Step 6 — Battery backlog
On battery the pull query adds `AND indexed_at >= (now − 7·86400)` — files newly *encountered by
the index* stay current regardless of their on-disk mtime; the old archive waits for AC. On AC
the filter drops and it drains everything. The idle re-poll picks the backlog up when you plug
in. Dev-testable via a hidden `--pretend-battery` flag (sets a `Politeness` field, read once
before threads start — no atomic needed; `OnBattery()` then returns true).

### Step 7 — Observability + CMake + verify
- `ExtractStatus` atomics (`extracted`, incremented by the writer as it applies each result).
- `Database::CountPending()` → `SELECT count(*) FROM files WHERE missing=0 AND needs_extract=1`;
  router reports `extractedFiles` + a live `pending` count alongside `indexedFiles`.
- CMake: add sources, ARC flags on the two `.mm`, and link each framework as its own token
  (`"-framework PDFKit"` `"-framework ImageIO"` `"-framework AVFoundation"`
  `"-framework CoreMedia"` `"-framework CoreGraphics"` — ImageIO needs CoreGraphics).
- Verify on a throwaway corpus: a `.txt`/`.md` with a unique body token, a text PDF, an mp3
  with ID3, a jpg with EXIF → `mactl query <bodyword>` / `<artist>` hits them; edit a file →
  flag goes stale → re-extracted; `--pretend-battery` leaves the old backlog unextracted until
  toggled off.

## Open judgment calls (defaulted — flag if you disagree)
- **No per-file extraction timeout / subprocess isolation** in v1 — a pathological PDF could
  stall the single extractor thread. Page/size caps are the mitigation; true isolation is a
  later hardening note.
- **Stale body text during re-extraction.** `upd_` changes `fingerprint` but leaves the old
  `content`/`tags` in `file_text` until re-extraction completes, so an edited file serves old
  body text for a brief window. Conscious call: the file stays findable; we don't blank on edit.
- **The extractor's third connection is ReadOnly** — it only reads `files`; all writes go
  through the writer.

## New / changed files
| File | Change |
|---|---|
| `daemon/src/index/database.{cpp,hpp}` | Schema v4: `needs_extract` + partial index + `file_meta`; bump version; `CountPending()` |
| `daemon/src/sweep/indexer.{hpp,cpp}` | `upd_` sets `needs_extract=1`; `ApplyExtraction(...)` + statements |
| `daemon/src/extract/text_extractors.hpp` | Declares extractors + `ExtractResult` |
| `daemon/src/extract/plain_text.cpp` | Plain-text (C++) |
| `daemon/src/extract/pdf_text.mm` | PDFKit (ARC) |
| `daemon/src/extract/media_tags.mm` | ImageIO + AVFoundation (ARC) |
| `daemon/src/extract/extractor.{hpp,cpp}` | Extractor thread: pull loop, lock-step, dispatch, politeness |
| `daemon/src/sweep/writer.{hpp,cpp}` | `kApplyExtract`, extractor lifecycle, `Notify()` |
| `daemon/src/sweep/sweep_status.hpp` | `ExtractStatus` |
| `daemon/src/sweep/politeness.{hpp,mm}` | `--pretend-battery` field |
| `daemon/src/ipc/request_router.{hpp,cpp}` | Report `extractedFiles` + `pending` |
| `daemon/src/main.cpp` | Hidden `--pretend-battery` dev flag |
| `daemon/CMakeLists.txt` | New sources, ARC flags, framework links |

## Revision notes (what the 2026-10-12 review changed)
1. **Discovery:** `content_fp` column → indexed `needs_extract` flag (+ partial index). The race
   guard moved onto the existing `fingerprint`, so `content_fp` was dropped entirely.
2. **Backpressure bug fixed:** added lock-step (one batch in flight). The original loop
   re-selected not-yet-applied rows and would re-extract them, piling duplicate payloads.
3. **Two infinite-loop traps closed:** unsupported/encrypted/empty/errored files now always clear
   the flag (guarded by fingerprint), so `pending` actually reaches zero.
4. **Smaller fixes:** `ApplyBatch` branches on `kind`; QoS `UTILITY` not `BACKGROUND`; CMake links
   each framework as its own token and adds CoreGraphics; battery "fresh" keyed on `indexed_at`.
</content>
</invoke>
