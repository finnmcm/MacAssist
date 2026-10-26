# MacAssist — Progress Tracker

*Living status doc. Read alongside `PLAN.md` (the authoritative design). Last updated 2026-10-26.*

MacAssist is a "smarter Finder" for macOS: a background daemon keeps a rich index of the user's
files; a Spotlight-style ⌥Space panel answers queries in under ~2 s. See `PLAN.md` for the design.

---

## Status at a glance

| Phase | Scope | Status |
|---|---|---|
| **Phase 1** | Vertical slice: hotkey → panel → daemon → filename FTS → open | ✅ Done |
| **Phase 2** | Real indexing: live sweeper + content extraction + test suite | ✅ Done |
| **Filter toolbar** | Layerable filters (kinds/tags/downloaded) + `downloaded_at` signal | ✅ Done |
| **Folders & zips** | Directories + `.zip` as first-class `folder`/`archive` kinds (schema v7) | ✅ Done |
| **Open in Editor** | `code` kind + ⌘E opens code/text/MD in the user's editor (vim default); configurable default terminal (Terminal/iTerm/Ghostty/WezTerm/kitty) | ✅ Done |
| **Phase 2.5** | Deeper extraction: Vision OCR, office docs (docx/pages), RTF | ⬜ Planned |
| **Polish** | Settings UI, onboarding + FDA flow, launchd, signing/notarization/DMG | ⬜ Planned |
| ~~**Intelligence**~~ | ~~AFM `SearchIntent` + CoreML embeddings + two-stage query~~ | ❌ **Dropped** (2026-10-19) |

**Current branch:** `main` (all of the above merged). **Schema version:** v7.

> **Direction (2026-10-19):** the Apple Intelligence phase (on-device AFM guided generation +
> CoreML embeddings + semantic/two-stage search) is **off the roadmap**. Focus is new product
> features on top of the keyword/FTS + filter foundation. See **What's next**.

---

## Facets / subsystems

### Daemon — `macassistd` (C++20, CMake)
| Module | Responsibility | Status |
|---|---|---|
| `ipc/socket_server` | Unix-socket accept loop, length-prefixed JSON frames | ✅ (single conn at a time) |
| `ipc/request_router` | Routes `ping`/`status`/`query` (incl. `filters`) | ✅ (`action`/`reindex`/`config` ⬜) |
| `index/database` | SQLite open, WAL, schema+migrations, connection roles, meta get/set | ✅ schema v7, writer/reader split |
| `sweep/crawler` | Incremental walk (upsert + fingerprint-skip + tombstone); folders as `folder` rows on opted-in roots | ✅ |
| `sweep/dir_filter` | Shared dir classification: excluded dirs, package bundles, inside-bundle test | ✅ |
| `sweep/indexer` | Shared per-file/dir upsert / tombstone (`UpsertFile`/`UpsertDir`/`UpsertPath`) | ✅ |
| `sweep/writer` | Long-lived writer thread: owns RW conn, runs crawl + reconciliation, drains command queue | ✅ |
| `sweep/fs_watcher` | FSEvents live updates (ObjC++/`.mm`, GCD queue) | ✅ |
| `sweep/politeness` | Thermal (NSProcessInfo) + battery (IOKit) sensors; crawl throttle | ✅ |
| `query/query_engine` | FTS5 BM25 over name/path/content/tags; name matches tiered above content-only; filter predicates + empty-text filters-only recency path | ✅ |
| `extract/` | Extractor thread (own RO conn, lock-step) + bridges: plain text, PDFKit, ImageIO/AVFoundation tags, Finder color tags, download date (quarantine xattr → Spotlight) | ✅ (OCR + office docs → Phase 2.5) |

### App — `MacAssist.app` (Swift, SwiftPM)
Menu-bar resident, global ⌥Space hotkey, non-activating blurred `NSPanel`, debounced live search.
✅ Filter toolbar (always-visible row + in-field chips, layered OR/AND, backspace-removes-last-chip).
✅ Open in Editor (⌘E, vim default, menu-bar **Default Editor ▸** with a Terminal App ▸ host
picker). A result-row click only highlights. Full Settings UI, onboarding, FDA flow ⬜ Polish.

