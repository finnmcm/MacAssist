# MacAssist

A smarter Finder for macOS. MacAssist keeps a warm, content-aware index of your
files and answers natural-language-ish queries — *"my resume"*, *"drake mp4s"*,
*"open my_photo.png"* — from a Spotlight-style hotkey panel, in well under a
second.

A background daemon (C++) crawls your folders, extracts text and media
metadata, and keeps an FTS5 index hot; a tiny Swift menu-bar app shows the
panel and talks to the daemon over a local socket. See
[`PLAN.md`](PLAN.md) for the full design.

> **Status:** v0.1 — functional, Apple Silicon only. Distributed ad-hoc-signed
> (not notarized), so installing takes one extra command (below).

---

## Features

- **Instant search** over filenames, paths, and extracted content (PDF /
  plain-text bodies, image EXIF, audio/video tags), ranked so an exact filename
  match wins.
- **Filter toolbar** — narrow by Photos · Videos · Music · Documents · Code ·
  Finder tags · recently-downloaded.
- **Per-result actions:** `Enter` reveal in Finder · `⌘Enter` open a folder of
  all hits · `⌘C` copy path · `⌘E` open in your editor · `⌘L` symlink to Desktop.
- **Open in Editor** — terminal editors (vim, …) in your chosen terminal, or GUI
  editors (VS Code, Cursor, Zed, …). Configured from the menu-bar **Default
  Editor** submenu. See [`EDITOR_PLAN.md`](EDITOR_PLAN.md).
- **Stays out of the way** — menu-bar resident, no Dock icon; the daemon runs in
  the background and keeps the index current via FSEvents + a nightly
  reconciliation pass.

## Requirements

- Apple Silicon Mac (M-series).
- macOS 14 or later.

## Install

1. Download `MacAssist-<version>.dmg` (or `.zip`) from the
   [**Releases**](https://github.com/finnmcm/MacAssist/releases) page.
2. From the `.dmg`, drag **MacAssist** into **Applications**. (From the `.zip`,
   unzip and move `MacAssist.app` to `/Applications`.)
3. Clear the download quarantine once — MacAssist is ad-hoc-signed, not
   notarized, so Gatekeeper otherwise blocks it:
   ```sh
   xattr -dr com.apple.quarantine /Applications/MacAssist.app
   ```
4. Launch **MacAssist**. It installs its background daemon as a per-user
   LaunchAgent automatically and starts indexing. A magnifying-glass icon
   appears in the menu bar.
5. **(Recommended) Grant Full Disk Access** so it can index everything:
   System Settings → Privacy & Security → **Full Disk Access** → add
   `macassistd` (inside `MacAssist.app/Contents/Resources/`). Without it,
   MacAssist still indexes folders macOS doesn't protect.

Then press **⌥Space** (Option-Space) anywhere to summon the panel. `Esc`
dismisses it.

## Uninstall

```sh
launchctl bootout "gui/$(id -u)/com.finn.macassist.daemon"
rm -f ~/Library/LaunchAgents/com.finn.macassist.daemon.plist
rm -rf ~/Library/Application\ Support/MacAssist ~/Library/Logs/MacAssist
rm -rf /Applications/MacAssist.app
```

## Build from source

Requires the Xcode Command Line Tools (Swift) and CMake.

```sh
./scripts/build.sh                 # daemon + mactl + app (debug)
./scripts/make-release.sh [0.1]    # signed MacAssist.app + .zip + .dmg → dist/
```

The daemon and `mactl` CLI build with CMake (C++20); the Swift app builds with
SwiftPM. They meet only over the Unix-domain-socket protocol
([`PLAN.md` §6](PLAN.md)). Tests: `./scripts/test.sh` (Catch2 + CTest).

> On a Mac where `cmake` is an x86_64 (Rosetta) build, plain CMake would emit an
> x86_64 daemon; `make-release.sh` forces `-DCMAKE_OSX_ARCHITECTURES=arm64` and
> pins `/usr/bin/codesign` so the shipped bundle is uniformly native.

## Architecture

```
MacAssist.app (Swift, menu-bar)  ──socket──▶  macassistd (C++ LaunchAgent)
  global hotkey, NSPanel,                       crawler + FSEvents + reconciler,
  results + actions                             PDF/text/media extraction,
                                                SQLite + FTS5 index, query engine
```

The full design, data model, sweep subsystem, and roadmap live in
[`PLAN.md`](PLAN.md); live status in [`PROGRESS.md`](PROGRESS.md).
