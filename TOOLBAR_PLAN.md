# Filter Toolbar — Implementation Plan

A small, always-visible toolbar under the search bar offering broad, layerable
filter searches. Selecting a filter adds a rounded chip to the front of the
search field. Filters stack, and typed text narrows on top of whatever filters
are active.

## Decisions (locked)

| Area | Decision |
|------|----------|
| "Recently downloaded" data | Index a real `downloaded_at`, primarily from Spotlight's `kMDItemDownloadedDate` with the `com.apple.quarantine` xattr as a fast path (schema v5 → v6). See §1. |
| Layering semantics | Same-type **OR**, cross-type **AND**; typed text narrows further. |
| Tags UI | A single **Tags ▾** button opening a color popover. |
| Toolbar placement | Always-visible row under the field; active filters render as chips inside the field. |
| Default sort (no typed text) | Most recent first (`modified_at DESC`; `downloaded_at DESC` when the Recent filter is active). With text: bm25 relevance. |
| v1 button set | Recently downloaded · Photos · Videos · Music · Documents · Tags ▾ |
| Chip interaction | Toolbar button toggles; chip `×` removes; backspace on empty field removes the last chip. |
| Persistence | Clear everything on each panel open (matches today's `reset()`). |

### Flagged implementation defaults
- **Tag filtering uses SQLite `json_each`** over `tags_json` (robust JSON parse,
  natural multi-tag + OR handling) rather than a fragile `LIKE`. JSON is
  confirmed available: the vendored amalgamation is SQLite 3.46.1, which builds
  the JSON functions in by default (no `SQLITE_OMIT_JSON` in the build), so no
  C++-side fallback is needed. The empty-`tags_json` guard in §2 is still
  required regardless.
- **Tags popover shows all 7 Finder colors** (fixed set, no extra daemon call).
  A "only colors actually in use" variant is a fast-follow needing a new
  distinct-tags query.
- **Download date: `kMDItemDownloadedDate` (Spotlight) is the source of truth,
  quarantine xattr is a fast path.** The xattr is volatile (cleared on approve/
  open, absent for pre-existing downloads), so read it first only to skip a
  Spotlight call when present+parseable, and fall back to `kMDItemDownloadedDate`
  whenever it's missing or unparseable. Only files with neither signal stay
  `NULL`. A known date is never overwritten with `NULL` on a later stat. See §1.
- **`downloaded_at` is a filter/display column only** — not fed into FTS
  (mirrors how `tags_json` is handled today).
- **Empty-text + filters is now a valid query** (today the app and query engine
  short-circuit on empty text and return nothing).

---

## 1. Daemon — index the download signal (schema v5 → v6)

- **`daemon/src/index/database.cpp`**: bump `kSchemaVersion` to 6; add
  `downloaded_at INTEGER` to the `files` table (NULL = not downloaded). The
  existing version-mismatch path already wipes and rebuilds the schema, so the
  first launch after this update performs a one-time full reindex — expected
  and consistent with the current destructive-by-design behavior.
- **Indexes for the recency sort.** The empty-text path (§2) orders `files` by
  `modified_at`/`downloaded_at`; at the PLAN's ~50k-file target an unindexed sort
  runs on every filter toggle. Add in the rebuild SQL:
  `CREATE INDEX idx_files_modified_at ON files(modified_at)` and a partial
  `CREATE INDEX idx_files_downloaded_at ON files(downloaded_at)
  WHERE downloaded_at IS NOT NULL`.
- **Where it's resolved: the extractor thread, not the crawl/`UpsertFile`
  path.** Resolution needs a Spotlight lookup (below), which is too costly to run
  synchronously for every file during the stat-first crawl (it would tax the
  very path the architecture keeps lean). Instead `UpsertFile` leaves
  `downloaded_at` NULL; the extractor — which already visits every file once
  (`needs_extract=1`), off the writer thread and thermal/battery-gated —
  resolves it via a new bridge (`extract/download_date.mm`) and returns it on
  `ExtractResult`, and the writer's `ApplyExtraction` persists it. So the date
  fills in progressively alongside content/tags, and a content change (which
  re-sets `needs_extract`) re-resolves it. The one cost is that
  "Recently downloaded" is empty until the initial extraction drains — the same
  progressive-fill behavior as extracted body text.
- **Source of truth is `kMDItemDownloadedDate` (Spotlight), with the quarantine
  xattr as a fast path.** The xattr is *volatile* — it is cleared once a file is
  approved/opened and is absent for anything downloaded before indexing began —
  so treating it as primary would leave most real downloads `NULL`.
  `kMDItemDownloadedDate` (via `MDItemCreate` / `MDItemCopyAttribute`) is durable
  and is the authoritative value; read the xattr first only to avoid a Spotlight
  call when it's present and parseable, and fall back to `kMDItemDownloadedDate`
  whenever the xattr is missing or unparseable.
- **Quarantine xattr format:** the value is `flags;hex-epoch;agent;uuid`
  (e.g. `0081;5f3e9c40;Safari;…`). The timestamp is the **second** `;`-separated
  field, hex-encoded **epoch seconds** — not a plist date. Split on `;`,
  hex-decode field 2. Anything that doesn't parse → fall back to Spotlight.
- **Never clobber a known date with NULL.** `ApplyExtraction` only *sets*
  `downloaded_at` when the extractor resolved a value; a file with no signal, or
  one whose volatile xattr later vanished, must not overwrite an existing
  non-NULL `downloaded_at` back to NULL.
- Files that are genuinely not downloads stay `NULL`. This is display/filter
  only — not added to the FTS `file_text` table.

## 2. Daemon — query engine filters (`daemon/src/query/query_engine.cpp`)

- `SearchFiles` grows a `QueryFilters` parameter:
  `kinds` (set of strings), `tag_colors` (set of ints), `downloaded_only` (bool).
  **Default it to an empty `QueryFilters{}`** so existing callers compile
  unchanged — `request_router.cpp` and the `query_engine` Catch2 tests call
  `SearchFiles` today and must keep building without edits until Brick B wires
  the filters through the router.
- **Combine logic** (same-type OR, cross-type AND):
  - `kinds` → `f.kind IN (…)` (values are canonical kinds —
    `image|audio|video|document` — not UI labels; see §4 for the mapping)
  - `tag_colors` → guard the JSON walk, then
    `EXISTS (SELECT 1 FROM json_each(f.tags_json)
    WHERE json_extract(value, '$.color') IN (…))`.
    **Guard is mandatory:** untagged files store `tags_json = ''` (the indexer
    binds `ReadFinderTagsJson()`'s empty string, not NULL — see `indexer.cpp`),
    and `json_each('')` raises *"malformed JSON"* and aborts the entire query.
    So the predicate is
    `f.tags_json IS NOT NULL AND f.tags_json <> '' AND json_valid(f.tags_json)
    AND EXISTS (…)` — `json_each(NULL)` is safe but `json_each('')` is not.
  - `downloaded_only` → `f.downloaded_at IS NOT NULL`
  - The three groups are ANDed together, and ANDed with the FTS MATCH when text
    is present.
- **Two code paths for the empty-text case** (today returns nothing):
  - *Text present*: current FTS query + filter predicates in `WHERE`, ordered by
    bm25 as now.
  - *No text, filters only*: query `files` directly (no FTS join),
    `WHERE` = filter predicates, `ORDER BY downloaded_at DESC` when the Recent
    filter is active else `modified_at DESC`, then `LIMIT`.

## 3. Protocol (both sides)

- **`shared/include/macassist/protocol.hpp` + `Protocol.swift` `QueryRequest`**:
  add a `filters` object — `{ kinds: [String], tagColors: [Int], downloaded: Bool }`.
  Omitted/empty preserves today's behavior, so older messages stay valid.
  `FileHit` optionally gains `downloadedAt` (not required for the v1 UI).
- **`DaemonClient.query`** accepts the filter set and includes it in the
  payload. Empty text + any filter is now a valid request (previously
  short-circuited).

## 4. Swift model (`SearchModel.swift`)

- New `@Published var activeFilters: [Filter]`, where `Filter` is an enum:
  `.recentlyDownloaded`, `.kind(String)`, `.tag(color: Int)`.
- **`.kind` carries the canonical DB kind, never the UI label.** `files.kind`
  (see `file_kind.cpp`) is one of `image | audio | video | document`. The toolbar
  labels are display-only and map to those values:
  `Photos → image`, `Videos → video`, `Music → audio`, `Documents → document`.
  The model emits the canonical kind so `f.kind IN (…)` matches; sending the
  label (`"music"`, `"photos"`) would silently return nothing.
- The debounce pipeline fires `run` on changes to **either** `queryText` **or**
  `activeFilters`; `run` no longer bails on empty text when filters exist. It
  translates `activeFilters` → the protocol filter object.
- `reset()` also clears `activeFilters` (persistence = clear everything on open).
- Helpers: `toggle(_ filter)`, `remove(_ filter)`, `removeLast()` (backspace).

## 5. UI (`SearchView.swift` + `PanelMetrics.swift`)

- **Field with chips**: wrap the magnifier + chip row + `TextField` in one
  rounded container that reads as the search field. Chips render to the left of
  the cursor — a rounded-rect capsule per active filter (label + `×`), with a
  color swatch for tag chips.
- **Toolbar row** under the field: always-visible `HStack` of buttons —
  `[Recently downloaded] [Photos] [Videos] [Music] [Documents] [Tags ▾]`.
  These are display labels only; each maps to a canonical kind per §4
  (Photos→image, Videos→video, Music→audio, Documents→document).
  Active buttons show a filled/selected state. "Tags ▾" opens a popover with
  all 7 Finder color dots; each pick adds a colored chip.
- **Interaction**: button click toggles its chip; chip `×` removes; backspace on
  an empty field removes the last chip — handled in PanelController's existing
  key monitor so it composes with the current arrow/Enter/Esc handling.
- **`PanelMetrics`**: add `toolbarHeight` and include it in the panel height
  math so results aren't clipped.

## 6. Tests (`daemon/tests`, Catch2)

- `SearchFiles` with: kinds-only, tag-only, downloaded-only, mixed
  (kind OR + tag AND), and text + filters.
- Assert a `kinds={"audio"}` filter returns the audio rows (guards the
  Music→audio canonical-kind mapping; a UI label would match nothing).
- Assert the empty-text path returns recency-ordered rows.
- Run the tag filter over a corpus mixing tagged and **untagged** files
  (`tags_json = ''`) and assert it returns only the matching tagged rows without
  erroring (guards the `json_each('')` abort).
- Assert `downloaded_at` extraction: a known `flags;hex-epoch;agent;uuid`
  quarantine xattr hex-decodes to the right epoch; a file with no quarantine
  xattr but a `kMDItemDownloadedDate` resolves via the Spotlight fallback; a
  plain file resolves to `NULL`.
- Assert a later extraction that resolves no date (0) does **not** reset an
  already-recorded `downloaded_at` to NULL (`ApplyExtraction` never-clobber).

---

## Brick sequence

1. **Brick A** — schema v6 + `downloaded_at` extraction (daemon only;
   verifiable via `mactl query` / Catch2 tests).
2. **Brick B** — query-engine filters + protocol fields + `DaemonClient`
   (headless-testable).
3. **Brick C** — `SearchModel` filter state + toolbar/chips UI.
   The app has no unit-test target today, so Brick C is verified manually
   against a running daemon. Checklist:
   - Toolbar button click adds its chip; clicking again removes it (toggle).
   - Chip `×` removes that filter; backspace on an empty field removes the
     last chip.
   - "Tags ▾" popover pick adds a colored tag chip.
   - With the field **empty** and a filter active, a query fires and returns
     recency-ordered results (not the old empty-text no-op).
   - Typing narrows the active filter set; `reset()` on panel reopen clears
     both text and chips.
   - Toolbar height is accounted for — results are not clipped in any state
     (empty, filters-only, filters + results).