### `mactl` (CLI) — `status`, `query <text>` work. ✅ Primary dev/test harness.

### Tests — `daemon/tests/` (Catch2 + CTest)
One `daemon_tests` binary over `macassistd_core` (daemon logic is a static lib shared by tests and
the executable). ~40 cases / ~267 assertions covering `file_kind`, `database` (schema/migration/
meta/counts/RO guard), `indexer` (upsert/fingerprint/tombstone/`needs_extract`/`ApplyExtraction`
race-guard), `crawler`, `query_engine` (incl. the name-match tier + filters), `plain_text`,
`frame`, `protocol`, `request_router`, and a threaded `Writer`+`Extractor` integration test. Golden
fixtures (`report.pdf`/`camera.jpg`/`song.m4a`) exercise the extraction bridges. Run
`./scripts/test.sh`. Catch2 v2 single header vendored at `third_party/catch2/`. *(App half has no
Swift test target; verified manually per repo convention.)*

### Build & distribution
CMake (daemon + `mactl`) + SwiftPM (app) via `scripts/build.sh`; `.app` bundling via
`scripts/make-app.sh`. ⬜ launchd agent, code signing, notarization, DMG (Polish). The
Xcode-workspace migration is **not planned** (it was only for the dropped AI interop).

---

## What we've done

### Open in Editor (`code` kind + ⌘E + configurable terminal)
Open any code / text / Markdown result in the editor of your choice with **⌘E**, straight from the
panel. Full design + Devil's Advocate review in `EDITOR_PLAN.md`.
- **New `code` kind.** `file_kind.cpp` classifies ~54 source + config/markup extensions
  (`py/js/ts/swift/...` + `json/yaml/xml/html/css/...`) as `kind=code`. These were **previously
  skipped by the indexer entirely** (unknown extension → `kSkipped`), so this is what makes source
  files searchable at all. Config/markup are intentionally `code`, not `document` (code icon + Code
  filter, excluded from Documents); `md`/`csv`/`txt` stay `document`. Additive — no schema bump.
- **Editor + terminal preference (app-side, `UserDefaults`).** `EditorSettings` holds a curated
  `EditorChoice` catalog (terminal: vim/nvim/nano/emacs · GUI: VS Code/Cursor/Zed/Sublime/Xcode/
  TextEdit) and a curated `TerminalHost` catalog (Terminal · iTerm · Ghostty · WezTerm · kitty).
  Both are install-filtered by bundle id via LaunchServices (the active selection stays visible even
  if uninstalled); reached from the menu-bar **Default Editor ▸** submenu (rebuilt on open).
- **Launch engine (`EditorLauncher`).** GUI via `open -a "<App>" <path>`. Terminal editors run in a
  host chosen by **launch strategy**: Terminal.app/iTerm via an `osascript` AppleScript that `cd`s
  into the file's folder and runs `<cmd> <path>` (the only hosts that trip the TCC Automation
  prompt); Ghostty/WezTerm/kitty via `open -n <appURL> --args <prefix> sh -lc "<cmd>"`, the app
  resolved by bundle id (no PATH, no Automation prompt), an unresolvable id → `appNotFound` *before*
  `open` (its exit status only reflects the hand-off). The `--args` vector is built by a pure
  `EditorLauncher.cliArgs`. Failures are returned and shown as a subtle panel status. Runs off the
  main thread (the first AppleScript launch blocks on the TCC prompt).
- **Action wiring.** `SearchModel.openInEditor()` gates on eligibility (`kind==code` OR a plain-text
  extension — pdf/docx excluded), launches, dismisses on success. ⌘E bound in `PanelController`;
  `Enter` still reveals, `⌘Enter` still opens the all-results folder. Added the **Code** toolbar
  filter + `</>` icon.
