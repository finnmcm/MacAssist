# MacAssist — "Open in Editor" (developer support)

*Feature plan. Read alongside `PLAN.md` (authoritative design) and `PROGRESS.md` (status).
Follows the `TOOLBAR_PLAN.md` per-feature convention. Finalized 2026-10-19.*

Let developers open any code / text / Markdown result in the terminal or IDE of their
choice, straight from the search panel. Default editor is **vim** (in Terminal.app),
changeable from the menu bar.

---

## 1. Locked decisions

| Area | Decision |
|---|---|
| Editor support | **Both** terminal editors (vim/nvim/nano/emacs, launched in a chosen terminal host) **and** GUI editors (VS Code/Cursor/Zed/Sublime/Xcode, launched directly). |
| Default | **vim** running in **Terminal.app**. |
| Settings entry | Menu-bar magnifying-glass menu → new **"Default Editor ▸"** submenu. |
| Trigger | New shortcut **⌘E** opens the selected result in the editor. `Enter` stays reveal-in-Finder, `⌘Enter` stays show-all-in-Finder, `⌘C` stays copy-path. |
| Non-text selection | ⌘E is a no-op with subtle `status` feedback — it never tries to open an image/pdf/video in a text editor. |
| Classification | New first-class **`code`** kind: its own icon + a "Code" toolbar filter. |
| Persistence | App-side **UserDefaults**. The daemon is untouched by the editor preference. |
| Code content search ("grep") | **Out of scope for now.** Code files are **name-searchable only** in v1 — the `plain_text` extractor whitelist is unchanged. (Revisit as a later feature.) |
| Schema | **No bump.** Classifying code extensions is additive; the startup full crawl + nightly reconcile insert the previously-skipped code files via normal upsert. |

### Defaults chosen (changeable)
- Shortcut = **⌘E** (keyCode 14). Terminal host default = **Terminal.app** (iTerm offered if installed).
- **Editor-eligible** = `kind == code` **OR** extension ∈
  `{txt, md, markdown, text, log, csv, tsv}`.
  `rtf`/`pdf`/`docx`/binaries are excluded (not plain text). Config/markup extensions
  (`json`/`yaml`/`xml`/`html`/`css`/…) are covered by `kind == code` — see the next bullet.
- **Config/markup files are classified `code`, by intent** — `json yaml yml toml ini cfg conf
  xml html htm css scss less` resolve to `kind=code`, not `document`. Consequence: they get the
  code icon, appear under the **Code** toolbar filter (not **Documents**), and are ⌘E-eligible.
  This is deliberate for a developer workflow. No index regression: none of these extensions are
  in the `document` map today (they're currently *unclassified and skipped entirely*), so this
  adds them rather than moving them out of Documents.
- Terminal editors `cd` into the file's parent directory before launching, so `:e` on
  neighbors works.

---

## 2. Why this shape

Opening is already entirely **app-side** (PLAN §6: "open/reveal/copy done app-side") — the
daemon never opens anything. So the editor launch, the preference, and the menu UI all live
in the Swift app. The **only** daemon change is classification, and it exists purely so the
files can be *found* in the first place.

**The prerequisite finding:** `indexer.cpp:156` skips any file whose extension isn't in
`KindForExtension` (`if (kind.empty()) return Result::kSkipped;`). The kind map
(`file_kind.cpp`) has no source-code extensions, so **today `.py`/`.js`/`.swift`/… are not in
the index at all** — invisible to search. `.txt`/`.md` are already `document` and findable.
Classifying code is therefore mandatory for the feature, not cosmetic.

**vim is a terminal program.** Opening a file "in vim" means spawning a terminal emulator
running `vim <path>` — a different mechanism from GUI editors (`open -a`). Because the default
is vim, the launch engine must handle the terminal path, not just GUI apps.

---

## 3. Daemon changes (Brick 1)

**`daemon/src/index/file_kind.cpp`** — add a `code` block to the `KindForExtension` map:

```
py pyw js mjs cjs ts tsx jsx swift c h cc cpp cxx hpp hh m mm java kt kts
go rs rb php pl sh bash zsh fish sql html htm css scss less vue svelte
json yaml yml toml ini cfg conf xml lua r scala clj ex exs erl hs dart
gradle cmake proto graphql gql   →  "code"
```

Notes:
- `md`/`csv`/`txt` stay `document` (already indexed); they become editor-eligible via the
  app's text-extension set, so **no recategorization** of those.
- **Config/markup extensions** (`json yaml yml toml ini cfg conf xml html htm css scss less`)
  are intentionally included in the `code` block above (see §1). They are currently
  unclassified — and therefore skipped and unindexed — so this makes them findable for the
  first time under `kind=code`; it does **not** move anything out of the Documents filter.
- Extensionless dev files (`Makefile`, `Dockerfile`) stay out of scope — the indexer keys on
  extension. Basename special-casing is a possible later add.
- Additive only: no schema change. Previously-skipped code files are inserted on the next full
  crawl (every daemon startup) and by nightly reconcile.

**Tests** — `daemon/tests/file_kind_test.cpp`: `.py`/`.swift`/`.go`/`.ts` → `code`; an unknown
extension → empty.

---

## 4. App changes

### Brick 2 — settings model + persistence (`EditorSettings.swift`, new)
- `enum EditorKind { case terminal, gui }`.
- `struct EditorChoice { id, displayName, kind, launchTarget }`:
  - terminal choices carry the command (`vim`, `nvim`, …);
  - GUI choices carry an app name / bundle id, launched via `open -a` / `NSWorkspace`
    (dodges the PATH problem an `LSUIElement` app has with CLIs like `code`).
- Curated **catalog** — terminal: vim, nvim, nano, emacs · GUI: VS Code, Cursor, Zed,
  Sublime Text, Xcode, TextEdit. (A free-text "Custom…" entry was built and then removed — see
  §7; the catalog covers the common editors.)
- `terminalHost` preference (Terminal.app default; iTerm if installed).
- Backed by `UserDefaults`; default = vim / Terminal.app.

### Brick 3 — launch engine (`EditorLauncher.swift`, new)
- **GUI:** `NSWorkspace.shared.open([fileURL], withApplicationAt: appURL, …)` resolving the app
  by name/bundle id — reliable, no PATH dependency.
- **Terminal:** AppleScript via `osascript`. Terminal.app:
  `do script "cd <dir>; <cmd> <quoted path>"` then `activate`; iTerm variant. Careful
  shell-quoting of the path.
- **TCC:** the first terminal launch triggers an Automation (Apple Events) permission prompt;
  on denial, surface a clear `status` message rather than failing silently.
- **Editor not installed:** a GUI editor whose app can't be resolved (NSWorkspace returns no
  app / `open -a` fails), or a terminal command that isn't on PATH, must surface the same subtle
  `status` feedback ("Couldn't launch <editor>") and leave the panel up — never fail silently.
  The launch result is reported back to `SearchModel` so Brick 4 can show it.