- **Verified.** Daemon: `.py/.swift/.go/.json/.html/.css` index as `code` and are name-searchable;
  `md/csv/txt/pdf` stay `document`, unknown `.xyz` still skipped; `pending → 0`. App builds clean;
  Ghostty launch confirmed live end-to-end. WezTerm/kitty recipes + bundle ids (`com.github.wez.
  wezterm`, `net.kovidgoyal.kitty`) are plausible but unverified (apps absent) — on the manual
  checklist.

### Filter toolbar (schema v6 + query filters + UI) — `09c99e3`
A broad, layerable filter toolbar under the search field; active filters render as chips inside the
field and stack with typed text. Shipped as bricks A–C:
- **Schema + signal.** New `files.downloaded_at`, resolved **in the extractor** (not the hot stat
  path) via `extract/download_date.mm`: quarantine xattr (hex-decode field 2) as the fast path →
  `kMDItemDownloadedDate` (Spotlight) fallback; `ApplyExtraction` **never clobbers a known date with
  NULL**. Partial index on `downloaded_at` + index on `modified_at`. *Empirical caveat: in practice
  the quarantine xattr carries the feature (~97% of `~/Downloads`), not Spotlight — see caveats.*
- **Query filters + protocol.** `SearchFiles` takes a `QueryFilters` (defaulted empty): `kinds` →
  `f.kind IN (…)`; `tag_colors` → guarded `json_each(tags_json)` EXISTS walk (the `tags_json <> ''
  AND json_valid(...)` guard is **mandatory** — `json_each('')` aborts the whole query);
  `downloaded_only` → `downloaded_at IS NOT NULL`. Same-type **OR**, cross-type **AND**, ANDed with
  FTS MATCH when text is present. New **empty-text filters-only path** orders by `downloaded_at
  DESC` else `modified_at DESC`. Protocol gains a `filters` object.
- **App state + UI.** `activeFilters: [Filter]` (`.recentlyDownloaded` / `.kind(String)` /
  `.tag(color:)`); **`.kind` carries the canonical DB kind** (`image|audio|video|document`), never
  the UI label (Photos→image, Music→audio). Debounce fires on text **or** filter changes and no
  longer bails on empty text when filters exist. `SearchView` renders the toolbar + in-field chips;
  backspace removes the last chip; `reset()` clears filters on reopen.
- **Tests:** kinds/tag/downloaded/mixed/text+filters, the Music→audio mapping, the untagged
  `json_each('')` guard, the empty-text recency order, the quarantine hex-decode + Spotlight
  fallback + never-clobber-with-NULL invariant.

### Folders & zips as first-class results (schema v7)
Two canonical kinds — **`folder`** and **`archive`** — so directories and `.zip` files show up with
their own icons (`folder` / `doc.zipper`).
- **Zips → `archive`** (one entry in `file_kind.cpp`); archive file only, contents not indexed;
  indexed in every root; picks up `downloaded_at`.
- **Folders → `folder`, scoped to Desktop/Downloads/Documents.** The crawl used to skip directories;
  now, on a folder-indexed root, it upserts the directory (`needs_extract=0` so folders never enter
  the extraction backlog) and keeps recursing. Movies/Music/Pictures index files only. `main.cpp`
  marks folder roots by basename; `--index-folders` opts every root in (dev).
- **Indexer refactor.** `UpsertFile`/`UpsertDir`/`UpsertPath` share one `UpsertStat`;
  `needs_extract` is a bound parameter. **Package bundles are single entries:** new
  `sweep/dir_filter.{hpp,cpp}` centralizes dir classification so the crawl and FSEvents agree — on
  folder roots a `.app/.pages/.key/…` is one `folder` row, not descended into; FSEvents skips its
  internals too. Live create/rename/delete reflects within ~2 s. No protocol change.
- **Tests + verification:** `.zip → archive`, `UpsertDir` → folder/`needs_extract=0`, the
  `UpsertPath` index-folders gate, folder-only-on-opt-in crawl, and bundle-as-single-entry (internal
  `preview.jpg` not indexed). Verified live on a throwaway corpus.

### Finder color tags in results — display-only (schema v5)
A tiny ObjC++ bridge (`extract/finder_tags.mm`) reads the `com.apple.metadata:_kMDItemUserTags`
xattr into a compact JSON array in `files.tags_json`. Read on **every stat** in
`Indexer::UpsertFile`, not during extraction — setting a tag changes neither mtime nor size, so the
fingerprint can't see it; the "unchanged" branch rewrites `tags_json` only when it differs. Carried
on `FileHit` → structured `tags` array per result → colored dots in result rows (`TagDots`). Not fed
into FTS. Verified: a tag-only change propagates within ~2 s (FSEvents → re-stat) despite an
unchanged fingerprint.

### Phase 2 — real indexing (bricks 1–6)
Developed on `phase2-indexing`, merged to `main`. Full history in git.
- **Schema v2 + connection-per-role** (`a465e26`). `Database::Open(path, Access)`: one RW (writer)
  + one RO (query) connection; WAL coordinates them. New `roots`/`exclusions`; `files` gains
  `root_id`, `missing`.
- **Background writer + incremental crawl** (`bb8d47f`). Crawl moved onto a long-lived `Writer`
  thread. Destroy-and-rebuild → path-keyed upsert with a monotonic generation counter
  (new→insert, changed-by-fingerprint→update, unchanged→skip, unseen→tombstone). Schema v3 adds
  `seen_gen`.
- **FSEvents live updates + command queue** (`06db3bb`). `FsWatcher` (first ObjC++ bridge) streams
  changes on a GCD queue; the writer drains a `WriteCommand` queue. Idempotent re-examine-path
  (stat→upsert or tombstone); drop/overflow→full rescan. Shared `Indexer` extracted.
- **Reconciliation + politeness** (`5b56c94`). The drain loop became `wait_until(next_reconcile)`;
  when idle for a full interval it runs a reconciliation crawl via `RunFullCrawl(reason)` (shared by
  initial crawl, reconcile, FSEvents full-rescan). `--reconcile-interval` (default 24 h). Writer at
  `QOS_CLASS_UTILITY`; `Politeness` reads thermal + battery; crawl pauses under thermal pressure.
  Startup always runs a full crawl; the watcher uses `sinceNow` (no replay).
- **Cheap extraction** (`7b88770`). A dedicated `Extractor` thread (third, RO connection) discovers
  stale files via the indexed `needs_extract` flag (schema v4), extracts off the writer thread, and
  funnels results back through the write-command queue (`kApplyExtract`). Lock-step + sweeping gate
  bound the queue; the writer clears the flag guarded by `fingerprint`. Bridges: `plain_text.cpp`
  (txt/md/csv), `pdf_text.mm` (PDFKit), `media_tags.mm` (EXIF + AV tags). On battery the backlog is
  deferred to AC (`indexed_at ≥ now−7d`; `--pretend-battery`). RTF + office docs + OCR → Phase 2.5.
- **Test suite** (`00ac393`). Daemon logic became a static lib (`macassistd_core`); `main.cpp` is a
  thin shell. Catch2 v2 vendored; see the Tests facet.
- **Query ranking — name-match tier** (during testing). `SearchFiles` tiers a direct filename match
  above any content/path-only hit (`LEFT JOIN` to a name-column match; `name_hit IS NULL` primary
  `ORDER BY`, bm25 ties within a tier), applied before `LIMIT`. Gotcha: **FTS5 `MATCH` rejects a
  table alias** — the derived-table `LEFT JOIN` avoids it (an aliased `MATCH` silently zeroed
  results).

**Verified** (ad-hoc, via `mactl` + `sqlite3`): a fresh crawl indexes all; a re-crawl re-indexes
only changed/new files and tombstones deletes; with the daemon running, live create/delete/rename/
modify reflect within ~2 s; body-text/PDF/EXIF/audio tokens all hit; `file_meta` holds typed
metadata; `pending` drains to 0 incl. deferred `.rtf` (stamped, not looped); editing a file
re-extracts within ~2 s; and on battery an old-`indexed_at` file stays deferred while fresh files
extract.