### Brick 4 — action wiring + classification surfacing
- `SearchModel.openInEditor()`: guard eligibility (`EditorEligibility.isEditable(kind, path)`),
  call `EditorLauncher`, `onDismiss?()` on success; otherwise set a subtle `status`
  ("Not a text file" / "Allow automation in System Settings" / "Couldn't launch <editor>")
  and leave the panel up.
- `PanelController.handle`: add `case 14 where cmd:` (**⌘E**) → `model.openInEditor()`.
- `main.swift setUpMenuBar()`: add a **"Default Editor ▸"** submenu — catalog with a checkmark
  on the current choice and a "Terminal app ▸" subitem (host picker, enabled only when a terminal
  editor is selected).
- `SearchView` + `Protocol`: `ResultRow.icon` gains
  `case "code": "chevron.left.forwardslash.chevron.right"`; add `toolbarButton("Code",
  .kind("code"))`; `FilterLabels.kindLabel` maps `code → "Code"`. No protocol change — `kind`
  already travels on `FileHit`.

---

## 5. Testing & verification

- **Daemon:** Catch2 classification cases (Brick 1); `./scripts/test.sh`.
- **App (manual, per repo convention):** rebuild daemon + restart →
  - `.py`/`.swift` now appear in `mactl query`;
  - in the panel, **⌘E** on a code / `.txt` / `.md` file opens it in vim/Terminal (and in a
    GUI editor after switching the default);
  - **⌘E** on an image/pdf shows the "not a text file" feedback;
  - the **Code** filter returns only code results;
  - `Enter` still reveals, `⌘Enter` still opens the folder.

---

## 6. Brick order

1. **Daemon classification** (+ tests) — unblocks everything; code becomes searchable at once.
2. **Settings model + menu UI** ("Default Editor ▸").
3. **Launch engine** (GUI first, then terminal/AppleScript).
4. **Action wiring** (⌘E) + Code filter/icon.
5. **Tests + manual verification checklist.**

---

## 7. Out of scope (candidate follow-ons)

- **Configurable terminal type.** Support non-standard terminals (Alacritty / kitty / WezTerm /
  Ghostty / …) via a user-supplied terminal command or launch template, instead of only the
  built-in Terminal.app / iTerm hosts.
- **Hide uninstalled *terminal* editors.** GUI editors are already hidden when not installed
  (bundle-id / LaunchServices resolution; the active selection stays visible regardless).
  Terminal commands are harder to detect from the app — an `LSUIElement` process has a restricted
  PATH and the command only needs to exist inside the terminal's own shell — so vim/nvim/nano/
  emacs are still always shown; a best-effort probe (e.g. the user's login shell) is the open
  piece.
- **Grep / code content search** — whitelist code extensions in the `plain_text` extractor so
  code bodies enter FTS. Deliberately deferred.
- Extensionless dev files (`Makefile`, `Dockerfile`) via basename classification.
- A full Settings window (Polish phase) — this feature stays in the menu bar for now.
- **Custom editor command (removed).** A free-text "Custom…" entry (command + "runs in
  terminal" toggle) was implemented, then removed: GUI custom was confusing — `open -a` resolves
  app *names*, not CLIs, so a dev typing `code` as a GUI target got "couldn't launch". The
  curated catalog covers the common editors; revisit only if a clear need returns (and, if so,
  resolve GUI CLIs via the login shell's PATH rather than `open -a`).