---

## What's next

Features, not Apple Intelligence (the AFM/embeddings/two-stage phase was dropped 2026-10-19 — see
the Direction note above; `PLAN.md` §4 keeps the rationale).

### Feature backlog (unordered — pick next)
- **Open-in-Editor follow-ons** (see `EDITOR_PLAN.md` §7):
  - **Hide uninstalled *terminal editors*.** GUI editors and terminal *hosts* are install-filtered
    by bundle id; terminal *editors* (vim/nvim/…) still aren't — an `LSUIElement` process has a
    restricted PATH and the command only needs to exist in the terminal's shell. A best-effort
    probe (e.g. querying the login shell) is the open piece.
  - **Code content search ("grep").** Whitelist code extensions in the `plain_text` extractor so
    code bodies enter FTS (code is name-only today).
- **Phase 2.5 — deeper extraction:** Vision OCR for image-only PDFs/images; office-doc (docx/pages/
  pptx/xlsx) + RTF body text. Re-triggers via the schema-bump nuke-rebuild.
- **Filter toolbar follow-ons:** "only colors actually in use" Tags popover (needs a distinct-tags
  query); more filter kinds (date ranges, size, specific folders/roots).
- **Richer result interactions:** Quick Look preview, drag-out, more per-row actions.
- **`action` / `reindex` / `config` IPC:** wire up the router verbs the daemon stubs today.
- **Product polish:** settings UI, onboarding + FDA flow, launchd install/upgrade, signing +
  notarization + DMG. (Needed before any real distribution.)

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
# --reconcile-interval <secs> overrides the 24h default; --pretend-battery forces the
# on-battery extraction policy (defers the >7d-old backlog):
./build/daemon/macassistd --socket /tmp/dev.sock --db /tmp/dev.db --root /path/to/corpus \
    --reconcile-interval 2 [--pretend-battery]
./build/tools/mactl/mactl --socket /tmp/dev.sock status   # watch pending -> 0
./build/tools/mactl/mactl --socket /tmp/dev.sock query resume
```

**App dev loop:** after changing app code, `./scripts/make-app.sh` + `pkill -x MacAssist` + `open`
— `swift build` alone won't update the menu-bar bundle, and `open` just reactivates a stale running
instance (same bundle id).

---

## Known caveats

### Open in Editor
- **Settings are reachable only from the menu-bar "Default Editor" submenu.** If the menu-bar icon
  is hidden, set the editor directly, e.g. `defaults write com.finn.macassist editor.selectedId
  <id>` (`vim`/`nvim`/`vscode`/`zed`/…) and the terminal via `editor.terminalHost`
  (`terminal`/`iterm`/`ghostty`/`wezterm`/`kitty`).
- **Uninstalled GUI editors and terminal hosts are hidden; terminal *editors* are always listed.**
  A GUI editor / terminal host is offered only if a bundle id resolves via LaunchServices (the
  active selection stays visible even if uninstalled). Terminal editors can't be reliably probed
  from an `LSUIElement` app (restricted PATH), so they're always shown — detecting those is a
  follow-on.
- **AppleScript terminal hosts need Automation (Apple Events) permission.** A terminal editor
  launched in **Terminal.app or iTerm** triggers a TCC prompt on first ⌘E; on denial the panel shows
  a "Privacy › Automation" hint and nothing opens. The CLI hosts (Ghostty/WezTerm/kitty) launch via
  `open` and need **no** Automation permission.
- **Config/markup (`json`/`yaml`/`xml`/`html`/`css`/…) are `code`, not `document`** — by design, so
  they appear under the **Code** filter with the code icon, not under **Documents**.
- **⌘E eligibility is text-only:** `kind==code` or a plain-text extension
  (`txt/md/markdown/text/log/csv/tsv`). A non-plain-text `document` (`pdf`/`docx`) is not eligible.

### Indexing / search coverage
- **Folders are indexed only under Desktop/Downloads/Documents** (schema v7). The other three
  default roots (Movies/Music/Pictures) index files only. A `.pages` (and other bundles:
  `.app/.key/.numbers/.photoslibrary/.rtfd`) on a folder root is **one `folder` row** (not
  `document`), internals not indexed. On non-folder roots, bundles keep today's descend-and-index
  behavior.
- **Zips are the `.zip` file only — contents are not indexed.** Classified `archive`. Other archive
  formats (`.tar/.gz/.dmg/.rar/.7z`) are **not** classified as `archive` yet (trivial in
  `file_kind.cpp`).
- **Office docs (`docx`/`doc`/`pptx`/`xlsx`/`pages`/`key`), `rtf` are name-only.** Crawled and
  findable by **filename + folder path**, but **body text is NOT extracted** (no bridge yet) —
  stamped `needs_extract=0` so they don't loop. Real content extraction is **Phase 2.5**.
- **OCR deferred to Phase 2.5** — scanned/image-only PDFs index with empty `content`
  (name-searchable only).
- **Extraction caps (PLAN §3):** text/PDF bodies 100 KB; PDFs 200 pages; media metadata a curated
  subset.
- **"Recently downloaded" rides on the quarantine xattr in practice.** `files.downloaded_at` (schema
  v6) resolves from `com.apple.quarantine` (fast path) → `kMDItemDownloadedDate` (fallback).
  Verified on real data (2026-10-14): **144/149 `~/Downloads` files carried the quarantine xattr**
  and it persisted, while `kMDItemDownloadedDate` was populated for ~zero. Effective coverage is
  "files that still have a quarantine xattr" (~97% here), not the universal "Spotlight source of
  truth" the plan assumed — a file with neither signal is not flagged as a download.

### Extraction engine — things to remember
- **`pending` is the durable truth; `extractedFiles` is not.** `extractedFiles` is an in-memory
  counter that resets to 0 on daemon restart. To know if extraction is complete, watch `pending → 0`
  in `mactl status`.
- **Schema bump = nuke-and-rebuild.** Any `kSchemaVersion` change (pre-release policy) drops all
  tables and re-crawls + re-extracts. Currently **v7**. This is how Phase 2.5 parsers will
  re-trigger extraction on existing files.
- **On battery, the old backlog is deferred** (`indexed_at ≥ now−7d`); freshly-touched/created files
  still extract. Use `--pretend-battery` to force the path on battery-only hardware.
- **No per-file extraction timeout** (single extractor thread) — a pathological PDF could stall the
  backlog. Page/size caps are the only mitigation; subprocess isolation is a later hardening.
- **Fingerprint is `mtime(sec):size`** — a same-size edit within the same wall-clock second as the
  prior index isn't detected until mtime ticks. Coarse by design; real edits virtually always change
  size or cross a second boundary.
- **`CoreGraphics PDF has logged an error`** in the daemon log is harmless noise from PDFKit parsing
  a malformed PDF; our code treats it as empty content and moves on.

### Dev workflow gotchas
- **Quit the old app before relaunching** (see the App dev loop note above).
- **Restarting the daemon does NOT require a re-crawl unless the schema changed.** Query-side changes
  just need a rebuild + restart; the index persists (WAL files alongside `index.db`).
- **FTS5 `MATCH` rejects a table alias** — use the table name or a derived table (fails to prepare →
  silent empty results).
- **Socket path ≤ ~104 chars** (`sockaddr_un` limit) — dev sockets must live in `/tmp`.
- **Menu-bar icon can hide behind the notch** on crowded menu bars; ⌘-drag to pull it out.

### Not yet built
- **No launchd agent yet** — the daemon is launched manually (Polish phase).
- **`action`/`reindex`/`config` IPC** not implemented (router handles `ping`/`status`/`query`).
